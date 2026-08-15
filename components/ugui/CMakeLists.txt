idf_component_register(
    SRCS "ugui.c" "quartz_gui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_timer
)