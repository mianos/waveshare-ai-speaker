#pragma once

// TCA9555 16-bit I2C I/O expander on the Waveshare audio board. Ported from the
// vendor demo. Drives board power / PA / peripheral enables. Init takes the
// already-created I2C master bus so the board owns a single shared bus.

#include "driver/i2c_master.h"
#include "esp_io_expander_tca95xx_16bit.h"

#ifdef __cplusplus
extern "C" {
#endif

extern esp_io_expander_handle_t io_expander;

void tca9555_driver_init(i2c_master_bus_handle_t i2c_bus);
void Set_EXIO(uint32_t pin, uint8_t state);
bool Read_EXIO(uint32_t pin);

#ifdef __cplusplus
}
#endif
