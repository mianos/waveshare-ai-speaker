#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct Settings;
class MqttClient;

// One captured utterance handed from VoicePipeline to SttClient. `pcm` is a
// heap_caps(SPIRAM) buffer of 16-bit mono samples; ownership transfers to
// SttClient on submit() (it frees the buffer when done, even on error/drop).
struct AudioJob {
    int16_t* pcm;
    size_t   samples;
    uint32_t sample_rate;
    uint32_t capture_ms;
};

// Transcribes captured audio on its own task: assembles the multipart/form-data
// body (streamed, never one big buffer), POSTs to the configured STT endpoint,
// and publishes the transcript to MQTT. Fail-fast: if stt_url is unset it logs
// and drops the job.
class SttClient {
public:
    SttClient(Settings& settings, MqttClient& mqtt);

    // Spawn the worker task + job queue. Call once.
    void start();

    // Enqueue an utterance for transcription, taking ownership of `pcm`. Returns
    // false and frees `pcm` if the queue is full (previous job still uploading).
    bool submit(int16_t* pcm, size_t samples, uint32_t sample_rate, uint32_t capture_ms);

private:
    static void workerTrampoline(void* arg);
    void worker();
    void process(const AudioJob& job);

    Settings&     settings_;
    MqttClient&   mqtt_;
    QueueHandle_t queue_ = nullptr;
};
