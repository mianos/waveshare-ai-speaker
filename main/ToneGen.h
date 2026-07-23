#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Pure PCM tone synthesis for the on-device wake chime.
//
// Deliberately free of any hardware / codec types so it can be unit-tested on
// the host (see test/host/test_tonegen.cpp). The board layer
// (bsp_audio_play_mono16) owns the ES8311 DAC, the 16->32-bit I2S slot
// expansion and the power-amp enable; this header only decides what samples to
// emit.
namespace tonegen {

// Sine peak, kept off the int16 rails so fades and the DAC don't clip.
constexpr int16_t kDefaultAmplitude = 12000;

// pi as a literal — M_PI is a GNU/POSIX extension, not ISO C++, so we don't
// rely on <cmath> exposing it.
constexpr double kPi = 3.14159265358979323846;

// One note in a chime.
struct Note {
    float    freqHz;
    uint32_t durationMs;
};

// Number of int16 samples a note of `durationMs` occupies at `sampleRate`.
inline size_t noteSamples(uint32_t durationMs, uint32_t sampleRate) {
    return static_cast<size_t>((static_cast<uint64_t>(durationMs) * sampleRate) / 1000u);
}

// Fill out[0..count) with a mono sine at `freqHz`, with a linear fade-in and
// fade-out of `fadeSamples` at each end. The fade is what stops the abrupt
// on/off from clicking; a linear ramp is enough here and stays trivial to test.
// `fadeSamples` is clamped to count/2 so the two ramps never overlap.
inline void fillNote(int16_t* out, size_t count, float freqHz,
                     uint32_t sampleRate, int16_t amplitude, size_t fadeSamples) {
    if (count == 0 || out == nullptr) return;
    if (fadeSamples > count / 2) fadeSamples = count / 2;
    const double w = 2.0 * kPi * static_cast<double>(freqHz) / sampleRate;
    for (size_t i = 0; i < count; ++i) {
        double env = 1.0;
        if (fadeSamples > 0) {
            if (i < fadeSamples) {
                env = static_cast<double>(i) / static_cast<double>(fadeSamples);
            } else if (i >= count - fadeSamples) {
                env = static_cast<double>(count - 1 - i) / static_cast<double>(fadeSamples);
            }
        }
        double s = std::sin(w * static_cast<double>(i)) * amplitude * env;
        long v = std::lround(s);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[i] = static_cast<int16_t>(v);
    }
}

// Total int16 samples a sequence of notes will render to.
inline size_t chimeSamples(const Note* notes, size_t nnotes, uint32_t sampleRate) {
    size_t total = 0;
    for (size_t i = 0; i < nnotes; ++i) total += noteSamples(notes[i].durationMs, sampleRate);
    return total;
}

// Render `notes` back-to-back into `out` (which must hold at least
// chimeSamples() samples). Each note fades out to zero and the next fades in
// from zero, so the joins are click-free. `fadeMs` is the per-note ramp length.
// Returns the number of samples written.
inline size_t renderChime(int16_t* out, const Note* notes, size_t nnotes,
                          uint32_t sampleRate, int16_t amplitude, uint32_t fadeMs) {
    const size_t fade = noteSamples(fadeMs, sampleRate);
    size_t off = 0;
    for (size_t i = 0; i < nnotes; ++i) {
        const size_t n = noteSamples(notes[i].durationMs, sampleRate);
        fillNote(out + off, n, notes[i].freqHz, sampleRate, amplitude, fade);
        off += n;
    }
    return off;
}

}  // namespace tonegen
