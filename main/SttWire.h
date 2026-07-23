#pragma once

#include <cstdint>
#include <string>

// Pure builders for the STT request wire format. No ESP-IDF dependencies so they
// are unit-tested on the host (test/host). SttClient streams these pieces to the
// server around the raw PCM so a multi-hundred-KB body never has to exist as one
// contiguous buffer:
//
//   [multipartHead] [wavHeader] [raw PCM bytes] [multipartTail]
//
// The whole thing is a single multipart/form-data body: a "file" part carrying a
// canonical 16-bit mono PCM WAV, plus "model" and optional "language" fields —
// the shape faster-whisper-server / speaches / whisper.cpp and the OpenAI
// /v1/audio/transcriptions API all accept.
namespace sttwire {

inline void appendLE16(std::string& out, uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

inline void appendLE32(std::string& out, uint32_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>((v >> 16) & 0xff);
    out += static_cast<char>((v >> 24) & 0xff);
}

// 44-byte canonical WAV header for `num_samples` of 16-bit mono PCM at
// `sample_rate`. The PCM samples follow immediately after.
inline std::string wavHeader(uint32_t num_samples, uint32_t sample_rate) {
    const uint16_t channels       = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t data_bytes      = num_samples * channels * (bits_per_sample / 8);
    const uint32_t byte_rate       = sample_rate * channels * (bits_per_sample / 8);
    const uint16_t block_align     = channels * (bits_per_sample / 8);

    std::string h;
    h.reserve(44);
    h += "RIFF";
    appendLE32(h, 36 + data_bytes);   // RIFF chunk size
    h += "WAVE";
    h += "fmt ";
    appendLE32(h, 16);                // fmt chunk size (PCM)
    appendLE16(h, 1);                 // audio format = PCM
    appendLE16(h, channels);
    appendLE32(h, sample_rate);
    appendLE32(h, byte_rate);
    appendLE16(h, block_align);
    appendLE16(h, bits_per_sample);
    h += "data";
    appendLE32(h, data_bytes);
    return h;
}

// Bytes of the multipart body that precede the WAV/PCM (opening boundary and the
// "file" part headers).
inline std::string multipartHead(const std::string& boundary, const std::string& filename) {
    return "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
           "Content-Type: audio/wav\r\n"
           "\r\n";
}

// Bytes of the multipart body that follow the WAV/PCM: closes the file part,
// then the "model" field and (if non-empty) "language", then the terminator.
inline std::string multipartTail(const std::string& boundary, const std::string& model,
                                 const std::string& language) {
    std::string t = "\r\n"
                    "--" + boundary + "\r\n"
                    "Content-Disposition: form-data; name=\"model\"\r\n"
                    "\r\n" + model + "\r\n";
    if (!language.empty()) {
        t += "--" + boundary + "\r\n"
             "Content-Disposition: form-data; name=\"language\"\r\n"
             "\r\n" + language + "\r\n";
    }
    t += "--" + boundary + "--\r\n";
    return t;
}

// Value for the request's Content-Type header.
inline std::string contentType(const std::string& boundary) {
    return "multipart/form-data; boundary=" + boundary;
}

}  // namespace sttwire
