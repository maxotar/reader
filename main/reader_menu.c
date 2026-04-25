#include "reader_menu.h"

#include <string.h>
#include "lvgl.h"
#include "reader_config.h"
#include "reader_font.h"
#include "reader_theme.h"

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
#define MENU_BG_HEX 0x181614
#define MENU_RULE_HEX 0x302C28
#define MENU_BTN_INACTIVE_HEX 0x2E2A27
#define MENU_BTN_ACTIVE_HEX 0xC8A96E
#define MENU_TEXT_HEX 0xD4CCC2
#define MENU_TEXT_ACTIVE_HEX 0x181614
#define MENU_LABEL_HEX 0x9A8E82

// ---------------------------------------------------------------------------
// Layout constants  (display: 240 x 536)
// ---------------------------------------------------------------------------
#define HEADER_Y 30
#define RULE1_Y 88

#define THEME_SECTION_Y 100
#define THEME_BTN_Y 130
#define THEME_BTN_H 90
#define DARK_BTN_X 10
#define DARK_BTN_W 104
#define LIGHT_BTN_X 126
#define LIGHT_BTN_W 104

#define RULE2_Y 236

#define FONT_SECTION_Y 252
#define FONT_BTN_Y 282
#define FONT_BTN_H 90
#define FONT_S_X 8
#define FONT_S_W 68
#define FONT_M_X 86
#define FONT_M_W 68
#define FONT_L_X 164
#define FONT_L_W 68

#define HINT_Y 438

// Hit zones (must match button bounds above)
#define HIT_THEME_Y1 THEME_BTN_Y
#define HIT_THEME_Y2 (THEME_BTN_Y + THEME_BTN_H)
#define HIT_FONT_Y1 FONT_BTN_Y
#define HIT_FONT_Y2 (FONT_BTN_Y + FONT_BTN_H)

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
static void draw_rule(lv_obj_t *canvas, int32_t y)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(MENU_RULE_HEX);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    lv_canvas_draw_rect(canvas, 0, y, LCD_H_RES, 1, &dsc);
}

static void draw_label(lv_obj_t *canvas,
                       int32_t x, int32_t y, int32_t w,
                       const char *text,
                       const lv_font_t *font,
                       uint32_t color_hex,
                       lv_text_align_t align)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_hex(color_hex);
    dsc.font = font;
    dsc.align = align;
    lv_canvas_draw_text(canvas, x, y, w, &dsc, text);
}

static void draw_button(lv_obj_t *canvas,
                        int32_t x, int32_t y, int32_t w, int32_t h,
                        bool active,
                        const char *label)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = lv_color_hex(active ? MENU_BTN_ACTIVE_HEX : MENU_BTN_INACTIVE_HEX);
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = 8;
    lv_canvas_draw_rect(canvas, x, y, w, h, &rect_dsc);

    const lv_font_t *font = reader_font_body();
    int32_t text_y = y + ((int32_t)h - (int32_t)font->line_height) / 2;
    if (text_y < y)
        text_y = y;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(active ? MENU_TEXT_ACTIVE_HEX : MENU_TEXT_HEX);
    label_dsc.font = font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(canvas, x, text_y, w, &label_dsc, label);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void reader_menu_render(lv_color_t *buffer)
{
    // Fill background before creating canvas so pixels outside draw calls are clean.
    lv_color_t bg = lv_color_hex(MENU_BG_HEX);
    size_t total = (size_t)LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < total; i++)
        buffer[i] = bg;

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    if (!canvas)
        return;
    lv_canvas_set_buffer(canvas, buffer, LCD_H_RES, LCD_V_RES, LV_IMG_CF_TRUE_COLOR);

    const lv_font_t *title_font = reader_font_title();
    const lv_font_t *body_font = reader_font_body();

    // Header
    draw_label(canvas, 0, HEADER_Y, LCD_H_RES,
               "Settings", title_font, MENU_TEXT_HEX, LV_TEXT_ALIGN_CENTER);
    draw_rule(canvas, RULE1_Y);

    // Theme section
    draw_label(canvas, CONTENT_X_PADDING, THEME_SECTION_Y,
               LCD_H_RES - CONTENT_X_PADDING * 2,
               "Theme", body_font, MENU_LABEL_HEX, LV_TEXT_ALIGN_LEFT);

    reader_theme_mode_t theme = reader_theme_get_mode();
    draw_button(canvas, DARK_BTN_X, THEME_BTN_Y, DARK_BTN_W, THEME_BTN_H,
                theme == READER_THEME_DARK, "Dark");
    draw_button(canvas, LIGHT_BTN_X, THEME_BTN_Y, LIGHT_BTN_W, THEME_BTN_H,
                theme == READER_THEME_LIGHT, "Light");
    draw_rule(canvas, RULE2_Y);

    // Font size section
    draw_label(canvas, CONTENT_X_PADDING, FONT_SECTION_Y,
               LCD_H_RES - CONTENT_X_PADDING * 2,
               "Font Size", body_font, MENU_LABEL_HEX, LV_TEXT_ALIGN_LEFT);

    reader_font_profile_t profile = reader_font_get_profile();
    draw_button(canvas, FONT_S_X, FONT_BTN_Y, FONT_S_W, FONT_BTN_H,
                profile == READER_FONT_PROFILE_SMALL, "S");
    draw_button(canvas, FONT_M_X, FONT_BTN_Y, FONT_M_W, FONT_BTN_H,
                profile == READER_FONT_PROFILE_MEDIUM, "M");
    draw_button(canvas, FONT_L_X, FONT_BTN_Y, FONT_L_W, FONT_BTN_H,
                profile == READER_FONT_PROFILE_LARGE, "L");

    // Dismiss hint
    draw_label(canvas, 0, HINT_Y, LCD_H_RES,
               "tap outside to dismiss", body_font, MENU_RULE_HEX, LV_TEXT_ALIGN_CENTER);

    lv_obj_del(canvas);
}

reader_menu_action_t reader_menu_hit_test(uint16_t x, uint16_t y)
{
    if (y >= HIT_THEME_Y1 && y < HIT_THEME_Y2)
    {
        if (x >= DARK_BTN_X && x < (uint16_t)(DARK_BTN_X + DARK_BTN_W))
            return READER_MENU_ACTION_SET_THEME_DARK;
        if (x >= LIGHT_BTN_X && x < (uint16_t)(LIGHT_BTN_X + LIGHT_BTN_W))
            return READER_MENU_ACTION_SET_THEME_LIGHT;
    }
    if (y >= HIT_FONT_Y1 && y < HIT_FONT_Y2)
    {
        if (x >= FONT_S_X && x < (uint16_t)(FONT_S_X + FONT_S_W))
            return READER_MENU_ACTION_SET_FONT_SMALL;
        if (x >= FONT_M_X && x < (uint16_t)(FONT_M_X + FONT_M_W))
            return READER_MENU_ACTION_SET_FONT_MEDIUM;
        if (x >= FONT_L_X && x < (uint16_t)(FONT_L_X + FONT_L_W))
            return READER_MENU_ACTION_SET_FONT_LARGE;
    }
    return READER_MENU_ACTION_NONE;
}
