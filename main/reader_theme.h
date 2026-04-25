#ifndef READER_THEME_H
#define READER_THEME_H

#include <stddef.h>

#include "lvgl.h"

typedef enum
{
    READER_THEME_DARK = 0,
    READER_THEME_LIGHT,
} reader_theme_mode_t;

reader_theme_mode_t reader_theme_get_mode(void);
void reader_theme_set_mode(reader_theme_mode_t mode);
void reader_theme_toggle(void);

lv_color_t reader_theme_background_color(void);
lv_color_t reader_theme_title_color(void);
lv_color_t reader_theme_body_color(void);

void reader_theme_fill_buffer(lv_color_t *buffer, size_t pixel_count);

#endif
