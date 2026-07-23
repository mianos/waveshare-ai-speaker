// ws-voice — Waveshare ESP32-S3 audio board voice → MQTT bridge.
//
// On the "Hi ESP" wakeword (on-device esp-sr WakeNet) it records the following
// speech (AFE-enhanced 16 kHz mono, ended by VAD silence or a max cap), POSTs
// the audio to a configurable Whisper-style STT server, and publishes the
// transcript to MQTT for Node-RED to act on. Shares its infrastructure
// components (wifimanager, mqttwrapper, settingsbase, webserver, jsonwrapper)
// with doorbell3 / ldr3 / atomecho.
//
// Optionally (play_tone, default on) plays a short chime out the ES8311 speaker
// at boot, on first Wi-Fi connect, and a blip on each wakeword. The speaker DAC
// is brought up separately from the mic and is non-fatal, so it can never stall
// boot. The on-board RGB LED lights green while a wakeword is being captured.
//
// MQTT (cmnd/<name>/...):
//   settings    any subset of the /config JSON (e.g. {"stt_url": "..."})
//   restart     {}
//   reprovision {}   clears Wi-Fi creds, reboots into ESP-Touch v2 provisioning
// Publishes:
//   tele/<name>/wake      {"event":"wake"}                on each wakeword
//   tele/<name>/stt       {"text":...,"ms":...,"stt_ms":...}  on transcription
//   tele/<name>/stterror  {...}                           on STT failure
//   tele/<name>/init,status                               identity + telemetry

#include <string>
#include <regex>
#include <cstdlib>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "NvsStorageManager.h"
#include "WifiManager.h"
#include "Settings.h"
#include "MqttClient.h"
#include "WebServer.h"
#include "VoiceWebServer.h"
#include "JsonWrapper.h"

#include "board.h"
#include "SttClient.h"
#include "VoicePipeline.h"
#include "ToneGen.h"

static const char* TAG = "wsvoice";

namespace {

// Signalled once on the first STA IP; drives the "connected" tone.
SemaphoreHandle_t s_connected = nullptr;

// Render a short chime (note sequence) and play it out the speaker. Best-effort:
// bsp_audio_play_mono16 no-ops if the ES8311 was not brought up. Called only
// from task contexts (app_main / a dedicated task), never a Wi-Fi callback.
void playChime(const tonegen::Note* notes, size_t nnotes) {
    constexpr uint32_t rate = 16000;
    const size_t n = tonegen::chimeSamples(notes, nnotes, rate);
    int16_t* buf = (int16_t*)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "chime alloc failed (%zu samples)", n);
        return;
    }
    tonegen::renderChime(buf, notes, nnotes, rate, tonegen::kDefaultAmplitude, /*fadeMs=*/5);
    bsp_audio_play_mono16(buf, n);
    heap_caps_free(buf);
}

// Rising two-note "power on" cue.
constexpr tonegen::Note kStartChime[] = {{587.0f, 120}, {880.0f, 150}};
// Rising three-note "connected" cue — distinct from the start chime.
constexpr tonegen::Note kConnectChime[] = {{880.0f, 90}, {1174.0f, 90}, {1568.0f, 140}};

// Waits for the first IP, then plays the connected chime once and exits.
void connectChimeTask(void*) {
    if (xSemaphoreTake(s_connected, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "playing connected tone");
        playChime(kConnectChime, sizeof(kConnectChime) / sizeof(kConnectChime[0]));
    }
    vTaskDelete(nullptr);
}

struct App {
    Settings*    settings;
    MqttClient*  mqtt;
    WiFiManager* wifi;
};

std::string uptimeString() {
    uint32_t seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t days = seconds / 86400; seconds %= 86400;
    uint32_t hours = seconds / 3600; seconds %= 3600;
    uint32_t minutes = seconds / 60;
    return std::to_string(days) + "d " + std::to_string(hours) + "h " +
           std::to_string(minutes) + "m";
}

std::string localIp() {
    char buf[16] = "0.0.0.0";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, buf, sizeof(buf));
    }
    return std::string(buf);
}

// ---- MQTT command handlers (cmnd/<name>/<cmd>) ----

esp_err_t handleSettings(MqttClient*, const std::string&, const JsonWrapper& d, void* ctx) {
    auto* app = static_cast<App*>(ctx);
    app->settings->loadFromJson(d);
    app->settings->save();
    app->settings->log();
    return ESP_OK;
}

esp_err_t handleRestart(MqttClient*, const std::string&, const JsonWrapper&, void*) {
    ESP_LOGW(TAG, "restart requested");
    esp_restart();
    return ESP_OK;
}

esp_err_t handleReprovision(MqttClient*, const std::string&, const JsonWrapper&, void* ctx) {
    ESP_LOGW(TAG, "reprovision requested");
    static_cast<App*>(ctx)->wifi->clear();  // clears Wi-Fi creds and restarts
    return ESP_OK;
}

// --- OTA rollback verification (see atomecho/ldr3 for the rationale) ---
constexpr int OTA_VERIFY_TIMEOUT_MS = 120000;
SemaphoreHandle_t s_got_ip = nullptr;

void onGotIp(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_got_ip)    xSemaphoreGive(s_got_ip);
        if (s_connected) xSemaphoreGive(s_connected);  // fires the connected tone
    }
}

void otaVerifyTask(void*) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "OTA: image pending verify; waiting up to %ds for connectivity",
                 OTA_VERIFY_TIMEOUT_MS / 1000);
        if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(OTA_VERIFY_TIMEOUT_MS)) == pdTRUE) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA: connectivity confirmed, image marked valid");
        } else {
            ESP_LOGE(TAG, "OTA: no IP within timeout; rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            ESP_LOGE(TAG, "OTA: rollback not possible; keeping current image");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    vTaskDelete(nullptr);
}

void telemetryTask(void* arg) {
    auto* app = static_cast<App*>(arg);
    const std::string base = "tele/" + app->settings->sensorName + "/";

    for (int i = 0; i < 20 && time(nullptr) < 1700000000; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    const esp_app_desc_t* desc = esp_app_get_description();
    JsonWrapper init;
    init.AddItem("version", 1);
    init.AddItem("build", std::string(desc->version));
    init.AddItem("built", std::string(desc->date) + " " + std::string(desc->time));
    init.AddTime();
    init.AddItem("hostname", app->settings->sensorName);
    init.AddItem("ip", localIp());
    app->mqtt->publish(base + "init", init.ToString());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        JsonWrapper d;
        d.AddTime();
        d.AddItem("uptime", uptimeString());
        d.AddItem("heap_free", (int)esp_get_free_heap_size());
        d.AddItem("psram_free", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        app->mqtt->publish(base + "status", d.ToString());
    }
}

}  // namespace

extern "C" void app_main(void) {
    static NvsStorageManager nvs;      // constructing this initialises NVS flash
    static Settings settings(nvs);
    settings.log();

    s_got_ip = xSemaphoreCreateBinary();
    s_connected = xSemaphoreCreateBinary();  // created before Wi-Fi can signal it

    // Wi-Fi: ESP-Touch v2 provisioning on first boot, else reconnect. onGotIp
    // feeds OTA rollback verification.
    static WiFiManager wifi(nvs, onGotIp, nullptr);
    std::string host = settings.sensorName;
    wifi.configSetHostName(host);

    // AFE runs continuously on both cores; a radio doze between DTIM beacons
    // would also add latency to the STT upload. Keep the radio awake.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    setenv("TZ", settings.tz.c_str(), 1);
    tzset();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);

    // MQTT (anonymous, plain mqtt://, Last-Will on the status topic).
    static std::string uri = "mqtt://" + settings.mqttServer + ":" + std::to_string(settings.mqttPort);
    static std::string statusTopic = "tele/" + settings.sensorName + "/status";
    static std::string lwt = "{\"status\":\"offline\"}";
    static esp_mqtt_client_config_t mcfg = {};
    mcfg.broker.address.uri        = uri.c_str();
    mcfg.credentials.client_id     = settings.sensorName.c_str();
    mcfg.session.last_will.topic   = statusTopic.c_str();
    mcfg.session.last_will.msg     = lwt.c_str();
    mcfg.session.last_will.msg_len = (int)lwt.size();
    mcfg.session.last_will.qos     = 1;
    static MqttClient mqtt(mcfg, settings.sensorName);

    static App app{ &settings, &mqtt, &wifi };

    const std::string b = "cmnd/" + settings.sensorName + "/";
    mqtt.registerHandler(b + "settings",    std::regex(b + "settings"),    handleSettings,    &app);
    mqtt.registerHandler(b + "restart",     std::regex(b + "restart"),     handleRestart,     &app);
    mqtt.registerHandler(b + "reprovision", std::regex(b + "reprovision"), handleReprovision, &app);
    mqtt.start();

    // Audio + voice: board mic path, then the STT uploader, then the esp-sr
    // wake/capture pipeline that feeds it.
    ESP_ERROR_CHECK(bsp_board_init());

    // On-board RGB LED — independent of the speaker, best-effort (non-fatal).
    // Flashed green by the voice pipeline while a wakeword is being captured.
    bsp_led_init();

    // Speaker tones (opt-in; see Settings::playTone). Brought up AFTER the mic
    // ADC and BEFORE the feed task starts, and deliberately NOT ESP_ERROR_CHECK'd
    // — a DAC failure must never take the mic/MQTT path down. Play the start tone
    // here (I2S idle) and arm the connected tone for the first IP.
    if (settings.playTone) {
        esp_err_t ar = bsp_audio_out_init();
        ESP_LOGI(TAG, "play_tone enabled; audio out init: %s", esp_err_to_name(ar));
        if (ar == ESP_OK) {
            ESP_LOGI(TAG, "playing start tone");
            playChime(kStartChime, sizeof(kStartChime) / sizeof(kStartChime[0]));
            xTaskCreate(connectChimeTask, "connchime", 4096, nullptr, 4, nullptr);
        }
    } else {
        ESP_LOGI(TAG, "play_tone disabled (speaker tones off)");
    }

    static SttClient stt(settings, mqtt);
    stt.start();
    static VoicePipeline voice(settings, mqtt, stt);
    voice.start();

    // Web server: /healthz, /reset, /set_hostname plus /firmware, /config,
    // /config/reset.
    static WebContext webctx(&wifi);
    static VoiceWebServer web(&webctx, settings);
    web.start();

    xTaskCreate(otaVerifyTask, "ota_verify", 4096, nullptr, 4, nullptr);
    xTaskCreate(telemetryTask, "telemetry", 4096, &app, 4, nullptr);

    ESP_LOGI(TAG, "ws-voice started");
}
