#pragma once

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct Settings;
class MqttClient;

// One "speak this" request. text/voice are heap-owned (allocated in speak(),
// freed by the worker once process() returns) so length isn't bounded by a
// fixed buffer: a FreeRTOS queue item just needs a fixed *size*, and a
// pointer is fixed size regardless of what it points to.
struct TtsJob {
    std::string* text;
    std::string* voice;  // nullptr -> worker falls back to Settings::ttsVoice
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
