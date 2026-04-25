file(GLOB READER_GENERATED_FONT_SRCS "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")

idf_component_register(
    SRCS
        "reader_font.c"
        "reader_menu.c"
        "app.c"
        "document_layout.c"
        "stats.c"
        "touch_input.c"
        "display_hal.c"
        "tile_cache.c"
        "render_runtime.c"
        "reader_theme.c"
        ${READER_GENERATED_FONT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd freertos esp_timer lvgl nvs_flash
)
