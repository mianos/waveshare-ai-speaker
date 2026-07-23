#include "SttClient.h"

#include <string>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "JsonWrapper.h"
#include "MqttClient.h"
#include "Settings.h"
#include "SttWire.h"

namespace {
constexpr const char* TAG = "stt";

// Fixed multipart boundary. The body is binary WAV, but a real collision with
// this token is astronomically unlikely; we do not scan/escape the payload.
constexpr const char* kBoundary = "----wsvoiceFormBoundary8x9Kq2mLp";

constexpr int    kHttpTimeoutMs   = 15000;  // STT inference can be slow
constexpr size_t kWriteChunk      = 4096;   // PCM streamed in these slices
constexpr size_t kMaxResponseBytes = 8192;  // transcript JSON is tiny
constexpr int    kQueueDepth       = 2;

// Write the whole buffer, looping over short writes. Returns false on error.
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

SttClient::SttClient(Settings& settings, MqttClient& mqtt)
    : settings_(settings), mqtt_(mqtt) {}

void SttClient::start() {
    queue_ = xQueueCreate(kQueueDepth, sizeof(AudioJob));
    if (!queue_) {
        ESP_LOGE(TAG, "failed to create job queue");
        return;
    }
    // 6 KB stack: HTTP client + cJSON parse. STT does not touch the audio path,
    // so a modest priority is fine.
    xTaskCreate(workerTrampoline, "stt", 6144, this, 4, nullptr);
}

bool SttClient::submit(int16_t* pcm, size_t samples, uint32_t sample_rate, uint32_t capture_ms) {
    AudioJob job{pcm, samples, sample_rate, capture_ms};
    if (!queue_ || xQueueSend(queue_, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "job queue full, dropping %zu samples (previous upload still in flight)", samples);
        heap_caps_free(pcm);
        return false;
    }
    return true;
}

void SttClient::workerTrampoline(void* arg) {
    static_cast<SttClient*>(arg)->worker();
}

void SttClient::worker() {
    for (;;) {
        AudioJob job;
        if (xQueueReceive(queue_, &job, portMAX_DELAY) == pdTRUE) {
            process(job);
            heap_caps_free(job.pcm);
        }
    }
}

void SttClient::process(const AudioJob& job) {
    const std::string base = "tele/" + settings_.sensorName + "/";

    // Fail-fast: nothing to POST to yet.
    if (settings_.sttUrl.empty()) {
        ESP_LOGW(TAG, "stt_url not configured; captured %ums (%zu samples) dropped. "
                      "Set it via cmnd/%s/settings or POST /config.",
                 job.capture_ms, job.samples, settings_.sensorName.c_str());
        JsonWrapper err;
        err.AddItem("error", std::string("stt_url_unset"));
        err.AddItem("ms", (int)job.capture_ms);
        err.AddTime();
        mqtt_.publish(base + "stterror", err.ToString());
        return;
    }

    const std::string head    = sttwire::multipartHead(kBoundary, "audio.wav");
    const std::string wavHdr  = sttwire::wavHeader((uint32_t)job.samples, job.sample_rate);
    const std::string tail    = sttwire::multipartTail(kBoundary, settings_.sttModel, settings_.sttLanguage);
    const std::string ctype   = sttwire::contentType(kBoundary);
    const size_t pcmBytes     = job.samples * sizeof(int16_t);
    const int    contentLen   = (int)(head.size() + wavHdr.size() + pcmBytes + tail.size());

    esp_http_client_config_t cfg = {};
    cfg.url        = settings_.sttUrl.c_str();
    cfg.method     = HTTP_METHOD_POST;
    cfg.timeout_ms = kHttpTimeoutMs;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", ctype.c_str());

    const int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(client, contentLen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        JsonWrapper e; e.AddItem("error", std::string(esp_err_to_name(err))); e.AddTime();
        mqtt_.publish(base + "stterror", e.ToString());
        return;
    }

    bool ok = writeAll(client, head.data(), head.size()) &&
              writeAll(client, wavHdr.data(), wavHdr.size());
    const char* pcmData = reinterpret_cast<const char*>(job.pcm);
    for (size_t off = 0; ok && off < pcmBytes; off += kWriteChunk) {
        size_t n = (pcmBytes - off < kWriteChunk) ? (pcmBytes - off) : kWriteChunk;
        ok = writeAll(client, pcmData + off, n);
    }
    ok = ok && writeAll(client, tail.data(), tail.size());
    if (!ok) {
        ESP_LOGE(TAG, "http body write failed");
        esp_http_client_cleanup(client);
        JsonWrapper e; e.AddItem("error", std::string("write_failed")); e.AddTime();
        mqtt_.publish(base + "stterror", e.ToString());
        return;
    }

    int64_t contentLength = esp_http_client_fetch_headers(client);
    (void)contentLength;
    int status = esp_http_client_get_status_code(client);

    std::string resp;
    char buf[512];
    while (resp.size() < kMaxResponseBytes) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r <= 0) break;
        resp.append(buf, r);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const uint32_t sttMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (status != 200) {
        ESP_LOGE(TAG, "STT HTTP %d in %ums", status, sttMs);
        JsonWrapper e;
        e.AddItem("error", std::string("http_status"));
        e.AddItem("status", status);
        e.AddItem("stt_ms", (int)sttMs);
        e.AddTime();
        mqtt_.publish(base + "stterror", e.ToString());
        return;
    }

    // OpenAI-compatible transcription response: {"text": "..."}.
    JsonWrapper doc = JsonWrapper::Parse(resp);
    std::string text;
    if (!doc.GetField("text", text)) {
        ESP_LOGE(TAG, "STT response missing 'text': %.120s", resp.c_str());
        JsonWrapper e;
        e.AddItem("error", std::string("bad_response"));
        e.AddItem("stt_ms", (int)sttMs);
        e.AddTime();
        mqtt_.publish(base + "stterror", e.ToString());
        return;
    }

    ESP_LOGI(TAG, "STT (%ums audio, %ums http): \"%s\"", job.capture_ms, sttMs, text.c_str());
    JsonWrapper out;
    out.AddItem("text", text);
    out.AddItem("ms", (int)job.capture_ms);   // captured audio length
    out.AddItem("stt_ms", (int)sttMs);        // round-trip latency (perf metric)
    out.AddTime();
    mqtt_.publish(base + "stt", out.ToString());
}
