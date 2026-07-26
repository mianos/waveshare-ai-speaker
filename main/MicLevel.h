#pragma once

// Pure, host-testable RMS/peak level metering for one interleaved channel of
// a multi-channel int16 PCM frame. Used by VoicePipeline's optional mic level
// log (mic_level_log setting) to help aim/gain the near-field mic array.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace miclevel {

struct ChannelLevel {
    float rmsDb;
    float peakDb;
};

// dBFS relative to full-scale int16 (32768). Silence clamps to -100 dB rather
// than -inf so it prints/compares sanely.
inline double toDbfs(double amplitude) {
    return 20.0 * std::log10(std::max(amplitude, 1.0) / 32768.0);
}

// Running sum-of-squares/peak/count for one channel, built up across however
// many feed chunks span an event (e.g. one wake capture) so the resulting
// level reflects that whole event instead of whatever a periodic timer
// happened to sample.
struct ChannelAccum {
    double  sumSq = 0.0;
    int16_t peak  = 0;
    size_t  count = 0;
};

// channelIdx indexes one of `channels` interleaved channels in `frame`
// (frameSamples samples per channel, `frame` holding frameSamples*channels
// int16s total). Adds this chunk's contribution to `acc`.
inline void accumulate(ChannelAccum& acc, const int16_t* frame, int frameSamples, int channels, int channelIdx) {
    for (int i = 0; i < frameSamples; ++i) {
        int16_t s = frame[i * channels + channelIdx];
        acc.sumSq += (double)s * (double)s;
        int16_t a = (int16_t)std::abs((int)s);
        if (a > acc.peak) acc.peak = a;
    }
    acc.count += (size_t)frameSamples;
}

inline ChannelLevel finishLevel(const ChannelAccum& acc) {
    if (acc.count == 0) return {-100.0f, -100.0f};
    const double rms = std::sqrt(acc.sumSq / acc.count);
    return {(float)toDbfs(rms), (float)toDbfs(acc.peak)};
}

// One-shot convenience: level of a single chunk, with no running state.
inline ChannelLevel channelLevel(const int16_t* frame, int frameSamples, int channels, int channelIdx) {
    ChannelAccum acc;
    accumulate(acc, frame, frameSamples, channels, channelIdx);
    return finishLevel(acc);
}

}  // namespace miclevel
