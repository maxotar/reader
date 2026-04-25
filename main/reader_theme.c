#include "reader_theme.h"

typedef struct
{
    uint32_t background_hex;
    uint32_t title_hex;
    uint32_t body_hex;
} reader_theme_palette_t;

static reader_theme_mode_t current_theme_mode = READER_THEME_DARK;

static const reader_theme_palette_t theme_palettes[] = {
    [READER_THEME_DARK] = {
        .background_hex = 0x000000,
        .title_hex = 0xFF2200,
        .body_hex = 0xCC1100,
    },
    [READER_THEME_LIGHT] = {
        .background_hex = 0xCFBA95,
        .title_hex = 0x1F1B17,
        .body_hex = 0x2A2622,
    },
};

static const reader_theme_palette_t *active_palette(void)
{
    return &theme_palettes[current_theme_mode];
}

reader_theme_mode_t reader_theme_get_mode(void)
{
    return current_theme_mode;
}

void reader_theme_set_mode(reader_theme_mode_t mode)
{
    if (mode < READER_THEME_DARK || mode > READER_THEME_LIGHT)
        return;
    current_theme_mode = mode;
}

void reader_theme_toggle(void)
{
    current_theme_mode = (current_theme_mode == READER_THEME_DARK) ? READER_THEME_LIGHT : READER_THEME_DARK;
}

lv_color_t reader_theme_background_color(void)
{
    return lv_color_hex(active_palette()->background_hex);
}

lv_color_t reader_theme_title_color(void)
{
    return lv_color_hex(active_palette()->title_hex);
}

lv_color_t reader_theme_body_color(void)
{
    return lv_color_hex(active_palette()->body_hex);
}

void reader_theme_fill_buffer(lv_color_t *buffer, size_t pixel_count)
{
    if (!buffer || pixel_count == 0)
        return;

    lv_color_t fill = reader_theme_background_color();
    for (size_t i = 0; i < pixel_count; i++)
    {
        buffer[i] = fill;
    }
}
