idf_component_register(
    SRCS "app.c" "document_layout.c" "stats.c" "touch_input.c" "display_hal.c" "tile_cache.c" "render_runtime.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd freertos esp_timer lvgl
)
