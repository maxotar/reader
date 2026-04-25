#include "reader_font.h"

#include "sdkconfig.h"

#if defined(__has_include)
#if __has_include("book_font.h")
#include "book_font.h"
#define READER_HAS_CUSTOM_BOOK_FONT 1
#endif
#endif

#if !defined(READER_HAS_CUSTOM_BOOK_FONT)
#define READER_HAS_CUSTOM_BOOK_FONT 0
#endif

static reader_font_profile_t current_profile = READER_FONT_PROFILE_MEDIUM;

void reader_font_set_profile(reader_font_profile_t profile)
{
    if (profile < READER_FONT_PROFILE_SMALL || profile > READER_FONT_PROFILE_LARGE)
        return;
    current_profile = profile;
}

reader_font_profile_t reader_font_get_profile(void)
{
    return current_profile;
}

const lv_font_t *reader_font_body(void)
{
#if READER_HAS_CUSTOM_BOOK_FONT
    switch (current_profile)
    {
    case READER_FONT_PROFILE_SMALL:
        return &book_font_body_16;
    case READER_FONT_PROFILE_LARGE:
        return &book_font_body_24;
    case READER_FONT_PROFILE_MEDIUM:
    default:
        return &book_font_body_20;
    }
#else
    switch (current_profile)
    {
    case READER_FONT_PROFILE_SMALL:
#if CONFIG_LV_FONT_DEJAVU_16_PERSIAN_HEBREW
        return &lv_font_dejavu_16_persian_hebrew;
#elif CONFIG_LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#elif CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#else
        return LV_FONT_DEFAULT;
#endif
    case READER_FONT_PROFILE_LARGE:
#if CONFIG_LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif CONFIG_LV_FONT_MONTSERRAT_22
        return &lv_font_montserrat_22;
#elif CONFIG_LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#else
        return LV_FONT_DEFAULT;
#endif
    case READER_FONT_PROFILE_MEDIUM:
    default:
#if CONFIG_LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#elif CONFIG_LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#elif CONFIG_LV_FONT_DEJAVU_16_PERSIAN_HEBREW
        return &lv_font_dejavu_16_persian_hebrew;
#else
        return LV_FONT_DEFAULT;
#endif
    }
#endif
}

const lv_font_t *reader_font_title(void)
{
#if READER_HAS_CUSTOM_BOOK_FONT
    switch (current_profile)
    {
    case READER_FONT_PROFILE_SMALL:
        return &book_font_title_18;
    case READER_FONT_PROFILE_LARGE:
        return &book_font_title_26;
    case READER_FONT_PROFILE_MEDIUM:
    default:
        return &book_font_title_22;
    }
#else
    switch (current_profile)
    {
    case READER_FONT_PROFILE_SMALL:
#if CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#elif CONFIG_LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return LV_FONT_DEFAULT;
#endif
    case READER_FONT_PROFILE_LARGE:
#if CONFIG_LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif CONFIG_LV_FONT_MONTSERRAT_22
        return &lv_font_montserrat_22;
#elif CONFIG_LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#else
        return LV_FONT_DEFAULT;
#endif
    case READER_FONT_PROFILE_MEDIUM:
    default:
#if CONFIG_LV_FONT_MONTSERRAT_22
        return &lv_font_montserrat_22;
#elif CONFIG_LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif CONFIG_LV_FONT_MONTSERRAT_18
        return &lv_font_montserrat_18;
#else
        return LV_FONT_DEFAULT;
#endif
    }
#endif
}
