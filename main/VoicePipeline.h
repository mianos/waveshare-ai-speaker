#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "MicLevel.h"

struct Settings;
class MqttClient;
class SttClient;

// esp-sr voice trigger + utterance capture.
//
// Two pinned tasks (mirroring esp-sr's reference topology):
//   feed  (core 0) — pulls mic frames from the board and feeds the AFE.
//   detect(core 1) — fetches AFE output; on the "Computer" wakeword it captures
//                    the following speech (AFE-enhanced mono 16 kHz), ending on
//                    VAD silence or a max-duration cap, then hands the buffer to
//                    SttClient.
//
// WakeNet is only a trigger here — there is no on-device command recognition;
// meaning comes from the STT transcript.
class VoicePipeline {
public:
    VoicePipeline(Settings& settings, MqttClient& mqtt, SttClient& stt);

    // Initialise esp-sr (models from the "model" flash partition) and start the
    // feed + detect tasks. Call after bsp_board_init().
    void start();

private:
    static void feedTrampoline(void* arg);
    static void captureTrampoline(void* arg);
    static void toneTrampoline(void* arg);
    void feedTask();
    void captureTask();

    // Dedicated tone task: waits for a notification from the detect task and
    // plays the wake blip, so playback never blocks the capture loop (playing
    // inline on the detect task came out silent).
    void toneLoop();
    void playWakeTone();

    TaskHandle_t toneTaskHandle_ = nullptr;

    // Mic channel layout read back from the AFE pcm_config after
    // afe_config_init(), so feedTask() can log per-mic-channel levels without
    // guessing which interleaved slots are mic vs. reference/unused.
    static constexpr int kMaxMicChannels = 4;
    int     micNum_ = 0;
    uint8_t micIds_[kMaxMicChannels] = {0, 0, 0, 0};

    // mic_level_log accumulates raw per-mic levels for the span of one wake
    // capture (started/reset by captureTask, filled in by feedTask on every
    // chunk while active) and logs one summary when the capture ends — a
    // periodic/free-running print mostly shows ambient noise between events
    // rather than anything about the recording itself.
    SemaphoreHandle_t     levelMutex_ = nullptr;
    bool                  levelActive_ = false;
    miclevel::ChannelAccum levelAccum_[kMaxMicChannels];

    Settings&   settings_;
    MqttClient& mqtt_;
    SttClient&  stt_;
};
