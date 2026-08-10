#include "TimerManager.h"

#include "esp_log.h"

#include "JsonWrapper.h"
#include "MqttClient.h"
#include "TtsClient.h"

namespace {
constexpr const char* TAG = "timer";
constexpr uint64_t kAnnouncePeriodUs = 60ULL * 1000000ULL;
// Skip an announcement this close to expiry -- the final "time's up" alert
// covers it, avoiding two TTS messages back-to-back on round durations.
constexpr time_t kAnnounceSkipWindowSec = 5;
}  // namespace

TimerManager::TimerManager(MqttClient& mqtt, TtsClient& tts, std::string sensorName)
    : mqtt_(mqtt), tts_(tts), sensorName_(std::move(sensorName)) {
    esp_timer_create_args_t args = {};
    args.callback = &TimerManager::onExpireTrampoline;
    args.arg      = this;
    args.name     = "voice_timer";
    esp_timer_create(&args, &handle_);

    esp_timer_create_args_t announceArgs = {};
    announceArgs.callback = &TimerManager::onAnnounceTrampoline;
    announceArgs.arg      = this;
    announceArgs.name     = "voice_timer_announce";
    esp_timer_create(&announceArgs, &announceHandle_);
}

TimerManager::~TimerManager() {
    if (handle_) {
        esp_timer_stop(handle_);
        esp_timer_delete(handle_);
    }
    if (announceHandle_) {
        esp_timer_stop(announceHandle_);
        esp_timer_delete(announceHandle_);
    }
}

void TimerManager::start(int seconds, const std::string& label, bool announce) {
    if (esp_timer_is_active(handle_)) {
        esp_timer_stop(handle_);
    }
    if (esp_timer_is_active(announceHandle_)) {
        esp_timer_stop(announceHandle_);
    }
    label_    = label;
    announce_ = announce;
    endsAt_   = time(nullptr) + seconds;
    esp_timer_start_once(handle_, (uint64_t)seconds * 1000000ULL);
    if (announce_) {
        esp_timer_start_periodic(announceHandle_, kAnnouncePeriodUs);
    }
    ESP_LOGI(TAG, "started: %ds label='%s' announce=%d", seconds, label_.c_str(), (int)announce_);
    publishStatus();
}

void TimerManager::cancel() {
    if (!esp_timer_is_active(handle_)) {
        ESP_LOGI(TAG, "cancel: no timer running");
        return;
    }
    esp_timer_stop(handle_);
    if (esp_timer_is_active(announceHandle_)) {
        esp_timer_stop(announceHandle_);
    }
    endsAt_ = 0;
    label_.clear();
    announce_ = false;
    ESP_LOGI(TAG, "cancelled");
    publishStatus();
}

void TimerManager::onExpireTrampoline(void* arg) {
    static_cast<TimerManager*>(arg)->onExpire();
}

void TimerManager::onAnnounceTrampoline(void* arg) {
    static_cast<TimerManager*>(arg)->onAnnounce();
}

void TimerManager::onExpire() {
    if (esp_timer_is_active(announceHandle_)) {
        esp_timer_stop(announceHandle_);
    }
    std::string text = label_.empty() ? "Your timer is up."
                                       : "Your " + label_ + " timer is up.";
    ESP_LOGI(TAG, "expired: %s", text.c_str());
    tts_.speak(text);
    endsAt_ = 0;
    label_.clear();
    announce_ = false;
    publishStatus();
}

void TimerManager::onAnnounce() {
    if (endsAt_ == 0) return;
    time_t remaining = endsAt_ - time(nullptr);
    if (remaining <= kAnnounceSkipWindowSec) return;

    std::string whose = label_.empty() ? "your timer" : "your " + label_ + " timer";
    std::string text;
    if (remaining >= 60) {
        long minutes = (remaining + 30) / 60;  // round to nearest minute
        text = std::to_string(minutes) + " minute" + (minutes == 1 ? "" : "s") + " left on " + whose + ".";
    } else {
        text = std::to_string((long)remaining) + " second" + (remaining == 1 ? "" : "s") + " left on " + whose + ".";
    }
    ESP_LOGI(TAG, "announce: %s", text.c_str());
    tts_.speak(text);
}

void TimerManager::publishStatus() const {
    JsonWrapper doc;
    doc.AddItem("active",   endsAt_ != 0);
    doc.AddItem("ends_at",  (int64_t)endsAt_);
    doc.AddItem("label",    label_);
    doc.AddItem("announce", announce_);
    mqtt_.publish("tele/" + sensorName_ + "/timer", doc.ToString());
}
