#pragma once

#include "SettingsBase.h"

// ws-voice settings schema — persistence/reset/log machinery lives in mianesp's
// settingsbase. Member initialisers are the compiled-in defaults; missing or
// unparseable NVS config falls back to them (logged).
//
// SettingsBase only stores std::string and int fields, so the two booleans are
// modelled as 0/1 ints.
//
// sttUrl is intentionally empty by default: there is no STT server yet, and the
// capture pipeline fail-fast refuses (and logs) to POST until it is set — via
// `cmnd/<name>/settings` or `POST /config`, e.g.
//   {"stt_url": "http://host:8000/v1/audio/transcriptions"}
struct Settings : SettingsBase {
    std::string mqttServer  = "mqtt2.mianos.com";
    int         mqttPort    = 1883;
    std::string sensorName  = "wsvoice";
    std::string tz          = "AEST-10AEDT,M10.1.0,M4.1.0/3";

    // STT (OpenAI-compatible multipart transcription endpoint).
    std::string sttUrl      = "";           // empty ⇒ STT disabled (fail-fast)
    std::string sttModel    = "whisper-1";  // model form field
    std::string sttLanguage = "en";         // language hint; "" omits the field

    // Capture end-of-utterance tuning.
    int vadSilenceMs     = 700;    // stop after this much continuous silence
    int maxCaptureMs     = 8000;   // hard cap on a single utterance
    int publishWakeEvent = 1;      // 1 ⇒ publish tele/<name>/wake on trigger
    // Speaker tones (start + connected). ES8311 bring-up is non-fatal and runs
    // after (never inside) the mic init, so a DAC failure can't stop the voice
    // path — hence this is on by default. Set 0 to skip the ES8311 entirely
    // (boot path then identical to the mic-only firmware).
    int playTone         = 1;      // 1 ⇒ bring up ES8311 + play start/connected tones

    explicit Settings(NvsStorageManager& nvs) : SettingsBase(nvs) {
        field("mqtt_server",  mqttServer);
        field("mqtt_port",    mqttPort);
        field("sensor_name",  sensorName);
        field("tz",           tz);
        field("stt_url",      sttUrl);
        field("stt_model",    sttModel);
        field("stt_language", sttLanguage);
        field("vad_silence_ms", vadSilenceMs);
        field("max_capture_ms", maxCaptureMs);
        field("publish_wake",   publishWakeEvent);
        field("play_tone",      playTone);
        load();
    }
};
