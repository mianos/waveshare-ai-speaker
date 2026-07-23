#include "VoicePipeline.h"

#include <cstring>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// esp-sr (managed component, fetched at build time).
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "board.h"
#include "JsonWrapper.h"
#include "MqttClient.h"
#include "Settings.h"
#include "SttClient.h"
#include "ToneGen.h"

namespace {
constexpr const char* TAG = "voice";
constexpr uint32_t kSampleRate    = 16000;
constexpr uint32_t kSamplesPerMs  = kSampleRate / 1000;  // 16

// One AFE instance for the (single) VoicePipeline; kept file-static so the
// task trampolines can reach it without threading it through user_data.
const esp_afe_sr_iface_t* s_afe      = nullptr;  // esp_afe_handle_from_config returns const
esp_afe_sr_data_t*        s_afe_data = nullptr;
srmodel_list_t*           s_models   = nullptr;
}  // namespace

VoicePipeline::VoicePipeline(Settings& settings, MqttClient& mqtt, SttClient& stt)
    : settings_(settings), mqtt_(mqtt), stt_(stt) {}

void VoicePipeline::start() {
    s_models = esp_srmodel_init("model");  // SPIFFS partition label in partitions.csv
    afe_config_t* cfg = afe_config_init(esp_get_input_format(), s_models,
                                        AFE_TYPE_SR, AFE_MODE_LOW_COST);
    cfg->ns_init  = true;   // noise suppression — cleaner audio for the STT server
    cfg->vad_init = true;   // VAD drives end-of-utterance detection
    // wakenet stays enabled (WN9 "Hi ESP" from sdkconfig); MultiNet is disabled
    // in sdkconfig, so no command model is loaded.
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);

    // Tone task: unpinned, below the feed/detect tasks — plays the wake blip
    // off the detect task (see toneLoop).
    xTaskCreate(toneTrampoline, "vp_tone", 4 * 1024, this, 4, &toneTaskHandle_);
    xTaskCreatePinnedToCore(captureTrampoline, "vp_detect", 8 * 1024, this, 5, nullptr, 1);
    xTaskCreatePinnedToCore(feedTrampoline,    "vp_feed",   8 * 1024, this, 5, nullptr, 0);
    ESP_LOGI(TAG, "voice pipeline started (wakeword 'Hi ESP')");
}

void VoicePipeline::feedTrampoline(void* arg)    { static_cast<VoicePipeline*>(arg)->feedTask(); }
void VoicePipeline::captureTrampoline(void* arg) { static_cast<VoicePipeline*>(arg)->captureTask(); }
void VoicePipeline::toneTrampoline(void* arg)    { static_cast<VoicePipeline*>(arg)->toneLoop(); }

// Waits for the detect task to signal a wake, then plays the blip. Kept off the
// detect task so playback neither stalls the capture/VAD loop nor contends with
// the AFE fetch (playing inline on the detect task wrote ESP_OK but was silent).
void VoicePipeline::toneLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        playWakeTone();
    }
}

// A short "I heard you" blip. No-ops if the speaker DAC isn't up.
//
// The clip starts with 100 ms of silence: the power amp is enabled per-clip and
// takes tens of ms to bias up, so a tone that starts immediately is swallowed
// (observed: 70 ms blip -> only the amp's turn-on pop was audible, while the
// longer boot/connect chimes played). The silence covers the amp's wake-up and
// the tone lands once it is live.
void VoicePipeline::playWakeTone() {
    static const tonegen::Note kWake[] = {{1200.0f, 120}};
    constexpr uint32_t kLeadInMs = 100;
    const size_t lead = tonegen::noteSamples(kLeadInMs, kSampleRate);
    const size_t tone = tonegen::chimeSamples(kWake, 1, kSampleRate);
    const size_t n = lead + tone;
    int16_t* buf = (int16_t*)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "wake tone: alloc failed (%zu samples)", n);
        return;
    }
    std::memset(buf, 0, lead * sizeof(int16_t));
    tonegen::renderChime(buf + lead, kWake, 1, kSampleRate, tonegen::kDefaultAmplitude, /*fadeMs=*/5);
    esp_err_t r = bsp_audio_play_mono16(buf, n);
    ESP_LOGI(TAG, "wake tone: %zu samples -> %s", n, esp_err_to_name(r));
    heap_caps_free(buf);
}

void VoicePipeline::feedTask() {
    int audio_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    int feed_channel    = esp_get_feed_channel();
    int nch             = s_afe->get_feed_channel_num(s_afe_data);
    if (nch != feed_channel) {
        ESP_LOGW(TAG, "AFE feed channels %d != board channels %d", nch, feed_channel);
    }
    size_t buf_bytes = (size_t)audio_chunksize * sizeof(int16_t) * feed_channel;
    int16_t* i2s_buf = (int16_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    assert(i2s_buf);

    esp_task_wdt_add(NULL);
    for (;;) {
        esp_get_feed_data(true, i2s_buf, buf_bytes);
        s_afe->feed(s_afe_data, i2s_buf);
        esp_task_wdt_reset();
    }
}

void VoicePipeline::captureTask() {
    const uint32_t maxCaptureMs = (uint32_t)settings_.maxCaptureMs;
    const uint32_t silenceLimit = (uint32_t)settings_.vadSilenceMs;
    // Buffer sized to the hard cap (+ one frame slack).
    const size_t capCapacity = (size_t)maxCaptureMs * kSamplesPerMs + 2048;

    bool     capturing = false;
    bool     sawSpeech = false;
    int16_t* cap       = nullptr;
    size_t   capSamples = 0;
    uint32_t silenceMs  = 0;
    uint32_t totalMs    = 0;

    const std::string wakeTopic = "tele/" + settings_.sensorName + "/wake";

    esp_task_wdt_add(NULL);
    for (;;) {
        afe_fetch_result_t* res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error");
            esp_task_wdt_reset();
            continue;
        }

        const int      frameSamples = res->data_size / (int)sizeof(int16_t);
        const uint32_t frameMs      = (uint32_t)frameSamples / kSamplesPerMs;

        const bool woke =
            (res->raw_data_channels == 1 && res->wakeup_state == WAKENET_DETECTED) ||
            (res->raw_data_channels > 1 && res->wakeup_state == WAKENET_CHANNEL_VERIFIED);

        if (!capturing && woke) {
            cap = (int16_t*)heap_caps_malloc(capCapacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!cap) {
                ESP_LOGE(TAG, "capture buffer alloc failed (%zu samples)", capCapacity);
                esp_task_wdt_reset();
                continue;  // stay armed, wakenet still on
            }
            capturing = true;
            sawSpeech = false;
            capSamples = 0;
            silenceMs = 0;
            totalMs = 0;
            s_afe->disable_wakenet(s_afe_data);  // don't re-trigger mid-utterance
            ESP_LOGI(TAG, "wake: capturing");
            bsp_led_set(0, 40, 0);  // green while listening/capturing
            if (settings_.playTone && toneTaskHandle_) {
                xTaskNotifyGive(toneTaskHandle_);  // blip, played off this task
            }
            if (settings_.publishWakeEvent) {
                JsonWrapper w;
                w.AddItem("event", std::string("wake"));
                w.AddTime();
                mqtt_.publish(wakeTopic, w.ToString());
            }
        }

        if (capturing) {
            if (capSamples + (size_t)frameSamples <= capCapacity) {
                std::memcpy(cap + capSamples, res->data, res->data_size);
                capSamples += frameSamples;
            }
            totalMs += frameMs;

            const bool speech = (res->vad_state == VAD_SPEECH);
            if (speech) {
                sawSpeech = true;
                silenceMs = 0;
            } else if (sawSpeech) {
                silenceMs += frameMs;
            }

            const bool stop = (sawSpeech && silenceMs >= silenceLimit) || (totalMs >= maxCaptureMs);
            if (stop) {
                capturing = false;
                s_afe->enable_wakenet(s_afe_data);  // re-arm
                bsp_led_set(0, 0, 0);  // off when capture ends

                if (sawSpeech && capSamples > 0) {
                    const uint32_t capMs = (uint32_t)capSamples / kSamplesPerMs;
                    ESP_LOGI(TAG, "captured %ums (%zu samples), sending to STT", capMs, capSamples);
                    stt_.submit(cap, capSamples, kSampleRate, capMs);  // takes ownership
                    cap = nullptr;
                } else {
                    ESP_LOGI(TAG, "wake but no speech; discarding");
                    heap_caps_free(cap);
                    cap = nullptr;
                }
            }
        }
        esp_task_wdt_reset();
    }
}
