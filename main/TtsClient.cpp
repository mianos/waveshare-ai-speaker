#include "TtsClient.h"

#include <cmath>
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

constexpr int    kHttpTimeoutMs = 20000;  // stall timeout for a single read, not the whole clip
constexpr size_t kReadChunk     = 2048;
// Backstop only, not a normal-use limit: audio is played incrementally as it
// downloads (see process()), so length isn't bounded by any buffer size.
// This just stops a runaway/misbehaving TTS server from streaming forever.
constexpr size_t kMaxStreamSamples = 16000 * 300;  // 5 min @ 16 kHz mono 16-bit
constexpr size_t kMaxStreamBytes   = kMaxStreamSamples * sizeof(int16_t);
constexpr int    kQueueDepth    = 2;

// LED brightness (0-40, matching VoicePipeline's wake-listening green) tracking
// this chunk's peak amplitude. sqrt compresses the range so quiet speech still
// shows some pulse instead of only lighting up near full scale.
constexpr uint8_t kLedMaxBrightness = 40;

uint8_t chunkLedBrightness(const int16_t* pcm, size_t nsamples) {
    int16_t peak = 0;
    for (size_t i = 0; i < nsamples; ++i) {
        int16_t a = pcm[i] < 0 ? (int16_t)(-pcm[i]) : pcm[i];
        if (a > peak) peak = a;
    }
    float level = std::sqrt((float)peak / 32768.0f);
    return (uint8_t)(level * kLedMaxBrightness);
}

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
    TtsJob job;
    job.text  = new std::string(text);
    job.voice = voice.empty() ? nullptr : new std::string(voice);
    if (!queue_ || xQueueSend(queue_, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "job queue full, dropping \"%.40s\"", text.c_str());
        delete job.text;
        delete job.voice;
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
            delete job.text;
            delete job.voice;
        }
    }
}

void TtsClient::process(const TtsJob& job) {
    const std::string base  = "tele/" + settings_.sensorName + "/";
    const std::string voice = job.voice ? *job.voice : settings_.ttsVoice;

    // Fail-fast: nothing to POST to yet.
    if (settings_.ttsUrl.empty()) {
        ESP_LOGW(TAG, "tts_url not configured; \"%s\" dropped. "
                      "Set it via cmnd/%s/settings or POST /config.",
                 job.text->c_str(), settings_.sensorName.c_str());
        JsonWrapper err;
        err.AddItem("error", std::string("tts_url_unset"));
        err.AddTime();
        mqtt_.publish(base + "ttserror", err.ToString());
        return;
    }

    const std::string body = tts::requestBody(*job.text, voice);

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

    // Play as it downloads: begin the stream, hand each HTTP chunk straight to
    // the codec, end the stream. Length is bounded only by kMaxStreamBytes (a
    // backstop against a runaway server), not by any pre-sized buffer.
    esp_err_t streamErr = bsp_audio_stream_begin();
    const bool canPlay = (streamErr == ESP_OK);

    // Small read buffer, off the "tts" task's 6 KB stack (it's shared with the
    // HTTP client's own working set) -- unlike the old whole-clip buffer, this
    // one is tiny, so a plain internal-RAM alloc is fine (no need for PSRAM).
    uint8_t* buf = (uint8_t*)heap_caps_malloc(kReadChunk + 1, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "tts read buffer alloc failed (%zu bytes)", kReadChunk + 1);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (canPlay) bsp_audio_stream_end();
        return;
    }
    size_t carry      = 0;  // 0 or 1 leftover byte at buf[0] from the previous read
    size_t totalBytes = 0;
    bool   playFailed = false;

    for (;;) {
        if (totalBytes >= kMaxStreamBytes) {
            ESP_LOGW(TAG, "TTS stream exceeded %zu bytes (%us backstop); stopping",
                     kMaxStreamBytes, (unsigned)(kMaxStreamSamples / 16000));
            break;
        }
        int r = esp_http_client_read(client, reinterpret_cast<char*>(buf) + carry, (int)kReadChunk);
        if (r <= 0) break;
        totalBytes += (size_t)r;

        const size_t avail       = carry + (size_t)r;
        const size_t sampleBytes = avail & ~size_t(1);  // whole int16 samples only
        const size_t samples     = sampleBytes / sizeof(int16_t);

        if (canPlay && !playFailed && samples > 0) {
            const int16_t* samplePcm = reinterpret_cast<int16_t*>(buf);
            uint8_t brightness = chunkLedBrightness(samplePcm, samples);
            bsp_led_set(0, 0, brightness);

            esp_err_t playRet = bsp_audio_stream_write(samplePcm, samples);
            if (playRet != ESP_OK) {
                ESP_LOGW(TAG, "TTS stream write failed: %s", esp_err_to_name(playRet));
                playFailed = true;
            }
        }

        carry = avail - sampleBytes;
        if (carry) buf[0] = buf[sampleBytes];
    }
    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (canPlay) {
        bsp_audio_stream_end();
        bsp_led_set(0, 0, 0);
    }

    const uint32_t ttsMs   = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    const size_t   samples = totalBytes / sizeof(int16_t);
    const uint32_t audioMs = (uint32_t)(samples / 16);  // 16 samples/ms at 16 kHz

    if (totalBytes < sizeof(int16_t)) {
        ESP_LOGW(TAG, "TTS returned no audio (%ums)", ttsMs);
        JsonWrapper e;
        e.AddItem("error", std::string("empty_response"));
        e.AddItem("tts_ms", (int)ttsMs);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }
    if (!canPlay) {
        ESP_LOGW(TAG, "TTS synthesised %ums audio but speaker unavailable: %s",
                 audioMs, esp_err_to_name(streamErr));
        JsonWrapper e;
        e.AddItem("error", std::string("speaker_unavailable"));
        e.AddItem("tts_ms", (int)ttsMs);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }
    if (playFailed) {
        ESP_LOGW(TAG, "TTS playback failed partway (%ums synthesised)", audioMs);
        JsonWrapper e;
        e.AddItem("error", std::string("playback_failed"));
        e.AddItem("tts_ms", (int)ttsMs);
        e.AddTime();
        mqtt_.publish(base + "ttserror", e.ToString());
        return;
    }

    ESP_LOGI(TAG, "TTS \"%s\" (%ums audio, %ums http)", job.text->c_str(), audioMs, ttsMs);
    JsonWrapper out;
    out.AddItem("text", *job.text);
    out.AddItem("ms", (int)audioMs);
    out.AddItem("tts_ms", (int)ttsMs);
    out.AddTime();
    mqtt_.publish(base + "tts", out.ToString());
}
