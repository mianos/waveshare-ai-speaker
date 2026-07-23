// Host-side tests for the STT wire builders (main/SttWire.h).
//
// These bytes go on the wire to the STT server around the captured PCM; a
// malformed WAV header or multipart envelope means the server rejects the
// request (silent loss of every voice command). The audio itself is
// hardware-in-the-loop and can't be tested here, but the framing can and must.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include "SttWire.h"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        const auto a = (actual);                                          \
        const auto e = (expected);                                        \
        if (a != e) {                                                     \
            std::printf("FAIL %s:%d  expected %lld, got %lld\n",          \
                        __FILE__, __LINE__, (long long)e, (long long)a);  \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static uint32_t le32(const std::string& s, size_t off) {
    return (uint8_t)s[off] | ((uint8_t)s[off + 1] << 8) |
           ((uint8_t)s[off + 2] << 16) | ((uint32_t)(uint8_t)s[off + 3] << 24);
}
static uint16_t le16(const std::string& s, size_t off) {
    return (uint8_t)s[off] | ((uint8_t)s[off + 1] << 8);
}
static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static void test_wav_header() {
    // 1000 samples of 16-bit mono @16kHz => 2000 data bytes.
    const uint32_t samples = 1000, rate = 16000, dataBytes = samples * 2;
    std::string h = sttwire::wavHeader(samples, rate);

    CHECK_EQ(h.size(), 44u);
    CHECK(h.substr(0, 4) == "RIFF");
    CHECK_EQ(le32(h, 4), 36u + dataBytes);   // RIFF chunk size = 36 + data
    CHECK(h.substr(8, 4) == "WAVE");
    CHECK(h.substr(12, 4) == "fmt ");
    CHECK_EQ(le32(h, 16), 16u);              // PCM fmt chunk size
    CHECK_EQ(le16(h, 20), 1u);               // audio format = PCM
    CHECK_EQ(le16(h, 22), 1u);               // channels = mono
    CHECK_EQ(le32(h, 24), rate);
    CHECK_EQ(le32(h, 28), rate * 2u);        // byte rate = rate*channels*2
    CHECK_EQ(le16(h, 32), 2u);               // block align = channels*2
    CHECK_EQ(le16(h, 34), 16u);              // bits per sample
    CHECK(h.substr(36, 4) == "data");
    CHECK_EQ(le32(h, 40), dataBytes);        // data chunk size = samples*2

    // Empty capture: header still valid, data size 0, RIFF size 36.
    std::string z = sttwire::wavHeader(0, rate);
    CHECK_EQ(z.size(), 44u);
    CHECK_EQ(le32(z, 40), 0u);
    CHECK_EQ(le32(z, 4), 36u);

    // Rate flows into byte rate at a different sample rate.
    std::string s8 = sttwire::wavHeader(10, 8000);
    CHECK_EQ(le32(s8, 24), 8000u);
    CHECK_EQ(le32(s8, 28), 16000u);          // 8000 * 1 * 2
}

static void test_multipart() {
    const std::string boundary = "----wsvoiceFormBoundary8x9Kq2mLp";

    std::string head = sttwire::multipartHead(boundary, "audio.wav");
    CHECK(head == "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                  "Content-Type: audio/wav\r\n"
                  "\r\n");

    // Tail with a language field.
    std::string tail = sttwire::multipartTail(boundary, "whisper-1", "en");
    CHECK(tail == "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"model\"\r\n"
                  "\r\nwhisper-1\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"language\"\r\n"
                  "\r\nen\r\n"
                  "--" + boundary + "--\r\n");

    // Empty language omits the language part entirely.
    std::string noLang = sttwire::multipartTail(boundary, "whisper-1", "");
    CHECK(!contains(noLang, "name=\"language\""));
    CHECK(contains(noLang, "name=\"model\""));
    CHECK(contains(noLang, "whisper-1"));
    // Ends with the multipart terminator.
    const std::string terminator = "--" + boundary + "--\r\n";
    CHECK(noLang.size() >= terminator.size() &&
          noLang.substr(noLang.size() - terminator.size()) == terminator);

    // Content-Type header carries the boundary.
    CHECK(sttwire::contentType(boundary) == "multipart/form-data; boundary=" + boundary);
}

int main() {
    test_wav_header();
    test_multipart();

    if (failures == 0) {
        std::printf("test_wav_builder: all tests passed\n");
        return 0;
    }
    std::printf("test_wav_builder: %d failure(s)\n", failures);
    return 1;
}
