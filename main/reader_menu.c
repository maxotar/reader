#include "reader_menu.h"

#include "lvgl.h"
#include "reader_config.h"
#include "reader_font.h"
#include "reader_theme.h"
#include <stdio.h>

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
#define HEADER_Y 12
#define RULE1_Y 52

#define THEME_SECTION_Y 66
#define THEME_BTN_Y 94
#define THEME_BTN_H 56
#define HIT_THEME_Y1 THEME_BTN_Y
#define HIT_THEME_Y2 (THEME_BTN_Y + THEME_BTN_H)
#define DARK_BTN_X 10
#define DARK_BTN_W 104
#define LIGHT_BTN_X 126
#define LIGHT_BTN_W 104

#define FONT_SECTION_Y 164
#define FONT_BTN_Y 192
#define FONT_BTN_H 56
#define HIT_FONT_Y1 FONT_BTN_Y
#define HIT_FONT_Y2 (FONT_BTN_Y + FONT_BTN_H)
#define FONT_S_X 8
#define FONT_S_W 68
#define FONT_M_X 86
#define FONT_M_W 68
#define FONT_L_X 164
#define FONT_L_W 68

#define CHAPTER_SECTION_Y 264
#define CHAPTER_LIST_BTN_Y 292
#define CHAPTER_LIST_BTN_X 8
#define CHAPTER_LIST_BTN_W (LCD_H_RES - 16)
#define CHAPTER_LIST_BTN_H 50
#define HIT_CHAPTER_LIST_BTN_Y1 CHAPTER_LIST_BTN_Y
#define HIT_CHAPTER_LIST_BTN_Y2 (CHAPTER_LIST_BTN_Y + CHAPTER_LIST_BTN_H)

#define HINT_Y 514

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
void reader_menu_render(lv_color_t *buffer, const reader_menu_state_t *state)
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

    // Chapter section - List button takes full width
    draw_label(canvas, CONTENT_X_PADDING, CHAPTER_SECTION_Y,
               LCD_H_RES - CONTENT_X_PADDING * 2,
               "Chapter", body_font, MENU_LABEL_HEX, LV_TEXT_ALIGN_LEFT);

    draw_button(canvas, CHAPTER_LIST_BTN_X, CHAPTER_LIST_BTN_Y, CHAPTER_LIST_BTN_W, CHAPTER_LIST_BTN_H,
                false, "Browse Chapters");
    ;

    // Dismiss hint
    draw_label(canvas, 0, HINT_Y, LCD_H_RES,
               "tap outside to dismiss", body_font, MENU_RULE_HEX, LV_TEXT_ALIGN_CENTER);

    lv_obj_del(canvas);
}

reader_menu_action_t reader_menu_hit_test(const reader_menu_state_t *state, uint16_t x, uint16_t y)
{
    reader_menu_action_t action = {
        .type = READER_MENU_ACTION_NONE,
        .chapter_index = 0,
    };

    if (y >= HIT_THEME_Y1 && y < HIT_THEME_Y2)
    {
        if (x >= DARK_BTN_X && x < (uint16_t)(DARK_BTN_X + DARK_BTN_W))
        {
            action.type = READER_MENU_ACTION_SET_THEME_DARK;
            return action;
        }
        if (x >= LIGHT_BTN_X && x < (uint16_t)(LIGHT_BTN_X + LIGHT_BTN_W))
        {
            action.type = READER_MENU_ACTION_SET_THEME_LIGHT;
            return action;
        }
    }
    if (y >= HIT_FONT_Y1 && y < HIT_FONT_Y2)
    {
        if (x >= FONT_S_X && x < (uint16_t)(FONT_S_X + FONT_S_W))
        {
            action.type = READER_MENU_ACTION_SET_FONT_SMALL;
            return action;
        }
        if (x >= FONT_M_X && x < (uint16_t)(FONT_M_X + FONT_M_W))
        {
            action.type = READER_MENU_ACTION_SET_FONT_MEDIUM;
            return action;
        }
        if (x >= FONT_L_X && x < (uint16_t)(FONT_L_X + FONT_L_W))
        {
            action.type = READER_MENU_ACTION_SET_FONT_LARGE;
            return action;
        }
    }

    if (y >= HIT_CHAPTER_LIST_BTN_Y1 && y < HIT_CHAPTER_LIST_BTN_Y2)
    {
        if (x >= CHAPTER_LIST_BTN_X && x < (uint16_t)(CHAPTER_LIST_BTN_X + CHAPTER_LIST_BTN_W))
        {
            action.type = READER_MENU_ACTION_OPEN_CHAPTER_LIST;
            return action;
        }
    }

    return action;
}

// ---------------------------------------------------------------------------
// Scrollable Chapter List (Full-Screen View)
// ---------------------------------------------------------------------------
// Buttons at top
#define CHAPTER_LIST_TOP_BTN_Y 8
#define CHAPTER_LIST_TOP_BTN_H 58
#define CHAPTER_LIST_CANCEL_X 2
#define CHAPTER_LIST_CANCEL_W 116
#define CHAPTER_LIST_CONFIRM_X 122
#define CHAPTER_LIST_CONFIRM_W 116

// Header below buttons
#define CHAPTER_LIST_HEADER_Y (CHAPTER_LIST_TOP_BTN_Y + CHAPTER_LIST_TOP_BTN_H + 6)
#define CHAPTER_LIST_SCROLL_START_Y (CHAPTER_LIST_HEADER_Y + 26)
#define CHAPTER_LIST_SCROLL_END_Y (LCD_V_RES - 2)

// Chapter row layout
#define CHAPTER_SELECT_BTN_X 8
#define CHAPTER_LIST_TITLE_X 40
#define CHAPTER_LIST_TITLE_W (LCD_H_RES - 16 - CHAPTER_LIST_TITLE_X)

static int32_t chapter_list_row_gap_px(void)
{
    const lv_font_t *body_font = reader_font_body();
    int32_t line_h = body_font ? (int32_t)body_font->line_height : 20;
    // Keep rows visually separated as fonts grow.
    return (line_h >= 30) ? 14 : 10;
}

static int32_t chapter_list_row_height_px(void)
{
    const lv_font_t *body_font = reader_font_body();
    int32_t line_h = body_font ? (int32_t)body_font->line_height : 20;
    int32_t row_h = line_h + 30;
    if (row_h < 56)
        row_h = 56;
    return row_h;
}

static int32_t chapter_list_select_btn_size_px(void)
{
    int32_t row_h = chapter_list_row_height_px();
    int32_t btn = row_h - 20;
    if (btn < 30)
        btn = 30;
    if (btn > 42)
        btn = 42;
    return btn;
}

void reader_menu_render_chapter_list(lv_color_t *buffer, const reader_menu_state_t *state, int32_t scroll_offset)
{
    if (!buffer || !state)
        return;

    // Fill background
    lv_color_t bg = lv_color_hex(MENU_BG_HEX);
    size_t total = (size_t)LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < total; i++)
        buffer[i] = bg;

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    if (!canvas)
        return;
    lv_canvas_set_buffer(canvas, buffer, LCD_H_RES, LCD_V_RES, LV_IMG_CF_TRUE_COLOR);

    const lv_font_t *body_font = reader_font_body();

    // Render chapter rows with scroll offset
    if (!state || state->chapter_count == 0 || !state->chapter_titles)
    {
        draw_label(canvas, 8, CHAPTER_LIST_SCROLL_START_Y, LCD_H_RES - 16,
                   "No chapters", body_font, MENU_TEXT_HEX, LV_TEXT_ALIGN_CENTER);
    }
    else
    {
        // Calculate which chapter rows are visible based on scroll_offset
        int32_t row_h = chapter_list_row_height_px();
        int32_t row_gap = chapter_list_row_gap_px();
        int32_t select_btn = chapter_list_select_btn_size_px();
        int32_t row_pixel_height = row_h + row_gap;
        int32_t first_visible_row = scroll_offset / row_pixel_height;
        if (first_visible_row < 0)
            first_visible_row = 0;

        int32_t screen_y = CHAPTER_LIST_SCROLL_START_Y - (scroll_offset % row_pixel_height);

        for (uint16_t chapter_idx = (uint16_t)first_visible_row; chapter_idx < state->chapter_count; chapter_idx++)
        {
            int32_t row_y = screen_y + (int32_t)(chapter_idx - first_visible_row) * row_pixel_height;

            // Stop rendering if we're beyond the screen
            if (row_y >= CHAPTER_LIST_SCROLL_END_Y)
                break;

            // Skip if row is above the scroll area
            if (row_y + row_h < CHAPTER_LIST_SCROLL_START_Y)
                continue;

            bool is_selected = (chapter_idx == state->current_chapter);

            // Draw row background
            lv_draw_rect_dsc_t rect_dsc;
            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.bg_color = lv_color_hex(is_selected ? MENU_BTN_ACTIVE_HEX : MENU_BTN_INACTIVE_HEX);
            rect_dsc.bg_opa = LV_OPA_COVER;
            rect_dsc.radius = 4;
            lv_canvas_draw_rect(canvas, 8, row_y, LCD_H_RES - 16, row_h, &rect_dsc);

            // Draw selectable button on left
            lv_draw_rect_dsc_t btn_dsc;
            lv_draw_rect_dsc_init(&btn_dsc);
            btn_dsc.bg_color = lv_color_hex(is_selected ? MENU_TEXT_ACTIVE_HEX : MENU_RULE_HEX);
            btn_dsc.bg_opa = LV_OPA_COVER;
            btn_dsc.radius = 3;
            int32_t btn_x = 8 + CHAPTER_SELECT_BTN_X;
            int32_t btn_y = row_y + (row_h - select_btn) / 2;
            lv_canvas_draw_rect(canvas, btn_x, btn_y, select_btn, select_btn, &btn_dsc);

            // Draw checkmark if selected
            if (is_selected)
            {
                lv_draw_label_dsc_t check_dsc;
                lv_draw_label_dsc_init(&check_dsc);
                check_dsc.color = lv_color_hex(MENU_BTN_INACTIVE_HEX);
                check_dsc.font = body_font;
                check_dsc.align = LV_TEXT_ALIGN_CENTER;
                lv_canvas_draw_text(canvas, btn_x, btn_y - 2, select_btn, &check_dsc, "✓");
            }

            // Draw chapter number and title (no wrapping, just clip)
            char label[256];
            lv_snprintf(label, sizeof(label), "%u. %s", (unsigned)(chapter_idx + 1),
                        state->chapter_titles[chapter_idx] ? state->chapter_titles[chapter_idx] : "(untitled)");

            lv_draw_label_dsc_t text_dsc;
            lv_draw_label_dsc_init(&text_dsc);
            text_dsc.color = lv_color_hex(is_selected ? MENU_TEXT_ACTIVE_HEX : MENU_TEXT_HEX);
            text_dsc.font = body_font;
            text_dsc.align = LV_TEXT_ALIGN_LEFT;
            // Use lv_canvas_draw_text with width limit to clip text
            int32_t text_y = row_y + (row_h - (int32_t)body_font->line_height) / 2;
            if (text_y < row_y)
                text_y = row_y;
            lv_canvas_draw_text(canvas, 8 + CHAPTER_LIST_TITLE_X, text_y, CHAPTER_LIST_TITLE_W, &text_dsc, label);
        }
    }

    // Draw top controls last so list rows never obscure them.
    draw_button(canvas, CHAPTER_LIST_CANCEL_X, CHAPTER_LIST_TOP_BTN_Y, CHAPTER_LIST_CANCEL_W, CHAPTER_LIST_TOP_BTN_H,
                false, "Cancel");
    draw_button(canvas, CHAPTER_LIST_CONFIRM_X, CHAPTER_LIST_TOP_BTN_Y, CHAPTER_LIST_CONFIRM_W, CHAPTER_LIST_TOP_BTN_H,
                false, "Confirm");
    draw_rule(canvas, CHAPTER_LIST_TOP_BTN_Y + CHAPTER_LIST_TOP_BTN_H + 6);
    draw_label(canvas, 0, CHAPTER_LIST_HEADER_Y, LCD_H_RES,
               "Tap button to select", body_font, MENU_LABEL_HEX, LV_TEXT_ALIGN_CENTER);

    lv_obj_del(canvas);
}

reader_menu_action_t reader_menu_chapter_list_hit_test(const reader_menu_state_t *state, int32_t scroll_offset, uint16_t x, uint16_t y)
{
    reader_menu_action_t action = {
        .type = READER_MENU_ACTION_NONE,
        .chapter_index = 0,
    };

    if (!state || state->chapter_count == 0 || !state->chapter_titles)
        return action;

    // Check for button hits first (Confirm/Cancel)
    if (y >= (uint16_t)CHAPTER_LIST_TOP_BTN_Y && y < (uint16_t)(CHAPTER_LIST_TOP_BTN_Y + CHAPTER_LIST_TOP_BTN_H))
    {
        // Cancel button
        if (x >= CHAPTER_LIST_CANCEL_X && x < (uint16_t)(CHAPTER_LIST_CANCEL_X + CHAPTER_LIST_CANCEL_W))
        {
            printf("DEBUG: Button hit - CANCEL at (%u, %u)\n", (unsigned)x, (unsigned)y);
            action.type = READER_MENU_ACTION_CHAPTER_LIST_CANCEL;
            return action;
        }
        // Confirm button
        if (x >= CHAPTER_LIST_CONFIRM_X && x < (uint16_t)(CHAPTER_LIST_CONFIRM_X + CHAPTER_LIST_CONFIRM_W))
        {
            printf("DEBUG: Button hit - CONFIRM at (%u, %u)\n", (unsigned)x, (unsigned)y);
            action.type = READER_MENU_ACTION_CHAPTER_LIST_CONFIRM;
            return action;
        }
    }

    // Check for chapter row hits (select chapter)
    if (y < CHAPTER_LIST_SCROLL_START_Y || y >= (uint16_t)CHAPTER_LIST_SCROLL_END_Y)
        return action;

    // Calculate which chapter row was tapped
    int32_t row_h = chapter_list_row_height_px();
    int32_t row_gap = chapter_list_row_gap_px();
    int32_t select_btn = chapter_list_select_btn_size_px();
    int32_t row_pixel_height = row_h + row_gap;
    int32_t first_visible_row = scroll_offset / row_pixel_height;
    if (first_visible_row < 0)
        first_visible_row = 0;

    int32_t screen_y = CHAPTER_LIST_SCROLL_START_Y - (scroll_offset % row_pixel_height);
    int32_t relative_y = y - screen_y;

    if (relative_y < 0)
        return action;

    uint16_t row_idx = (uint16_t)(relative_y / row_pixel_height);
    uint16_t chapter_idx = (uint16_t)(first_visible_row + row_idx);

    if (chapter_idx >= state->chapter_count)
        return action;

    int32_t row_y = screen_y + row_idx * row_pixel_height;

    // Check if tap is within the row bounds
    if (y >= (uint16_t)row_y && y < (uint16_t)(row_y + row_h))
    {
        // Check if tap is on the selection button (left side of row)
        int32_t btn_x = 8 + CHAPTER_SELECT_BTN_X;
        int32_t btn_y = row_y + (row_h - select_btn) / 2;
        if (x >= (uint16_t)btn_x && x < (uint16_t)(btn_x + select_btn) &&
            y >= (uint16_t)btn_y && y < (uint16_t)(btn_y + select_btn))
        {
            action.type = READER_MENU_ACTION_CHAPTER_LIST_SELECT;
            action.chapter_index = chapter_idx;
            return action;
        }
    }

    return action;
}
