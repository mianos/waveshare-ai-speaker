#include "VoicePipeline.h"

#include <algorithm>
#include <cstdio>
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
#include "MicLevel.h"
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

// Applies the near-field mic array tuning settings on top of esp-sr's own
// input-format-derived defaults. -1 fields are left untouched; only the
// gain is unconditionally set (100 = 1.00x is a no-op).
void applyAfeTuning(afe_config_t* cfg, Settings& settings) {
    if (settings.wakenetMode >= 0 && settings.wakenetMode <= DET_MODE_90_COPY_PARAMS) {
        cfg->wakenet_mode = (det_mode_t)settings.wakenetMode;
    } else if (settings.wakenetMode != -1) {
        ESP_LOGW(TAG, "wakenet_mode %d out of range [0,6]; keeping default", settings.wakenetMode);
    }
    if (settings.vadMode >= VAD_MODE_0 && settings.vadMode <= VAD_MODE_4) {
        cfg->vad_mode = (vad_mode_t)settings.vadMode;
    } else if (settings.vadMode != -1) {
        ESP_LOGW(TAG, "vad_mode %d out of range [0,4]; keeping default", settings.vadMode);
    }
    if (settings.agcTargetDbfs != -1) {
        cfg->agc_target_level_dbfs = settings.agcTargetDbfs;
    }
    if (settings.agcCompressionDb != -1) {
        cfg->agc_compression_gain_db = settings.agcCompressionDb;
    }
    int gainX100 = settings.micGainX100;
    if (gainX100 < 10 || gainX100 > 1000) {
        ESP_LOGW(TAG, "mic_gain_x100 %d out of range [10,1000]; using 100", gainX100);
        gainX100 = 100;
    }
    cfg->afe_linear_gain = gainX100 / 100.0f;

    ESP_LOGI(TAG, "afe tuning: wakenet_mode=%d vad_mode=%d agc_target_dbfs=%d "
                  "agc_compression_db=%d linear_gain=%.2f",
             (int)cfg->wakenet_mode, (int)cfg->vad_mode, cfg->agc_target_level_dbfs,
             cfg->agc_compression_gain_db, cfg->afe_linear_gain);
}

}  // namespace

VoicePipeline::VoicePipeline(Settings& settings, MqttClient& mqtt, SttClient& stt)
    : settings_(settings), mqtt_(mqtt), stt_(stt) {}

void VoicePipeline::start() {
    levelMutex_ = xSemaphoreCreateMutex();

    s_models = esp_srmodel_init("model");  // SPIFFS partition label in partitions.csv
    // Anti-brick guard: a corrupt/empty model partition (e.g. an interrupted
    // /model upload) must not take down boot — web/MQTT/OTA stay up so the
    // model can be re-uploaded. Voice is simply disabled until then.
    const char* wnName = s_models ? esp_srmodel_filter(s_models, ESP_WN_PREFIX, nullptr) : nullptr;
    if (!wnName) {
        ESP_LOGE(TAG, "no WakeNet model in the 'model' partition; voice pipeline "
                      "disabled (re-upload via POST /model, then reboot)");
        return;
    }
    std::snprintf(wakeModel_, sizeof(wakeModel_), "%s", wnName);
    ESP_LOGI(TAG, "wakenet model: %s", wakeModel_);
    afe_config_t* cfg = afe_config_init(esp_get_input_format(), s_models,
                                        AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed; voice pipeline disabled");
        return;
    }
    cfg->ns_init  = true;   // noise suppression — cleaner audio for the STT server
    cfg->vad_init = true;   // VAD drives end-of-utterance detection
    // wakenet stays enabled (WN9 "Hi, ESP" from sdkconfig); MultiNet is disabled
    // in sdkconfig, so no command model is loaded.
    applyAfeTuning(cfg, settings_);

    micNum_ = std::min(cfg->pcm_config.mic_num, kMaxMicChannels);
    for (int i = 0; i < micNum_; ++i) micIds_[i] = cfg->pcm_config.mic_ids[i];

    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);

    // Tone task: unpinned, below the feed/detect tasks — plays the wake blip
    // off the detect task (see toneLoop).
    xTaskCreate(toneTrampoline, "vp_tone", 4 * 1024, this, 4, &toneTaskHandle_);
    xTaskCreatePinnedToCore(captureTrampoline, "vp_detect", 8 * 1024, this, 5, &captureTaskHandle_, 1);
    xTaskCreatePinnedToCore(feedTrampoline,    "vp_feed",   8 * 1024, this, 5, &feedTaskHandle_, 0);
    started_ = true;
    ESP_LOGI(TAG, "voice pipeline started (wakeword 'Hi, ESP')");
}

// The AFE's WakeNet inference reads model coefficients straight out of
// memory-mapped flash (CONFIG_MODEL_IN_FLASH) — rewriting the partition under
// it would feed the detector garbage and risk a crash mid-write. Suspend
// everything that drives the AFE first; the null guards matter because start()
// may have bailed (no model) leaving the handles unset, and vTaskSuspend(NULL)
// would suspend the *calling* task — the web server handling the upload.
void VoicePipeline::suspendForModelUpdate() {
    if (feedTaskHandle_)    vTaskSuspend(feedTaskHandle_);
    if (captureTaskHandle_) vTaskSuspend(captureTaskHandle_);
    if (toneTaskHandle_)    vTaskSuspend(toneTaskHandle_);
    ESP_LOGW(TAG, "voice pipeline suspended for model update");
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

        // Feed this chunk into the running per-mic level accumulators, but
        // only while captureTask has a capture in progress (see there) — a
        // level sampled on a free-running timer mostly shows ambient noise
        // between events, not anything about the recording it's meant to
        // characterise.
        if (settings_.micLevelLog && micNum_ > 0) {
            xSemaphoreTake(levelMutex_, portMAX_DELAY);
            if (levelActive_) {
                for (int m = 0; m < micNum_; ++m) {
                    miclevel::accumulate(levelAccum_[m], i2s_buf, audio_chunksize, feed_channel, micIds_[m]);
                }
            }
            xSemaphoreGive(levelMutex_);
        }

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

            if (settings_.micLevelLog) {
                xSemaphoreTake(levelMutex_, portMAX_DELAY);
                for (int m = 0; m < micNum_; ++m) levelAccum_[m] = miclevel::ChannelAccum{};
                levelActive_ = true;
                xSemaphoreGive(levelMutex_);
            }
            if (settings_.playTone && toneTaskHandle_) {
                xTaskNotifyGive(toneTaskHandle_);  // blip, played off this task
            }
            if (settings_.publishWakeEvent) {
                JsonWrapper w;
                w.AddItem("event", std::string("wake"));
                // Detection-quality telemetry. volume_db is the input level
                // (dB, pre-AGC) over WakeNet's ~1.5 s receptive field — the
                // direct "what did the model hear" number for tuning
                // mic_hw_gain_db (a 0 dB PGA once made the wakeword deaf
                // while Whisper still transcribed fine). channel is the mic
                // channel that triggered; wake_ms the phrase length; n a
                // per-boot counter so gaps/reboots are visible in the stream.
                w.AddItem("n", (int)++wakeCount_);
                w.AddItem("volume_db", res->data_volume);
                w.AddItem("channel", res->trigger_channel_id);
                w.AddItem("wake_ms", (int)((uint32_t)res->wake_word_length / kSamplesPerMs));
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

                if (settings_.micLevelLog && micNum_ > 0) {
                    miclevel::ChannelLevel lvl[kMaxMicChannels];
                    xSemaphoreTake(levelMutex_, portMAX_DELAY);
                    levelActive_ = false;
                    for (int m = 0; m < micNum_; ++m) lvl[m] = miclevel::finishLevel(levelAccum_[m]);
                    xSemaphoreGive(levelMutex_);

                    char line[160];
                    int  off = 0;
                    for (int m = 0; m < micNum_; ++m) {
                        off += std::snprintf(line + off, sizeof(line) - off,
                            "%smic%d rms=%.1fdB peak=%.1fdB", m ? "  " : "", m, lvl[m].rmsDb, lvl[m].peakDb);
                    }
                    ESP_LOGI(TAG, "capture levels (%ums): %s", totalMs, line);

                    // Publish the same summary so gain can be tuned without a
                    // serial console (peaks near 0 dBFS ⇒ clipping, lower
                    // mic_hw_gain_db; far below ⇒ raise it).
                    JsonWrapper ml;
                    ml.AddItem("ms", (int)totalMs);
                    for (int m = 0; m < micNum_; ++m) {
                        const std::string p = "mic" + std::to_string(m);
                        ml.AddItem(p + "_rms_db",  lvl[m].rmsDb);
                        ml.AddItem(p + "_peak_db", lvl[m].peakDb);
                    }
                    ml.AddTime();
                    mqtt_.publish("tele/" + settings_.sensorName + "/miclevel", ml.ToString());
                }

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
