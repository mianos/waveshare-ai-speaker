#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include "JsonWrapper.h"
#include "WebServer.h"

struct Settings;
class TtsClient;

// ws-voice HTTP control surface, layered on the shared WebServer base
// (/reset, /set_hostname, /healthz). Adds:
//   POST /firmware       raw .bin body -> inactive OTA slot -> reboot
//   GET  /firmware       running image version / partition
//   GET  /config         current settings as JSON
//   POST /config         apply + persist a subset of settings
//   POST /config/reset   restore settings to defaults (optional wifi wipe)
//   POST /say            {"text":"...", "voice":"..."} -> speak via TTS
//   GET  /volume         current speaker_volume as JSON
//   POST /volume         {"volume": 0-100} -> speaker_volume, applies live
// Handlers recover this instance from req->user_ctx.
class VoiceWebServer : public WebServer {
public:
    VoiceWebServer(WebContext* ctx, Settings& settings, TtsClient& tts);

    esp_err_t start() override;

protected:
    void populate_healthz_fields(WebContext* ctx, JsonWrapper& json) override;

private:
    static esp_err_t firmware_post_handler(httpd_req_t* req);
    static esp_err_t firmware_get_handler(httpd_req_t* req);
    static esp_err_t config_get_handler(httpd_req_t* req);
    static esp_err_t config_post_handler(httpd_req_t* req);
    static esp_err_t config_reset_post_handler(httpd_req_t* req);
    static esp_err_t say_post_handler(httpd_req_t* req);
    static esp_err_t volume_get_handler(httpd_req_t* req);
    static esp_err_t volume_post_handler(httpd_req_t* req);

    Settings&  settings_;
    TtsClient& tts_;
};
