#ifndef READER_MENU_H
#define READER_MENU_H

#include <stdint.h>
#include "lvgl.h"

typedef enum
{
    READER_MENU_ACTION_NONE = 0,
    READER_MENU_ACTION_SET_THEME_DARK,
    READER_MENU_ACTION_SET_THEME_LIGHT,
    READER_MENU_ACTION_SET_FONT_SMALL,
    READER_MENU_ACTION_SET_FONT_MEDIUM,
    READER_MENU_ACTION_SET_FONT_LARGE,
} reader_menu_action_t;

// Render the full-screen menu overlay into an LCD_H_RES x LCD_V_RES pixel buffer.
void reader_menu_render(lv_color_t *buffer);

// Hit-test a tap and return the resulting action (NONE = dismiss without change).
reader_menu_action_t reader_menu_hit_test(uint16_t x, uint16_t y);

#endif
