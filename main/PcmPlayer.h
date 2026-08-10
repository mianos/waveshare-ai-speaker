#pragma once

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct Settings;
class MqttClient;

// One "play this clip" request. url is heap-owned (allocated in play(), freed
// by the worker once process() returns) — same fixed-size-queue-item pattern
// as TtsJob. nullptr ⇒ worker falls back to Settings::playUrl.
struct PlayJob {
    std::string* url;
};

// Plays a raw-PCM cue clip fetched from a URL (16 kHz mono s16le, no
// container — see the README for the ffmpeg recipe that produces one) on its
// own task. Unlike TtsClient, the whole clip is buffered into PSRAM before
// playback starts: cue clips are short with a known Content-Length, and
// buffering first means a mid-download Wi-Fi stall can't punch an audible gap
// in the sound. No on-device decoding or resampling — the URL must already
// serve the board's native format. Fail-fast: if no URL is given and
// play_url is unset, it logs and drops the job.
class PcmPlayer {
public:
    PcmPlayer(Settings& settings, MqttClient& mqtt);

    // Spawn the worker task + job queue. Call once.
    void start();

    // Enqueue a "play this URL" request. url="" falls back to
    // Settings::playUrl. Returns false (and logs) if the queue is full.
    bool play(const std::string& url = "");

private:
    static void workerTrampoline(void* arg);
    void worker();
    void process(const PlayJob& job);

    Settings&     settings_;
    MqttClient&   mqtt_;
    QueueHandle_t queue_ = nullptr;
};
