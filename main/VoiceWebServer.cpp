#include "VoiceWebServer.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <string>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "PcmPlayer.h"
#include "Settings.h"
#include "TtsClient.h"
#include "WifiManager.h"

namespace {

constexpr const char* TAG = "voiceweb";

// /config bodies are tiny — bound the input so a hostile Content-Length can't
// trigger a huge std::string allocation. /firmware has its own bound (the OTA
// partition size).
constexpr size_t kMaxJsonBodyBytes = 4 * 1024;

std::string read_request_body(httpd_req_t* req) {
    std::string body;
    body.reserve(req->content_len);
    char buf[256];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, std::min<int>(remaining, static_cast<int>(sizeof(buf))));
        if (got <= 0) break;
        body.append(buf, got);
        remaining -= got;
    }
    return body;
}

esp_err_t send_json(httpd_req_t* req, const JsonWrapper& json) {
    httpd_resp_set_type(req, "application/json");
    std::string out = json.ToString();
    return httpd_resp_sendstr(req, out.c_str());
}

}  // namespace

VoiceWebServer::VoiceWebServer(WebContext* ctx, Settings& settings, TtsClient& tts, PcmPlayer& player)
    : WebServer(ctx), settings_(settings), tts_(tts), player_(player) {}

esp_err_t VoiceWebServer::start() {
    esp_err_t r = WebServer::start();
    if (r != ESP_OK) return r;

    struct Route {
        const char*    uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    };
    const std::array<Route, 9> routes = {{
        {"/firmware",     HTTP_POST, firmware_post_handler},
        {"/firmware",     HTTP_GET,  firmware_get_handler},
        {"/config",       HTTP_GET,  config_get_handler},
        {"/config",       HTTP_POST, config_post_handler},
        {"/config/reset", HTTP_POST, config_reset_post_handler},
        {"/say",          HTTP_POST, say_post_handler},
        {"/play",         HTTP_POST, play_post_handler},
        {"/volume",       HTTP_GET,  volume_get_handler},
        {"/volume",       HTTP_POST, volume_post_handler},
    }};

    for (const Route& route : routes) {
        httpd_uri_t uri = {
            .uri      = route.uri,
            .method   = route.method,
            .handler  = route.handler,
            .user_ctx = this,
        };
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s %s: %s",
                route.method == HTTP_POST ? "POST" : "GET", route.uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

void VoiceWebServer::populate_healthz_fields(WebContext*, JsonWrapper& json) {
    const esp_app_desc_t*  desc    = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    json.AddItem("version",   std::string(desc->version));
    json.AddItem("partition", std::string(running->label));
    json.AddItem("heap_free", static_cast<int>(esp_get_free_heap_size()));
}

// POST /firmware — raw .bin body. Streams into the inactive OTA slot, sets it as
// the next boot partition, then reboots.
// Deploy: curl --data-binary @build/ws_voice.bin http://<host>/firmware
esp_err_t VoiceWebServer::firmware_post_handler(httpd_req_t* req) {
    if (req->content_len <= 0) return sendJsonError(req, 400, "Content-Length required");

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) return sendJsonError(req, 500, "no OTA partition available");
    if (req->content_len > static_cast<int>(target->size)) {
        return sendJsonError(req, 413, "image larger than OTA partition");
    }
    ESP_LOGI(TAG, "OTA: writing %d bytes to %s @ 0x%" PRIx32,
        req->content_len, target->label, target->address);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

    char buf[1024];
    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, std::min<int>(remaining, static_cast<int>(sizeof(buf))));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            esp_ota_abort(handle);
            return sendJsonError(req, 400, "request body truncated");
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            return sendJsonError(req, 500, esp_err_to_name(err));
        }
        written   += got;
        remaining -= got;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) return sendJsonError(req, 400, esp_err_to_name(err));
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

    ESP_LOGW(TAG, "OTA: %d bytes written to %s; rebooting", written, target->label);
    JsonWrapper resp;
    resp.AddItem("status",    std::string("ok"));
    resp.AddItem("written",   written);
    resp.AddItem("partition", std::string(target->label));
    send_json(req, resp);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t VoiceWebServer::firmware_get_handler(httpd_req_t* req) {
    const esp_app_desc_t*  desc    = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    JsonWrapper resp;
    resp.AddItem("version",   std::string(desc->version));
    resp.AddItem("idf_ver",   std::string(desc->idf_ver));
    resp.AddItem("date",      std::string(desc->date));
    resp.AddItem("time",      std::string(desc->time));
    resp.AddItem("partition", std::string(running->label));
    return send_json(req, resp);
}

esp_err_t VoiceWebServer::config_get_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    JsonWrapper resp = self->settings_.toJson();
    return send_json(req, resp);
}

// POST /config — apply any subset of the settings keys, persist, return the new
// full settings. STT params take effect on the next capture; sensor_name/mqtt/tz
// changes take effect on reboot.
esp_err_t VoiceWebServer::config_post_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    if (req->content_len > kMaxJsonBodyBytes) return sendJsonError(req, 413, "request body too large");
    std::string body = read_request_body(req);
    if (body.empty()) return sendJsonError(req, 400, "empty body");
    JsonWrapper json = JsonWrapper::Parse(body);
    if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");

    self->settings_.loadFromJson(json);
    self->settings_.save();
    self->settings_.log();

    JsonWrapper resp = self->settings_.toJson();
    return send_json(req, resp);
}

// POST /config/reset — restore every setting to its default and persist.
// Optional body {"wifi": true} also clears Wi-Fi credentials, rebooting into
// provisioning (ESP-Touch v2).
esp_err_t VoiceWebServer::config_reset_post_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);

    bool wipe_wifi = false;
    if (req->content_len > 0) {
        if (req->content_len > kMaxJsonBodyBytes) return sendJsonError(req, 413, "request body too large");
        std::string body = read_request_body(req);
        if (!body.empty()) {
            JsonWrapper json = JsonWrapper::Parse(body);
            if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");
            json.GetField("wifi", wipe_wifi);
        }
    }

    self->settings_.resetToDefaults();
    self->settings_.save();
    self->settings_.log();
    ESP_LOGW(TAG, "config reset to defaults (wipe_wifi=%d)", wipe_wifi);

    JsonWrapper resp;
    resp.AddItem("status",       std::string(wipe_wifi ? "reset+wifi_clear+reboot" : "reset"));
    resp.AddItem("wifi_cleared", wipe_wifi);
    esp_err_t r = send_json(req, resp);

    if (wipe_wifi && self->webContext && self->webContext->wifiManager) {
        ESP_LOGW(TAG, "clearing wifi credentials and rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        self->webContext->wifiManager->clear();
    }
    return r;
}

// POST /say — {"text":"...", "voice":"..."} (voice optional, falls back to
// Settings::ttsVoice). Enqueues onto the TTS worker and returns immediately;
// mirrors the "say" MQTT command.
esp_err_t VoiceWebServer::say_post_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    if (req->content_len > kMaxJsonBodyBytes) return sendJsonError(req, 413, "request body too large");
    std::string body = read_request_body(req);
    if (body.empty()) return sendJsonError(req, 400, "empty body");
    JsonWrapper json = JsonWrapper::Parse(body);
    if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");

    std::string text, voice;
    if (!json.GetField("text", text) || text.empty()) {
        return sendJsonError(req, 400, "missing 'text'");
    }
    json.GetField("voice", voice);

    bool queued = self->tts_.speak(text, voice);
    JsonWrapper resp;
    resp.AddItem("status", std::string(queued ? "queued" : "dropped_queue_full"));
    return send_json(req, resp);
}

// POST /play — {"url":"..."} (url optional, falls back to Settings::playUrl).
// Fetches a raw-PCM clip (16 kHz mono s16le) and plays it out the speaker.
// Enqueues onto the player worker and returns immediately; mirrors the "play"
// MQTT command. Deliberately lenient about the body: no body, `{}` and a
// parse failure all play the default clip rather than 400 — JsonWrapper's
// Empty() can't tell an empty object from invalid JSON, and for a cue
// trigger "just play something" beats rejecting the request.
esp_err_t VoiceWebServer::play_post_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    if (req->content_len > kMaxJsonBodyBytes) return sendJsonError(req, 413, "request body too large");

    std::string url;
    if (req->content_len > 0) {
        JsonWrapper json = JsonWrapper::Parse(read_request_body(req));
        json.GetField("url", url);
    }

    bool queued = self->player_.play(url);
    JsonWrapper resp;
    resp.AddItem("status", std::string(queued ? "queued" : "dropped_queue_full"));
    return send_json(req, resp);
}

esp_err_t VoiceWebServer::volume_get_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    JsonWrapper resp;
    resp.AddItem("volume", self->settings_.speakerVolume);
    return send_json(req, resp);
}

// POST /volume — {"volume": 0-100}. A thin wrapper around the same
// loadFromJson()/onChange machinery as /config, just under a shorter,
// dedicated key/path for a value that's tweaked often and applies live.
esp_err_t VoiceWebServer::volume_post_handler(httpd_req_t* req) {
    VoiceWebServer* self = static_cast<VoiceWebServer*>(req->user_ctx);
    if (req->content_len > kMaxJsonBodyBytes) return sendJsonError(req, 413, "request body too large");
    std::string body = read_request_body(req);
    if (body.empty()) return sendJsonError(req, 400, "empty body");
    JsonWrapper json = JsonWrapper::Parse(body);
    if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");

    int vol = 0;
    if (!json.GetField("volume", vol)) return sendJsonError(req, 400, "missing 'volume'");

    JsonWrapper doc;
    doc.AddItem("speaker_volume", vol);
    self->settings_.loadFromJson(doc);
    self->settings_.save();

    JsonWrapper resp;
    resp.AddItem("volume", self->settings_.speakerVolume);
    return send_json(req, resp);
}
