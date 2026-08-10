#pragma once

#include <ctime>
#include <string>

#include "esp_timer.h"

class MqttClient;
class TtsClient;

// A single named countdown timer ("computer create a 10 minute timer for the
// pasta"). Only one runs at a time -- starting a new one replaces whatever
// was already running. Scheduling lives entirely on-device (esp_timer,
// one-shot + an optional periodic announce timer) so it survives Node-RED
// flow redeploys; MQTT is only used for the spoken alerts (via TtsClient)
// and for publishing status so Node-RED can answer "how long is left"
// without polling the device.
class TimerManager {
public:
    TimerManager(MqttClient& mqtt, TtsClient& tts, std::string sensorName);
    ~TimerManager();

    // label may be empty ("set a timer for 10 minutes") or set ("... for the
    // pasta"). announce, if true, speaks the remaining time once a minute
    // until expiry. Replaces any timer already running.
    void start(int seconds, const std::string& label, bool announce);

    // No-op (logged) if nothing is running.
    void cancel();

private:
    static void onExpireTrampoline(void* arg);
    static void onAnnounceTrampoline(void* arg);
    void onExpire();
    void onAnnounce();
    void publishStatus() const;

    MqttClient&        mqtt_;
    TtsClient&         tts_;
    std::string        sensorName_;
    esp_timer_handle_t handle_ = nullptr;          // one-shot: fires on expiry
    esp_timer_handle_t announceHandle_ = nullptr;  // periodic: fires every 60s while announce_
    time_t             endsAt_ = 0;                // 0 = no timer active
    std::string        label_;
    bool               announce_ = false;
};
