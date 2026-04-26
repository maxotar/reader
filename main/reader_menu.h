#ifndef READER_MENU_H
#define READER_MENU_H

#include <stdint.h>
#include "lvgl.h"

#define READER_MENU_CHAPTER_ROWS_PER_PAGE 4

typedef struct
{
    uint16_t chapter_count;
    uint16_t current_chapter;
    const char *const *chapter_titles;
} reader_menu_state_t;

typedef enum
{
    READER_MENU_ACTION_NONE = 0,
    READER_MENU_ACTION_SET_THEME_DARK,
    READER_MENU_ACTION_SET_THEME_LIGHT,
    READER_MENU_ACTION_SET_FONT_SMALL,
    READER_MENU_ACTION_SET_FONT_MEDIUM,
    READER_MENU_ACTION_SET_FONT_LARGE,
    READER_MENU_ACTION_CHAPTER_OPEN,
    READER_MENU_ACTION_OPEN_CHAPTER_LIST,
    READER_MENU_ACTION_CHAPTER_LIST_SELECT,
    READER_MENU_ACTION_CHAPTER_LIST_CONFIRM,
    READER_MENU_ACTION_CHAPTER_LIST_CANCEL,
} reader_menu_action_type_t;

typedef struct
{
    reader_menu_action_type_t type;
    uint16_t chapter_index;
} reader_menu_action_t;

// Render the full-screen menu overlay into an LCD_H_RES x LCD_V_RES pixel buffer.
void reader_menu_render(lv_color_t *buffer, const reader_menu_state_t *state);

// Render a full-screen scrollable chapter list (all chapters, scrollable).
// scroll_offset: scroll position in pixels (0 = top of list)
void reader_menu_render_chapter_list(lv_color_t *buffer, const reader_menu_state_t *state, int32_t scroll_offset);

// Hit-test a tap and return the resulting action (NONE = dismiss without change).
reader_menu_action_t reader_menu_hit_test(const reader_menu_state_t *state, uint16_t x, uint16_t y);

// Hit-test for scrollable chapter list view
reader_menu_action_t reader_menu_chapter_list_hit_test(const reader_menu_state_t *state, int32_t scroll_offset, uint16_t x, uint16_t y);

// Returns the maximum valid scroll offset for a chapter list with the given chapter count.
int32_t reader_menu_chapter_list_max_scroll(uint16_t chapter_count);

// Returns gesture/layout bounds used by touch handling for the chapter list screen.
void reader_menu_chapter_list_gesture_bounds(uint16_t *top_buttons_end_y,
                                             uint16_t *scroll_start_y,
                                             uint16_t *select_button_end_x);

#endif
