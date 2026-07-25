#pragma once

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct Settings;
class MqttClient;

// One "speak this" request. Fixed-size buffers (mirroring mianesp's
// AudioPlayer::PlayRequest) avoid a heap alloc for a small, frequent job.
struct TtsJob {
    char text[256];
    char voice[32];  // empty -> worker falls back to Settings::ttsVoice
};

// Synthesizes speech on its own task: POSTs text to the configured FastKoko
// (Kokoro-FastAPI) endpoint requesting raw PCM (response_format=pcm), already
// resampled to 16 kHz mono by the server (tts_url carries ?sample_rate=16000 —
// see README), then plays it out the board speaker via bsp_audio_play_mono16.
// No on-device decoding or resampling: the response bytes go straight to the
// DAC. Fail-fast: if tts_url is unset it logs and drops the job.
class TtsClient {
public:
    TtsClient(Settings& settings, MqttClient& mqtt);

    // Spawn the worker task + job queue. Call once.
    void start();

    // Enqueue a "speak this" request. voice="" falls back to Settings::ttsVoice.
    // Returns false (and logs) if text is empty or the queue is full (previous
    // request still in flight).
    bool speak(const std::string& text, const std::string& voice = "");

private:
    static void workerTrampoline(void* arg);
    void worker();
    void process(const TtsJob& job);

    Settings&     settings_;
    MqttClient&   mqtt_;
    QueueHandle_t queue_ = nullptr;
};
