#pragma once

#include <string>

// Builds the JSON body for FastKoko's (Kokoro-FastAPI) OpenAI-compatible
// POST /v1/audio/speech endpoint. response_format is fixed to "pcm" — raw
// 16-bit samples, no container, no decoder needed on-device. The server is
// expected to already resample to 16 kHz mono (ws-voice's tts_url carries
// ?sample_rate=16000; see README), so the device just streams the response
// straight into bsp_audio_play_mono16.
//
// Header-only and free of ESP-IDF dependencies: the input text arrives from
// MQTT/HTTP and is untrusted, so the escaping is host-tested (test/host).
namespace tts {

inline std::string escapeJson(const std::string& in) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xf];
                    out += hex[c & 0xf];
                } else {
                    out += (char)c;  // UTF-8 bytes pass through
                }
        }
    }
    return out;
}

inline std::string requestBody(const std::string& text, const std::string& voice) {
    return "{\"model\":\"kokoro\",\"input\":\"" + escapeJson(text) +
           "\",\"voice\":\"" + escapeJson(voice) +
           "\",\"response_format\":\"pcm\"}";
}

}  // namespace tts
