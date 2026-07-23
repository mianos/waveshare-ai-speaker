#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct Settings;
class MqttClient;
class SttClient;

// esp-sr voice trigger + utterance capture.
//
// Two pinned tasks (mirroring esp-sr's reference topology):
//   feed  (core 0) — pulls mic frames from the board and feeds the AFE.
//   detect(core 1) — fetches AFE output; on the "Hi ESP" wakeword it captures
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

    Settings&   settings_;
    MqttClient& mqtt_;
    SttClient&  stt_;
};
