idf_component_register(
    SRCS "bsp_board.c" "tca9555_driver.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2s esp_driver_i2c)
# esp_codec_dev (ES7210) and esp_io_expander_tca95xx_16bit are pulled in as
# managed dependencies (idf_component.yml) and auto-added to this component's
# requirements.
