#include "TtsClient.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "board.h"
#include "JsonWrapper.h"
#include "MqttClient.h"
#include "Settings.h"
#include "TtsRequest.h"

namespace {
constexpr const char* TAG = "tts";

constexpr int    kHttpTimeoutMs = 20000;  // synth + resample can take a few seconds
constexpr size_t kReadChunk     = 2048;
// Cap a single utterance's PCM: 15 s @ 16 kHz mono 16-bit. Ample for a spoken
// prompt; a longer response is played up to this and logged as truncated
// rather than growing the buffer unbounded.
constexpr size_t kMaxPcmSamples = 16000 * 15;
constexpr size_t kMaxPcmBytes   = kMaxPcmSamples * sizeof(int16_t);
constexpr int    kQueueDepth    = 2;

bool writeAll(esp_http_client_handle_t client, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = esp_http_client_write(client, data + off, len - off);
        if (n < 0) return false;
        if (n == 0) break;  // connection closed by peer
        off += (size_t)n;
    }
    return off == len;
}
}  // namespace

TtsClient::TtsClient(Settings& settings, MqttClient& mqtt)
    : settings_(settings), mqtt_(mqtt) {}

void TtsClient::start() {
    queue_ = xQueueCreate(kQueueDepth, sizeof(TtsJob));
    if (!queue_) {
        ESP_LOGE(TAG, "failed to create job queue");
        return;
    }
    // 6 KB stack: HTTP client + cJSON, same as SttClient. Runs at the same
    // priority; TTS and STT never fire on the same utterance so contention is
    // not a concern.
    xTaskCreate(workerTrampoline, "tts", 6144, this, 4, nullptr);
}

bool TtsClient::speak(const std::string& text, const std::string& voice) {
    if (text.empty()) {
        ESP_LOGW(TAG, "speak() called with empty text");
        return false;
    }
    TtsJob job{};
    std::snprintf(job.text, sizeof(job.text), "%s", text.c_str());
    std::snprintf(job.voice, sizeof(job.voice), "%s", voice.c_str());
    if (!queue_ || xQueueSend(queue_, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "job queue full, dropping \"%.40s\"", text.c_str());
        return false;
    }
    return true;
}

void TtsClient::workerTrampoline(void* arg) {
    static_cast<TtsClient*>(arg)->worker();
}

void TtsClient::worker() {
    for (;;) {
        TtsJob job;
        if (xQueueReceive(queue_, &job, portMAX_DELAY) == pdTRUE) {
            process(job);
        }
    }
}

void TtsClient::process(const TtsJob& job) {
    const std::string base  = "tele/" + settings_.sensorName + "/";
    const std::string voice = job.voice[0] ? std::string(job.voice) : settings_.ttsVoice;

    // Fail-fast: nothing to POST to yet.
    if (settings_.ttsUrl.empty()) {
        ESP_LOGW(TAG, "tts_url not configured; \"%s\" dropped. "
                      "Set it via cmnd/%s/settings or POST /config.",
                 job.text, settings_.sensorName.c_str());
        JsonWrapper err;
        err.AddItem("error", std::string("tts_url_unset"));
        err.AddTime();
        mqtt_.publish(base + "ttserror", err.ToString());
        return;
    }

    const std::string body = tts::requestBody(job.text, voice);

    esp_http_client_config_t cfg = {};
    cfg.url        = settings_.ttsUrl.c_str();
    cfg.method     = HTTP_METHOD_POST;
    cfg.timeout_ms = kHttpTimeoutMs;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    const int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(client, (int)body.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        JsonWrapper e; e.AddItem("error", std::string(esp_err_to_name(err))); e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }
    if (!writeAll(client, body.data(), body.size())) {
        ESP_LOGE(TAG, "http body write failed");
        esp_http_client_cleanup(client);
        JsonWrapper e; e.AddItem("error", std::string("write_failed")); e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char errbuf[256] = {0};
        esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
        ESP_LOGE(TAG, "TTS HTTP %d: %s", status, errbuf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        JsonWrapper e;
        e.AddItem("error", std::string("http_status"));
        e.AddItem("status", status);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }

    int16_t* pcm = (int16_t*)heap_caps_malloc(kMaxPcmBytes, MALLOC_CAP_SPIRAM);
    if (!pcm) {
        ESP_LOGE(TAG, "pcm buffer alloc failed (%zu bytes)", kMaxPcmBytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    size_t total = 0;
    while (total < kMaxPcmBytes) {
        int r = esp_http_client_read(client, reinterpret_cast<char*>(pcm) + total,
                                      (int)std::min(kReadChunk, kMaxPcmBytes - total));
        if (r <= 0) break;
        total += (size_t)r;
    }
    const bool truncated = (total >= kMaxPcmBytes);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const uint32_t ttsMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (total < sizeof(int16_t)) {
        ESP_LOGW(TAG, "TTS returned no audio (%ums)", ttsMs);
        heap_caps_free(pcm);
        JsonWrapper e;
        e.AddItem("error", std::string("empty_response"));
        e.AddItem("tts_ms", (int)ttsMs);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }
    if (truncated) {
        ESP_LOGW(TAG, "TTS response exceeded %zu bytes (%us cap); playback truncated",
                 kMaxPcmBytes, (unsigned)(kMaxPcmSamples / 16000));
    }

    const size_t   samples = total / sizeof(int16_t);
    const uint32_t audioMs = (uint32_t)(samples / 16);  // 16 samples/ms at 16 kHz

    esp_err_t playRet = bsp_audio_play_mono16(pcm, samples);
    heap_caps_free(pcm);

    if (playRet != ESP_OK) {
        ESP_LOGW(TAG, "TTS synthesised %ums audio but speaker unavailable: %s",
                 audioMs, esp_err_to_name(playRet));
        JsonWrapper e;
        e.AddItem("error", std::string("speaker_unavailable"));
        e.AddItem("tts_ms", (int)ttsMs);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }

    ESP_LOGI(TAG, "TTS \"%s\" (%ums audio, %ums http)", job.text, audioMs, ttsMs);
    JsonWrapper out;
    out.AddItem("text", std::string(job.text));
    out.AddItem("ms", (int)audioMs);
    out.AddItem("tts_ms", (int)ttsMs);
    out.AddTime();
    mqtt_.publish(base + "tts", out.ToString());
}
