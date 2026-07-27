#pragma once

// Minimal public interface for the Waveshare ESP32-S3 audio board mic path.
// Deliberately free of esp_codec_dev / i2s types so consumers (the esp-sr feed
// task) don't inherit the codec headers — they only need raw int16 frames.
//
// Ported from the board vendor demo (ESP32-S3-AUDIO-Board-Demo/esp_sr_02),
// trimmed to input only: I2C control bus, I2S_NUM_1, the ES7210 4-mic ADC, and
// the TCA9555 I/O-expander that gates board power/PA. The ES8311 speaker DAC and
// SD-card paths from the demo are intentionally omitted (mic-only firmware).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2C, I2S RX, the ES7210 ADC and the TCA9555 expander. Call once,
// before starting the esp-sr feed task.
esp_err_t bsp_board_init(void);

// Set the ES7210's analog mic PGA gain (dB, all 4 channels) — the actual
// hardware pickup level, distinct from any software gain applied later in the
// AFE pipeline. The codec quantises to its supported steps (0-37.5dB in
// roughly 3dB steps up to 30dB, then 30/34.5/36/37.5). Safe to call at any
// time after bsp_board_init() succeeds — it's a live I2C register write, no
// I2S/AFE re-init needed. No-op returning ESP_ERR_INVALID_STATE if the mic
// hasn't been brought up.
esp_err_t bsp_mic_set_gain(float db);

// Bring up the ES8311 speaker DAC for tone playback. Best-effort and INDEPENDENT
// of bsp_board_init(): call it after bsp_board_init() and before the mic feed
// task starts. Never aborts the caller — returns an error (logged) if the DAC is
// unavailable, leaving the mic path untouched. Safe to skip entirely.
esp_err_t bsp_audio_out_init(void);

// Set the ES8311 output volume (0-100). A live esp_codec_dev register write,
// same as bsp_mic_set_gain() -- safe to call any time after
// bsp_audio_out_init() succeeds, no reinit needed. No-op returning
// ESP_ERR_INVALID_STATE if the DAC hasn't been brought up.
esp_err_t bsp_speaker_set_volume(int vol);

// Play a mono 16-bit / 16 kHz PCM buffer out the speaker (blocks until written,
// gating the power amp for the duration). No-op returning ESP_ERR_INVALID_STATE
// if bsp_audio_out_init() has not succeeded.
esp_err_t bsp_audio_play_mono16(const int16_t *pcm, size_t nsamples);

// Incremental playback for one long/streamed clip (e.g. TTS audio arriving
// over HTTP in chunks), as an alternative to bsp_audio_play_mono16() when the
// whole clip isn't available up front. Keeps the power amp enabled and the
// board's playback lock held across the whole sequence instead of toggling
// per call, which would click and add ~10-100ms latency between chunks of
// what should be one continuous sound.
//
// Call bsp_audio_stream_begin() once, then bsp_audio_stream_write() any
// number of times with successive chunks (each a whole number of samples),
// then bsp_audio_stream_end() once. Not reentrant -- one stream at a time,
// same as bsp_audio_play_mono16(). All three no-op with
// ESP_ERR_INVALID_STATE if bsp_audio_out_init() has not succeeded.
esp_err_t bsp_audio_stream_begin(void);
esp_err_t bsp_audio_stream_write(const int16_t *pcm, size_t nsamples);
esp_err_t bsp_audio_stream_end(void);

// Bring up the on-board WS2812 RGB strip. Best-effort; on failure bsp_led_set()
// no-ops. Call once, after bsp_board_init().
esp_err_t bsp_led_init(void);

// Set the whole strip to an RGB colour (0,0,0 clears it). No-op if the strip
// was not brought up. Safe to call from any task.
void bsp_led_set(uint8_t r, uint8_t g, uint8_t b);

// Read one AFE feed chunk of interleaved microphone audio into `buffer`.
// buffer_len is in bytes and must be a whole number of frames
// (sizeof(int16_t) * esp_get_feed_channel()). Pass is_get_raw_channel=true to
// hand the AFE the raw multi-channel stream (what the feed task wants).
esp_err_t esp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);

// Number of interleaved channels produced by esp_get_feed_data (4: 2 mics +
// playback reference + idle, matching esp_get_input_format()).
int esp_get_feed_channel(void);

// AFE channel layout string for afe_config_init(), e.g. "RMNM".
const char *esp_get_input_format(void);

#ifdef __cplusplus
}
#endif
