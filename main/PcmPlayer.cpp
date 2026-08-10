#include "PcmPlayer.h"

#include <algorithm>
#include <cstdint>
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

namespace {
constexpr const char* TAG = "pcmplay";

constexpr int    kHttpTimeoutMs = 20000;  // stall timeout for a single read, not the whole clip
constexpr size_t kReadChunk     = 4096;
// Whole-clip cap. Cue clips are a few seconds; this bounds the PSRAM a
// hostile/misconfigured URL can claim, not normal use.
constexpr size_t kMaxClipBytes    = 2 * 1024 * 1024;  // ~64 s @ 16 kHz mono s16
// Grow-by-doubling start when the server sends no Content-Length (chunked).
constexpr size_t kInitialCapBytes = 64 * 1024;
constexpr int    kQueueDepth      = 2;
}  // namespace

PcmPlayer::PcmPlayer(Settings& settings, MqttClient& mqtt)
    : settings_(settings), mqtt_(mqtt) {}

void PcmPlayer::start() {
    queue_ = xQueueCreate(kQueueDepth, sizeof(PlayJob));
    if (!queue_) {
        ESP_LOGE(TAG, "failed to create job queue");
        return;
    }
    // Same shape as the TTS worker (HTTP client on a 6 KB stack, priority 4).
    // Playback itself serialises on the board's play lock, so a cue and a
    // "say" queued together just play back-to-back.
    xTaskCreate(workerTrampoline, "pcmplay", 6144, this, 4, nullptr);
}

bool PcmPlayer::play(const std::string& url) {
    PlayJob job;
    job.url = url.empty() ? nullptr : new std::string(url);
    if (!queue_ || xQueueSend(queue_, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "job queue full, dropping \"%.60s\"",
                 url.empty() ? "<play_url>" : url.c_str());
        delete job.url;
        return false;
    }
    return true;
}

void PcmPlayer::workerTrampoline(void* arg) {
    static_cast<PcmPlayer*>(arg)->worker();
}

void PcmPlayer::worker() {
    for (;;) {
        PlayJob job;
        if (xQueueReceive(queue_, &job, portMAX_DELAY) == pdTRUE) {
            process(job);
            delete job.url;
        }
    }
}

void PcmPlayer::process(const PlayJob& job) {
    const std::string base = "tele/" + settings_.sensorName + "/";
    const std::string url  = job.url ? *job.url : settings_.playUrl;

    auto fail = [&](const char* code, int status = 0) {
        JsonWrapper e;
        e.AddItem("error", std::string(code));
        if (status) e.AddItem("status", status);
        if (!url.empty()) e.AddItem("url", url);
        e.AddTime();
        mqtt_.publish(base + "playerror", e.ToString());
    };

    // Fail-fast: nothing to GET.
    if (url.empty()) {
        ESP_LOGW(TAG, "play: no url given and play_url unset; dropped. "
                      "Set it via cmnd/%s/settings or POST /config.",
                 settings_.sensorName.c_str());
        fail("play_url_unset");
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url        = url.c_str();
    cfg.method     = HTTP_METHOD_GET;
    cfg.timeout_ms = kHttpTimeoutMs;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        fail("client_init_failed");
        return;
    }

    const int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fail(esp_err_to_name(err));
        return;
    }

    const int64_t lenHeader = esp_http_client_fetch_headers(client);  // -1 ⇒ chunked
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s -> HTTP %d", url.c_str(), status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fail("http_status", status);
        return;
    }
    if (lenHeader > (int64_t)kMaxClipBytes) {
        ESP_LOGE(TAG, "clip too large: %lld bytes (cap %zu)", (long long)lenHeader, kMaxClipBytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fail("too_large");
        return;
    }

    // Buffer the whole clip in PSRAM. Content-Length sizes it exactly; a
    // chunked response falls back to grow-by-doubling up to the cap.
    size_t cap = lenHeader > 0 ? (size_t)lenHeader : kInitialCapBytes;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "clip buffer alloc failed (%zu bytes)", cap);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fail("alloc_failed");
        return;
    }

    size_t total     = 0;
    bool   readError = false;
    bool   tooLarge  = false;
    for (;;) {
        if (total == cap) {
            if (lenHeader > 0) break;  // got exactly what the server promised
            if (cap >= kMaxClipBytes) {
                tooLarge = true;
                break;
            }
            const size_t newCap = std::min(cap * 2, kMaxClipBytes);
            uint8_t* nb = (uint8_t*)heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM);
            if (!nb) {
                ESP_LOGE(TAG, "clip buffer grow failed (%zu bytes)", newCap);
                readError = true;
                break;
            }
            buf = nb;
            cap = newCap;
        }
        int r = esp_http_client_read(client, reinterpret_cast<char*>(buf) + total,
                                     (int)std::min(kReadChunk, cap - total));
        if (r < 0) {
            ESP_LOGE(TAG, "http read failed at %zu bytes", total);
            readError = true;
            break;
        }
        if (r == 0) break;  // EOF
        total += (size_t)r;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    const uint32_t httpMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (readError || tooLarge) {
        heap_caps_free(buf);
        fail(tooLarge ? "too_large" : "read_failed");
        return;
    }
    if (lenHeader > 0 && total != (size_t)lenHeader) {
        // Play what arrived anyway — a truncated cue beats silence — but flag it.
        ESP_LOGW(TAG, "clip truncated: got %zu of %lld bytes", total, (long long)lenHeader);
    }

    const size_t samples = total / sizeof(int16_t);  // trailing odd byte ignored
    if (samples == 0) {
        ESP_LOGW(TAG, "clip empty (%ums http)", httpMs);
        heap_caps_free(buf);
        fail("empty_response");
        return;
    }

    // One-shot playback: amp enable, write, DMA drain and the play lock are
    // all handled inside (see bsp_audio_play_mono16). Blocks for the clip.
    esp_err_t playRet = bsp_audio_play_mono16(reinterpret_cast<int16_t*>(buf), samples);
    heap_caps_free(buf);

    const uint32_t audioMs = (uint32_t)(samples / 16);  // 16 samples/ms at 16 kHz
    if (playRet == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "clip fetched (%ums audio) but speaker unavailable", audioMs);
        fail("speaker_unavailable");
        return;
    }
    if (playRet != ESP_OK) {
        ESP_LOGW(TAG, "playback failed: %s", esp_err_to_name(playRet));
        fail("playback_failed");
        return;
    }

    ESP_LOGI(TAG, "played %s (%ums audio, %ums http)", url.c_str(), audioMs, httpMs);
    JsonWrapper out;
    out.AddItem("url", url);
    out.AddItem("ms", (int)audioMs);
    out.AddItem("http_ms", (int)httpMs);
    out.AddTime();
    mqtt_.publish(base + "play", out.ToString());
}
