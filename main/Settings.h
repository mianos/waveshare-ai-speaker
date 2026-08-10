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

    // TTS ("say" command): FastKoko (Kokoro-FastAPI) OpenAI-compatible speech
    // endpoint. The device requests response_format=pcm (raw 16-bit, no
    // container) and never resamples — ?sample_rate=16000 in the URL is a
    // FastKoko extension that makes the server deliver audio already at the
    // board's native rate. say refuses (and logs) if this is blanked.
    std::string ttsUrl   = "http://docker-host.mianos.com:8880/v1/audio/speech?sample_rate=16000";
    std::string ttsVoice = "af_heart";

    // "play" command: URL of a raw-PCM cue clip (16 kHz mono s16le, no
    // container — the board's native format; see the README's ffmpeg recipe)
    // used when a play command arrives without a "url" field. The pcm/
    // directory on the web host holds pre-converted clips.
    std::string playUrl  = "http://mqtt2.mianos.com/pcm/plucky.pcm";

    // Capture end-of-utterance tuning.
    int vadSilenceMs     = 700;    // stop after this much continuous silence
    int maxCaptureMs     = 8000;   // hard cap on a single utterance
    int publishWakeEvent = 1;      // 1 ⇒ publish tele/<name>/wake on trigger

    // esp-sr AFE / near-field mic array tuning. -1 on any of the mode/level
    // fields means "leave esp-sr's own default for this hardware/input format
    // alone" — only a non-negative value is applied to the AFE config, so an
    // untouched install behaves exactly as before this setting existed.
    // wakenet_mode: det_mode_t — 0=90% normal, 1=95% aggressive, 2/3=2-channel
    // 90%/95% (this board has a 2-mic array), 4/5=3-channel. Higher = more
    // sensitive trigger, more false alarms.
    int wakenetMode      = -1;
    // vad_mode: vad_mode_t 0-4 (0=normal .. 4=very very very aggressive). A
    // *lower* mode reports speech more readily (good for a quiet/near-field
    // mic); higher rejects more as noise.
    int vadMode          = -1;
    // agc_target_level_dbfs: AGC target envelope, in -dBFS (esp-sr default 3).
    int agcTargetDbfs    = -1;
    // agc_compression_gain_db: fixed digital gain applied by AGC (default 9).
    int agcCompressionDb = -1;
    // afe_linear_gain: extra output gain multiplier, x100 (100 = 1.00x, valid
    // range 10-1000 = 0.1x-10x). Always applied (100 is a no-op). This is a
    // software multiplier on the AFE's already-processed *output* — it does
    // not improve pickup SNR (see mic_hw_gain_db for the actual hardware gain).
    int micGainX100      = 100;
    // ES7210 analog mic PGA gain, in dB (the real hardware pickup level,
    // applied at the ADC before any AFE processing). The codec quantises to
    // its own supported steps (0-37.5dB); see bsp_mic_set_gain(). Matches the
    // vendor demo's default of 30dB.
    int micHwGainDb      = 30;
    // 1 ⇒ log each mic channel's RMS/peak level (dBFS) for every wake
    // capture, to help aim/position the near-field array and pick a gain.
    int micLevelLog      = 0;
    // Speaker tones (start + connected). ES8311 bring-up is non-fatal and runs
    // after (never inside) the mic init, so a DAC failure can't stop the voice
    // path — hence this is on by default. Set 0 to skip the ES8311 entirely
    // (boot path then identical to the mic-only firmware).
    int playTone         = 1;      // 1 ⇒ bring up ES8311 + play start/connected tones
    // ES8311 output volume, 0-100 (matches the vendor demo's default of 70).
    // A live esp_codec_dev register write (see bsp_speaker_set_volume()) — no
    // reboot needed, unlike most of the esp-sr AFE tuning above.
    int speakerVolume    = 70;

    explicit Settings(NvsStorageManager& nvs) : SettingsBase(nvs) {
        field("mqtt_server",  mqttServer);
        field("mqtt_port",    mqttPort);
        field("sensor_name",  sensorName);
        field("tz",           tz);
        field("stt_url",      sttUrl);
        field("stt_model",    sttModel);
        field("stt_language", sttLanguage);
        field("tts_url",      ttsUrl);
        field("tts_voice",    ttsVoice);
        field("play_url",     playUrl);
        field("vad_silence_ms", vadSilenceMs);
        field("max_capture_ms", maxCaptureMs);
        field("publish_wake",   publishWakeEvent);
        field("play_tone",      playTone);
        field("speaker_volume", speakerVolume);
        field("wakenet_mode",      wakenetMode);
        field("vad_mode",          vadMode);
        field("agc_target_dbfs",   agcTargetDbfs);
        field("agc_compression_db", agcCompressionDb);
        field("mic_gain_x100",     micGainX100);
        field("mic_hw_gain_db",    micHwGainDb);
        field("mic_level_log",     micLevelLog);
        load();
    }
};
