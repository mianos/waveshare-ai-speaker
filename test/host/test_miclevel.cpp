// Host-side tests for the mic level meter maths (main/MicLevel.h).
//
// The mic array / AFE feed itself is hardware-in-the-loop, but the dBFS and
// interleaved-channel extraction are pure: a wrong channel stride would read
// the wrong mic (or the reference/unused slot), and a wrong dB formula would
// make the level log useless for aiming/gaining the array.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "MicLevel.h"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void test_full_scale_is_zero_dbfs() {
    std::vector<int16_t> frame(100, 32767);
    miclevel::ChannelLevel lvl = miclevel::channelLevel(frame.data(), 100, 1, 0);
    CHECK(std::abs(lvl.rmsDb) < 0.01f);
    CHECK(std::abs(lvl.peakDb) < 0.01f);
}

static void test_silence_clamps_low() {
    std::vector<int16_t> frame(100, 0);
    miclevel::ChannelLevel lvl = miclevel::channelLevel(frame.data(), 100, 1, 0);
    CHECK(lvl.rmsDb <= -90.0f);
    CHECK(lvl.peakDb <= -90.0f);
}

static void test_half_scale_is_about_minus_6db() {
    std::vector<int16_t> frame(200, 16384);  // 0.5 full-scale
    miclevel::ChannelLevel lvl = miclevel::channelLevel(frame.data(), 200, 1, 0);
    CHECK(std::abs(lvl.rmsDb - (-6.02f)) < 0.1f);
    CHECK(std::abs(lvl.peakDb - (-6.02f)) < 0.1f);
}

static void test_peak_ge_rms() {
    // A single loud spike among many quiet samples: peak must reflect the
    // spike, rms must stay well below it (diluted by the quiet majority).
    std::vector<int16_t> frame(256, 100);
    frame[10] = 32000;
    miclevel::ChannelLevel lvl = miclevel::channelLevel(frame.data(), 256, 1, 0);
    CHECK(lvl.peakDb > lvl.rmsDb + 20.0f);
}

static void test_interleaved_channel_selection() {
    // 3 channels, 4 frames: channel 0 is loud, channel 1 is silent, channel 2
    // is half-scale. Picking the wrong stride/offset would blend or swap these.
    const int channels = 3;
    const int frames = 4;
    std::vector<int16_t> buf(channels * frames);
    for (int i = 0; i < frames; ++i) {
        buf[i * channels + 0] = 30000;
        buf[i * channels + 1] = 0;
        buf[i * channels + 2] = 16384;
    }

    miclevel::ChannelLevel ch0 = miclevel::channelLevel(buf.data(), frames, channels, 0);
    miclevel::ChannelLevel ch1 = miclevel::channelLevel(buf.data(), frames, channels, 1);
    miclevel::ChannelLevel ch2 = miclevel::channelLevel(buf.data(), frames, channels, 2);

    CHECK(ch0.rmsDb > ch2.rmsDb);
    CHECK(ch2.rmsDb > ch1.rmsDb);
    CHECK(ch1.rmsDb <= -90.0f);
}

static void test_zero_frame_count_is_safe() {
    std::vector<int16_t> frame(4, 1234);
    miclevel::ChannelLevel lvl = miclevel::channelLevel(frame.data(), 0, 1, 0);
    CHECK(lvl.rmsDb <= -90.0f);
    CHECK(lvl.peakDb <= -90.0f);
}

// The mic level log now accumulates across every feed chunk spanning a wake
// capture (see VoicePipeline::captureTask), not a single chunk — this must
// give the same answer as computing the level over the whole event in one
// shot, or the "levels for this recording" summary would be wrong.
static void test_streaming_accumulate_matches_batch() {
    const int frames1 = 200, frames2 = 150;
    std::vector<int16_t> chunk1(frames1), chunk2(frames2), whole(frames1 + frames2);
    for (int i = 0; i < frames1; ++i) chunk1[i] = whole[i] = (int16_t)(1000 + i * 37 % 5000);
    for (int i = 0; i < frames2; ++i) {
        int16_t v = (int16_t)(-2000 - i * 53 % 6000);
        chunk2[i] = v;
        whole[frames1 + i] = v;
    }

    miclevel::ChannelAccum acc;
    miclevel::accumulate(acc, chunk1.data(), frames1, 1, 0);
    miclevel::accumulate(acc, chunk2.data(), frames2, 1, 0);
    miclevel::ChannelLevel streamed = miclevel::finishLevel(acc);

    miclevel::ChannelLevel batched = miclevel::channelLevel(whole.data(), frames1 + frames2, 1, 0);

    CHECK(std::abs(streamed.rmsDb - batched.rmsDb) < 0.001f);
    CHECK(std::abs(streamed.peakDb - batched.peakDb) < 0.001f);
}

static void test_accum_reset_and_empty() {
    miclevel::ChannelAccum acc;
    miclevel::ChannelLevel lvl = miclevel::finishLevel(acc);
    CHECK(lvl.rmsDb <= -90.0f);
    CHECK(lvl.peakDb <= -90.0f);

    // Reassigning a fresh ChannelAccum (as captureTask does at the start of
    // each wake capture) must not carry over a previous event's peak/energy.
    acc.sumSq = 1e12;
    acc.peak  = 32000;
    acc.count = 999;
    acc = miclevel::ChannelAccum{};
    lvl = miclevel::finishLevel(acc);
    CHECK(lvl.rmsDb <= -90.0f);
    CHECK(lvl.peakDb <= -90.0f);
}

int main() {
    test_full_scale_is_zero_dbfs();
    test_silence_clamps_low();
    test_half_scale_is_about_minus_6db();
    test_peak_ge_rms();
    test_interleaved_channel_selection();
    test_zero_frame_count_is_safe();
    test_streaming_accumulate_matches_batch();
    test_accum_reset_and_empty();

    if (failures == 0) {
        std::printf("test_miclevel: all tests passed\n");
        return 0;
    }
    std::printf("test_miclevel: %d failure(s)\n", failures);
    return 1;
}
