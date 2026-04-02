idf_component_register(
    SRCS "app.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd freertos esp_timer lvgl
)
