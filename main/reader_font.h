#ifndef READER_FONT_H
#define READER_FONT_H

#include "lvgl.h"

typedef enum
{
    READER_FONT_PROFILE_SMALL = 0,
    READER_FONT_PROFILE_MEDIUM,
    READER_FONT_PROFILE_LARGE,
} reader_font_profile_t;

void reader_font_set_profile(reader_font_profile_t profile);
reader_font_profile_t reader_font_get_profile(void);

const lv_font_t *reader_font_body(void);
const lv_font_t *reader_font_title(void);

#endif