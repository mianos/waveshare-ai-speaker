// Host-side tests for the wake-chime synthesis (main/ToneGen.h).
//
// The audio path itself is hardware-in-the-loop and can't be tested here, but
// the sample maths can: a bad envelope clicks or clips, a bad length sends the
// DAC the wrong number of samples. This locks the pure part down.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ToneGen.h"

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
        const long long a = (long long)(actual);                         \
        const long long e = (long long)(expected);                       \
        if (a != e) {                                                     \
            std::printf("FAIL %s:%d  expected %lld, got %lld\n",          \
                        __FILE__, __LINE__, e, a);                        \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void test_note_samples() {
    CHECK_EQ(tonegen::noteSamples(1000, 16000), 16000);
    CHECK_EQ(tonegen::noteSamples(100, 16000), 1600);
    CHECK_EQ(tonegen::noteSamples(0, 16000), 0);
    // Rounds toward zero (integer division) — a partial sample is never emitted.
    CHECK_EQ(tonegen::noteSamples(1, 16000), 16);
}

static void test_fade_endpoints_and_amplitude() {
    const uint32_t rate = 16000;
    const int16_t amp = 12000;
    const size_t n = tonegen::noteSamples(100, rate);  // 1600
    const size_t fade = tonegen::noteSamples(5, rate);  // 80
    std::vector<int16_t> buf(n, 32767);

    tonegen::fillNote(buf.data(), n, 880.0f, rate, amp, fade);

    // Endpoints must be exactly zero (fade env = 0), or it clicks.
    CHECK_EQ(buf.front(), 0);
    CHECK_EQ(buf.back(), 0);

    // Never exceeds the requested amplitude; and the tone is actually present
    // (peak reaches most of the amplitude somewhere in the sustain).
    int16_t peak = 0;
    for (int16_t s : buf) {
        int16_t a = (s < 0) ? (int16_t)-s : s;
        if (a > peak) peak = a;
    }
    CHECK(peak <= amp);
    CHECK(peak >= (int16_t)(amp * 0.8));
}

static void test_sustain_matches_sine() {
    const uint32_t rate = 16000;
    const int16_t amp = 10000;
    const size_t n = tonegen::noteSamples(100, rate);
    const size_t fade = tonegen::noteSamples(5, rate);
    std::vector<int16_t> buf(n, 0);
    tonegen::fillNote(buf.data(), n, 1000.0f, rate, amp, fade);

    // A sample well inside the sustain region should equal amp*sin(w*i).
    const size_t i = n / 2;
    const double w = 2.0 * tonegen::kPi * 1000.0 / rate;
    const long expected = std::lround(std::sin(w * (double)i) * amp);
    CHECK(std::abs((long)buf[i] - expected) <= 1);
}

static void test_fade_clamped() {
    // fadeSamples larger than count/2 must be clamped, not read out of bounds,
    // and endpoints still zero.
    const size_t n = 10;
    std::vector<int16_t> buf(n, 123);
    tonegen::fillNote(buf.data(), n, 440.0f, 16000, 12000, /*fade=*/9999);
    CHECK_EQ(buf.front(), 0);
    CHECK_EQ(buf.back(), 0);
    for (int16_t s : buf) CHECK(s >= -12000 && s <= 12000);

    // count == 0 is a no-op (must not crash / write).
    tonegen::fillNote(buf.data(), 0, 440.0f, 16000, 12000, 4);
}

static void test_render_chime() {
    const uint32_t rate = 16000;
    const tonegen::Note notes[] = {{880.0f, 90}, {1320.0f, 110}};
    const size_t expected = tonegen::noteSamples(90, rate) + tonegen::noteSamples(110, rate);
    CHECK_EQ(tonegen::chimeSamples(notes, 2, rate), expected);

    std::vector<int16_t> buf(expected, 32767);
    const size_t written = tonegen::renderChime(buf.data(), notes, 2, rate, 12000, 5);
    CHECK_EQ(written, expected);

    // Each note boundary is click-free: last sample of note 1 and first of
    // note 2 are both zero (fade-out then fade-in).
    const size_t n1 = tonegen::noteSamples(90, rate);
    CHECK_EQ(buf[0], 0);              // start of chime
    CHECK_EQ(buf[n1 - 1], 0);         // end of note 1
    CHECK_EQ(buf[n1], 0);             // start of note 2
    CHECK_EQ(buf[expected - 1], 0);   // end of chime
}

int main() {
    test_note_samples();
    test_fade_endpoints_and_amplitude();
    test_sustain_matches_sine();
    test_fade_clamped();
    test_render_chime();

    if (failures == 0) {
        std::printf("test_tonegen: all tests passed\n");
        return 0;
    }
    std::printf("test_tonegen: %d failure(s)\n", failures);
    return 1;
}
