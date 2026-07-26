#include "board.h"
#include "tca9555_driver.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"

static const char *TAG = "board";

// --- Waveshare ESP32-S3 audio board pinout (from the vendor demo) ---
#define I2C_PORT        (I2C_NUM_0)
#define GPIO_I2C_SCL    (GPIO_NUM_10)
#define GPIO_I2C_SDA    (GPIO_NUM_11)
#define GPIO_I2S_MCLK   (GPIO_NUM_12)
#define GPIO_I2S_BCLK   (GPIO_NUM_13)  // SCLK
#define GPIO_I2S_WS     (GPIO_NUM_14)  // LRCK
#define GPIO_I2S_DIN    (GPIO_NUM_15)  // SDIN, mic data in
#define GPIO_I2S_DOUT   (GPIO_NUM_16)  // speaker data out (ES8311 DAC)

// The ES7210 captures 4 TDM channels; the AFE consumes all four ("RMNM").
#define ADC_I2S_CHANNEL 4
// ES7210 mic capture gain (dB), matching the vendor demo.
#define RECORD_VOLUME   (30.0f)
// ES8311 output volume (0..100), matching the vendor demo.
#define PLAYER_VOLUME   (70)
// The speaker power amp is gated by TCA9555 output pin 8 (vendor demo). Held
// off except while a tone is playing.
#define SPK_PA_EXIO     (IO_EXPANDER_PIN_NUM_8)
// On-board WS2812 RGB strip (vendor demo: 7 LEDs on GPIO38).
#define LED_GPIO        (GPIO_NUM_38)
#define LED_COUNT       (7)

static i2s_chan_handle_t       tx_handle      = NULL;  // shared clock; drives ES8311 speaker
static i2s_chan_handle_t       rx_handle      = NULL;  // mic capture
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_codec_dev_handle_t  record_dev     = NULL;
static esp_codec_dev_handle_t  play_dev       = NULL;  // ES8311 DAC; NULL until bsp_audio_out_init()
static led_strip_handle_t      s_led          = NULL;  // WS2812; NULL until bsp_led_init()
static SemaphoreHandle_t       s_play_lock    = NULL;  // serialises speaker writes

static esp_err_t i2c_master_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port   = I2C_PORT,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// Create the shared I2S_NUM_1 master channel pair. We only read from rx_handle,
// but the vendor demo creates and enables both tx and rx on the one channel so
// the master clock tree matches what the ES7210 was validated against; we keep
// that exactly and simply never write to tx.
static esp_err_t bsp_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s new channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 16 kHz, 32-bit slots, stereo (the ES7210 packs its 4 TDM channels into
    // this stream — see esp_get_feed_data).
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_I2S_MCLK,
            .bclk = GPIO_I2S_BCLK,
            .ws   = GPIO_I2S_WS,
            .dout = GPIO_I2S_DOUT,
            .din  = GPIO_I2S_DIN,
        },
    };
    ret |= i2s_channel_init_std_mode(tx_handle, &std_cfg);
    ret |= i2s_channel_init_std_mode(rx_handle, &std_cfg);
    ret |= i2s_channel_enable(tx_handle);
    ret |= i2s_channel_enable(rx_handle);
    return ret;
}

// Shared by boot-time bring-up and the runtime bsp_mic_set_gain() setter.
static void set_all_mic_gain(float db)
{
    for (int ch = 0; ch < 4; ++ch) {
        esp_codec_dev_set_in_channel_gain(record_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(ch), db);
    }
}

static esp_err_t bsp_codec_adc_init(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = I2S_NUM_1,
        .rx_handle  = rx_handle,
        .tx_handle  = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if      = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    record_dev = esp_codec_dev_new(&dev_cfg);
    if (!record_dev) {
        ESP_LOGE(TAG, "es7210 codec dev create failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = 16000,
        .channel         = 2,
        .bits_per_sample = 32,
    };
    esp_err_t ret = esp_codec_dev_open(record_dev, &fs);
    set_all_mic_gain(RECORD_VOLUME);
    return ret;
}

esp_err_t bsp_mic_set_gain(float db)
{
    if (!record_dev) return ESP_ERR_INVALID_STATE;
    set_all_mic_gain(db);
    return ESP_OK;
}

esp_err_t bsp_board_init(void)
{
    esp_err_t ret = i2c_master_init();
    if (ret != ESP_OK) return ret;

    ret = bsp_i2s_init();
    if (ret != ESP_OK) return ret;

    ret = bsp_codec_adc_init();
    if (ret != ESP_OK) return ret;

    // Power/PA enables live on the expander; bring it up on the shared bus.
    tca9555_driver_init(i2c_bus_handle);

    ESP_LOGI(TAG, "board init done (mic-only)");
    return ESP_OK;
}

// Bring up the ES8311 speaker DAC for tone playback. Intentionally SEPARATE from
// bsp_board_init() and non-fatal: call it after bsp_board_init() but before the
// mic feed task starts (so the codec opens while the shared I2S is idle, exactly
// as the vendor demo orders it). On any failure it logs, leaves play_dev NULL,
// and returns an error — the mic path is never affected. Safe to skip entirely.
//
// Opened at 32-bit/stereo/16 kHz to match the shared I2S slot geometry the mic
// already uses (esp_codec_dev_open reconfigures the tx slots to bits_per_sample,
// so 32-bit means no change from bsp_i2s_init and no disturbance to the mic's
// clock). Playback writes 32-bit stereo slots with each 16-bit sample in the
// high half (see bsp_audio_play_mono16). The power amp is gated separately via
// the TCA9555 (SPK_PA_EXIO), not a codec pin.
esp_err_t bsp_audio_out_init(void)
{
    if (play_dev) return ESP_OK;
    if (!tx_handle || !i2c_bus_handle) {
        ESP_LOGE(TAG, "audio out: board not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    // Probe the ES8311 before touching the codec driver: if the chip does not
    // ACK we skip cleanly rather than risk the driver faulting on a dead bus.
    // ES8311_CODEC_DEFAULT_ADDR is the 8-bit form; i2c_master_probe wants 7-bit.
    esp_err_t probe = i2c_master_probe(i2c_bus_handle, ES8311_CODEC_DEFAULT_ADDR >> 1, 50);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "audio out: ES8311 not found on I2C (0x%02x): %s; tones disabled",
                 ES8311_CODEC_DEFAULT_ADDR >> 1, esp_err_to_name(probe));
        return ESP_ERR_NOT_FOUND;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_1,
        .rx_handle = NULL,
        .tx_handle = tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,       // amp is on the TCA9555 expander, not a GPIO
        .use_mclk   = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "audio out: es8311_codec_new failed; tones disabled");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if  = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    play_dev = esp_codec_dev_new(&dev_cfg);
    if (!play_dev) {
        ESP_LOGE(TAG, "audio out: esp_codec_dev_new failed; tones disabled");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = 16000,
        .channel         = 2,
        .bits_per_sample = 32,   // match the shared I2S slots; data in the high half
    };
    esp_err_t ret = esp_codec_dev_open(play_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio out: codec open failed: %s; tones disabled", esp_err_to_name(ret));
        esp_codec_dev_delete(play_dev);
        play_dev = NULL;
        return ret;
    }
    // Set volume after open (some codecs latch it from the open sample info).
    esp_codec_dev_set_out_vol(play_dev, PLAYER_VOLUME);

    if (!s_play_lock) s_play_lock = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "audio out ready (ES8311, 32-bit, vol %d)", PLAYER_VOLUME);
    return ESP_OK;
}

esp_err_t bsp_audio_play_mono16(const int16_t *pcm, size_t nsamples)
{
    if (!play_dev) return ESP_ERR_INVALID_STATE;
    if (!pcm || nsamples == 0) return ESP_OK;

    // Serialise playback: overlapping writes (e.g. connect tone vs wake blip)
    // to the shared duplex I2S garble each other.
    if (s_play_lock) xSemaphoreTake(s_play_lock, portMAX_DELAY);

    // Enable the power amp and let it settle before the first sample.
    Set_EXIO(SPK_PA_EXIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Expand mono 16-bit -> stereo 32-bit slots in small chunks: each sample
    // goes in the high half of its 32-bit slot, duplicated L/R. The ES8311 is
    // configured for 16-bit words and reads those high bits (vendor-demo path).
    esp_err_t ret = ESP_OK;
    int32_t frame[256 * 2];
    const size_t kChunk = 256;
    for (size_t off = 0; off < nsamples; off += kChunk) {
        size_t n = (nsamples - off < kChunk) ? (nsamples - off) : kChunk;
        for (size_t i = 0; i < n; ++i) {
            int32_t s = (int32_t)pcm[off + i] << 16;
            frame[2 * i]     = s;  // left
            frame[2 * i + 1] = s;  // right
        }
        ret = esp_codec_dev_write(play_dev, frame, (int)(n * 2 * sizeof(int32_t)));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio out: codec write failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    // esp_codec_dev_write only queues into the I2S DMA; the samples still need to
    // clock out. Wait for that to drain before cutting the amp, or short clips
    // (still entirely in the DMA) never reach the speaker. The pending audio is
    // bounded by both the clip length and the DMA depth (~90 ms at this config),
    // so drain by the smaller of the two plus a small margin — larger clips have
    // mostly played by the time write() returns and only the DMA tail remains.
    uint32_t play_ms = (uint32_t)(nsamples / 16);  // 16 samples/ms at 16 kHz
    uint32_t drain_ms = (play_ms < 90 ? play_ms : 90) + 20;
    vTaskDelay(pdMS_TO_TICKS(drain_ms));

    Set_EXIO(SPK_PA_EXIO, 0);
    if (s_play_lock) xSemaphoreGive(s_play_lock);
    return ret;
}

// Bring up the on-board WS2812 strip (RMT backend). Best-effort and independent
// of everything else; on failure s_led stays NULL and bsp_led_set() no-ops.
esp_err_t bsp_led_init(void)
{
    if (s_led) return ESP_OK;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num        = LED_GPIO,
        .max_leds              = LED_COUNT,
        .led_model             = LED_MODEL_WS2812,
        // RGB, matching the vendor demo: with the WS2812-standard GRB declared
        // here, the wake indicator came out red instead of green on this strip.
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags                 = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags         = { .with_dma = 0 },
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led init failed: %s; LED disabled", esp_err_to_name(ret));
        s_led = NULL;
        return ret;
    }
    led_strip_clear(s_led);
    ESP_LOGI(TAG, "led ready (WS2812 x%d on GPIO%d)", LED_COUNT, LED_GPIO);
    return ESP_OK;
}

void bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    if (r == 0 && g == 0 && b == 0) {
        led_strip_clear(s_led);
        return;
    }
    for (int i = 0; i < LED_COUNT; ++i) {
        led_strip_set_pixel(s_led, i, r, g, b);
    }
    led_strip_refresh(s_led);
}

esp_err_t esp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = esp_codec_dev_read(record_dev, (void *)buffer, buffer_len);
    if (!is_get_raw_channel) {
        int audio_chunksize = buffer_len / (int)(sizeof(int16_t) * ADC_I2S_CHANNEL);
        for (int i = 0; i < audio_chunksize; i++) {
            int16_t ref = buffer[4 * i + 0];
            buffer[3 * i + 0] = buffer[4 * i + 1];
            buffer[3 * i + 1] = buffer[4 * i + 3];
            buffer[3 * i + 2] = ref;
        }
    }
    return ret;
}

int esp_get_feed_channel(void)
{
    return ADC_I2S_CHANNEL;
}

const char *esp_get_input_format(void)
{
    return "RMNM";
}
