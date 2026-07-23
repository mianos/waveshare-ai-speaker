#include "tca9555_driver.h"

#include "esp_log.h"

static const char *TAG = "tca9555";

esp_io_expander_handle_t io_expander = NULL;

void tca9555_driver_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca95xx_16bit(
        i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &io_expander);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tca9555 create failed: %s", esp_err_to_name(ret));
        return;
    }

    // Match the vendor demo's direction map: pins 0,1,5,6,8 outputs (power/PA
    // enables), pins 2,9,10,11 inputs.
    ret = esp_io_expander_set_dir(io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5 |
        IO_EXPANDER_PIN_NUM_6 | IO_EXPANDER_PIN_NUM_8, IO_EXPANDER_OUTPUT);
    if (ret != ESP_OK) ESP_LOGE(TAG, "set output dir failed: %s", esp_err_to_name(ret));

    ret = esp_io_expander_set_dir(io_expander,
        IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 |
        IO_EXPANDER_PIN_NUM_11, IO_EXPANDER_INPUT);
    if (ret != ESP_OK) ESP_LOGE(TAG, "set input dir failed: %s", esp_err_to_name(ret));
}

void Set_EXIO(uint32_t pin, uint8_t state)
{
    esp_err_t ret = esp_io_expander_set_level(io_expander, pin, state);
    if (ret != ESP_OK) ESP_LOGE(TAG, "set level failed: %s", esp_err_to_name(ret));
}

bool Read_EXIO(uint32_t pin)
{
    uint32_t level_mask = 0;
    esp_err_t ret = esp_io_expander_get_level(io_expander, pin, &level_mask);
    if (ret != ESP_OK) ESP_LOGE(TAG, "get level failed: %s", esp_err_to_name(ret));
    return level_mask & pin;
}
