/*
    ESP32-S3 AMOLED E-Reader Rendering Pipeline (High-Level Design)

    This project implements a low-power, high-performance e-reader UI targeting
    a 1.91" AMOLED display driven by the SH8601 controller on an ESP32-S3 platform.

    The goal is to render text-centric content (EPUB chapters, documents, and
    long-form reading material) with a visually minimal aesthetic:
        - Pure black background (pixel-off AMOLED state)
        - Text typography adapted per active theme
        - Theme system (dark/light) with sepia light palette
        - Settings menu (long-press left side): theme toggle, font size, chapter picker
        - Scrollable continuous reading surface
        - Touch-driven, low-latency redraw during interaction and scrolling

    The system is designed around constrained embedded hardware resources:

    Hardware characteristics:
        - ESP32-S3 dual-core MCU @ 160 MHz (default; configurable up to 240 MHz)
        - 8 MB external PSRAM (primary framebuffer and tile cache storage)
        - Internal SRAM/IRAM for time-critical execution paths
        - 16 MB external SPI flash (firmware, assets, and persistent storage)
        - QSPI-connected SH8601 AMOLED display controller (240x536 resolution)
        - Capacitive touch input (FT3168 via I2C)
        - Portrait orientation with vertical scrolling as the primary interaction model

    Current rendering architecture:
        - Tile-based rendering of large documents
        - Cached rasterized text regions for reuse during scroll
*/
/*
    Implementation notes and key learnings:

        LVGL 11-bit canvas limit:
            lv_img_header_t stores canvas width/height in 11-bit fields (max 2047px).
            Any canvas taller than 2047px is silently truncated; all text draws beyond
            that pixel row produce no output and no error. Fixed by keeping each
            runtime tile bitmap at 1024px tall, safely under the 2047px limit.

        Display context ordering:
            lv_txt_get_size requires a registered LVGL display to resolve font metrics.
            Calling it before lv_disp_drv_register returns zero dimensions, collapsing
            all computed paragraph heights and causing a black screen.
            Fixed: init_document_layout must be called after lv_disp_drv_register.

        Event-driven rendering:
            A fixed-rate render loop wastes CPU at idle. Switching to a delta-gated
            redraw (only blit when scroll_y changed) plus adaptive vTaskDelay
            (1 ms active / 50 ms idle) keeps idle CPU near zero while preserving
            responsive scrolling.

        Touch I2C fail-safe:
            If the FT3168 I2C read fails, is_touching must be cleared to false.
            Leaving it stale keeps the render task in 1 ms active pacing indefinitely,
            blocking the return to the 50 ms idle sleep and burning CPU.

    Phase 4/5 - rotating tile cache (current):
        Fixed-height document descriptors define the full virtual y-range, but only
        three PSRAM tile bitmaps are kept resident at runtime: previous, current,
        and next. When scroll crosses into an uncached descriptor, the least-recently
        used bitmap is evicted and that descriptor is re-baked into the freed buffer.
        Viewport blitting still uses the same fast path: direct blit when fully inside
        one tile, scratch-buffer composition only when crossing a tile boundary.
*/

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "reader_config.h"
#include "document_layout.h"
#include "stats.h"
#include "touch_input.h"
#include "display_hal.h"
#include "tile_cache.h"
#include "render_runtime.h"
#include "reader_theme.h"
#include "reader_font.h"
#include "reader_menu.h"
#if defined(__has_include)
#if __has_include("content_generated.h")
#include "content_generated.h"
#else
#include "content.h"
#endif
#else
#include "content.h"
#endif
#include <stdlib.h>

static const char *TAG = "app";

#if defined(CONTENT_GENERATED_HAS_CHAPTERS)
#define BOOK_CHAPTER_COUNT ((uint16_t)generated_chapter_count)
#else
static const char *const generated_chapter_titles[] = {chapter_title};
static const char *const generated_chapter_contents[] = {chapter_content};
#define BOOK_CHAPTER_COUNT ((uint16_t)1)
#endif

static const char *book_chapter_title_at(uint16_t index)
{
    if (index >= BOOK_CHAPTER_COUNT)
        return generated_chapter_titles[0];
    return generated_chapter_titles[index];
}

static const char *book_chapter_content_at(uint16_t index)
{
    if (index >= BOOK_CHAPTER_COUNT)
        return generated_chapter_contents[0];
    return generated_chapter_contents[index];
}

static uint16_t clamp_chapter_index(uint16_t index)
{
    if (BOOK_CHAPTER_COUNT == 0)
        return 0;
    if (index >= BOOK_CHAPTER_COUNT)
        return (uint16_t)(BOOK_CHAPTER_COUNT - 1);
    return index;
}

// --- Shared State ---
static int32_t scroll_y = 0;
static uint16_t touch_x = 0;
static uint16_t touch_y = 0;
static bool is_touching = false;
static volatile bool spi_bus_busy = false;
static bool is_rendering_baked = false;
static bool menu_open = false;
static bool chapter_list_open = false;
static int32_t chapter_list_scroll_offset = 0;
static uint16_t chapter_list_pending_index = 0;
static bool needs_layout_rebuild = false;
static uint16_t active_chapter_index = 0;
static SemaphoreHandle_t state_mutex = NULL;

// tile_cache_buffers: the three real PSRAM-resident tile bitmaps used at runtime.
// scratch_buffer: single-screen composition buffer for two-tile boundary crossings.

static lv_color_t *tile_cache_buffers[RUNTIME_TILE_BUFFERS] = {0};
static lv_color_t *scratch_buffer = NULL;
static esp_lcd_panel_handle_t panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static i2c_master_dev_handle_t touch_dev = NULL;
static TaskHandle_t render_task_handle = NULL;
static document_layout_t doc_layout = {0};
static uint32_t tile_generation_counter = 0;
static tile_cache_context_t tile_cache_ctx = {0};
static render_runtime_context_t render_ctx = {0};
static touch_runtime_context_t touch_ctx = {0};

static uint32_t frame_count = 0;
static uint32_t last_frame_count = 0;
static stats_context_t stats_ctx = {0};

typedef struct
{
    uint16_t chapter;
    int32_t scroll_y;
    uint8_t theme;
    uint8_t font;
} persisted_state_t;

static bool nvs_dirty = false;
static TickType_t nvs_last_change_tick = 0;
static persisted_state_t last_saved_state = {0};
static bool last_saved_state_valid = false;
static persisted_state_t last_observed_state = {0};
static bool last_observed_state_valid = false;

#define NVS_SAVE_DEBOUNCE_MS 3000
#define NVS_SAVE_POLL_MS 500

static inline void state_lock(void)
{
    if (state_mutex)
        xSemaphoreTake(state_mutex, portMAX_DELAY);
}

static inline void state_unlock(void)
{
    if (state_mutex)
        xSemaphoreGive(state_mutex);
}

static void snapshot_persisted_state(persisted_state_t *out)
{
    if (!out)
        return;

    state_lock();
    out->chapter = active_chapter_index;
    out->scroll_y = scroll_y;
    out->theme = (uint8_t)reader_theme_get_mode();
    out->font = (uint8_t)reader_font_get_profile();
    state_unlock();
}

static bool persisted_state_equals(const persisted_state_t *a, const persisted_state_t *b)
{
    if (!a || !b)
        return false;

    return (a->chapter == b->chapter) &&
           (a->scroll_y == b->scroll_y) &&
           (a->theme == b->theme) &&
           (a->font == b->font);
}

static void build_menu_state(reader_menu_state_t *state)
{
    if (!state)
        return;

    state_lock();
    state->chapter_count = BOOK_CHAPTER_COUNT;
    // When chapter list is open, show the pending selection; otherwise show actual chapter
    if (chapter_list_open)
        state->current_chapter = clamp_chapter_index((uint16_t)chapter_list_pending_index);
    else
        state->current_chapter = clamp_chapter_index((uint16_t)active_chapter_index);
    state_unlock();
    state->chapter_titles = generated_chapter_titles;
}

static void get_menu_state(void *user_ctx, reader_menu_state_t *state_out)
{
    (void)user_ctx;
    build_menu_state(state_out);
}

static void do_layout_rebuild(void *user_ctx)
{
    (void)user_ctx;
    state_lock();
    uint16_t chapter_index = clamp_chapter_index((uint16_t)active_chapter_index);
    active_chapter_index = chapter_index;
    scroll_y = 0;
    state_unlock();

    const char *title = book_chapter_title_at(chapter_index);
    const char *content = book_chapter_content_at(chapter_index);

    reset_document_layout(&doc_layout);
    init_document_layout(&doc_layout, title, content);
    tile_cache_ctx.title = title;
    tile_cache_ctx.content = content;
    tile_cache_invalidate_all(&tile_cache_ctx);
    ESP_LOGI(TAG, "Chapter %u loaded: %s | %lu paragraphs, %ld px total",
             (unsigned)(chapter_index + 1),
             title,
             (unsigned long)doc_layout.paragraph_count,
             (long)doc_layout.total_height);
}

static void handle_menu_tap(uint16_t x, uint16_t y, void *user_ctx)
{
    (void)x;
    (void)y;
    (void)user_ctx;
    reader_menu_state_t menu_state = {0};
    build_menu_state(&menu_state);
    reader_menu_action_t action = reader_menu_hit_test(&menu_state, x, y);
    bool close_menu = true;
    bool state_changed = false;

    state_lock();
    switch (action.type)
    {
    case READER_MENU_ACTION_SET_THEME_DARK:
        if (reader_theme_get_mode() != READER_THEME_DARK)
        {
            reader_theme_toggle();
            tile_cache_invalidate_all(&tile_cache_ctx);
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_SET_THEME_LIGHT:
        if (reader_theme_get_mode() != READER_THEME_LIGHT)
        {
            reader_theme_toggle();
            tile_cache_invalidate_all(&tile_cache_ctx);
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_SET_FONT_SMALL:
        if (reader_font_get_profile() != READER_FONT_PROFILE_SMALL)
        {
            reader_font_set_profile(READER_FONT_PROFILE_SMALL);
            needs_layout_rebuild = true;
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_SET_FONT_MEDIUM:
        if (reader_font_get_profile() != READER_FONT_PROFILE_MEDIUM)
        {
            reader_font_set_profile(READER_FONT_PROFILE_MEDIUM);
            needs_layout_rebuild = true;
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_SET_FONT_LARGE:
        if (reader_font_get_profile() != READER_FONT_PROFILE_LARGE)
        {
            reader_font_set_profile(READER_FONT_PROFILE_LARGE);
            needs_layout_rebuild = true;
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_CHAPTER_OPEN:
        if (action.chapter_index < BOOK_CHAPTER_COUNT && action.chapter_index != active_chapter_index)
        {
            active_chapter_index = action.chapter_index;
            needs_layout_rebuild = true;
            state_changed = true;
        }
        break;
    case READER_MENU_ACTION_OPEN_CHAPTER_LIST:
        // Open full-screen scrollable chapter list
        chapter_list_pending_index = active_chapter_index;
        chapter_list_open = true;
        chapter_list_scroll_offset = 0;
        touch_ctx.chapter_list_scroll_max = reader_menu_chapter_list_max_scroll(BOOK_CHAPTER_COUNT);
        // close_menu stays true (default) so menu_open will be set to false
        break;
    case READER_MENU_ACTION_NONE:
    default:
        break;
    }

    menu_open = close_menu ? false : true;
    if (state_changed)
    {
        nvs_dirty = true;
        nvs_last_change_tick = xTaskGetTickCount();
    }
    state_unlock();

    if (render_task_handle)
        xTaskNotifyGive(render_task_handle);
}

static void handle_chapter_list_tap(uint16_t x, uint16_t y, void *user_ctx)
{
    (void)x;
    (void)y;
    (void)user_ctx;
    reader_menu_state_t menu_state = {0};
    build_menu_state(&menu_state);
    state_lock();
    int32_t scroll_offset = chapter_list_scroll_offset;
    state_unlock();
    reader_menu_action_t action = reader_menu_chapter_list_hit_test(&menu_state, scroll_offset, x, y);
    bool state_changed = false;

    state_lock();
    switch (action.type)
    {
    case READER_MENU_ACTION_CHAPTER_LIST_SELECT:
        // Update pending selection (but don't load chapter yet)
        chapter_list_pending_index = action.chapter_index;
        break;
    case READER_MENU_ACTION_CHAPTER_LIST_CONFIRM:
        // Apply the pending selection and close the list
        if (chapter_list_pending_index < BOOK_CHAPTER_COUNT &&
            chapter_list_pending_index != active_chapter_index)
        {
            active_chapter_index = chapter_list_pending_index;
            needs_layout_rebuild = true;
            state_changed = true;
        }
        chapter_list_open = false;
        chapter_list_scroll_offset = 0;
        break;
    case READER_MENU_ACTION_CHAPTER_LIST_CANCEL:
        // Close list without applying selection
        chapter_list_open = false;
        chapter_list_scroll_offset = 0;
        break;
    case READER_MENU_ACTION_NONE:
    default:
        break;
    }

    if (state_changed)
    {
        nvs_dirty = true;
        nvs_last_change_tick = xTaskGetTickCount();
    }
    state_unlock();

    if (render_task_handle)
        xTaskNotifyGive(render_task_handle);
}

static void handle_left_control_event(left_control_event_t event,
                                      uint16_t x,
                                      uint16_t y,
                                      void *user_ctx)
{
    (void)x;
    (void)y;
    (void)user_ctx;
    switch (event)
    {
    case LEFT_CONTROL_EVENT_HOLD:
        state_lock();
        menu_open = true;
        state_unlock();
        if (render_task_handle)
            xTaskNotifyGive(render_task_handle);
        break;
    default:
        break;
    }
}

static uint32_t count_loaded_tiles(void)
{
    return tile_cache_count_loaded(&tile_cache_ctx);
}

static void bake_content(void)
{
    size_t tile_buffer_size = (size_t)LCD_H_RES * TILE_HEIGHT * sizeof(lv_color_t);
    for (uint32_t slot = 0; slot < RUNTIME_TILE_BUFFERS; slot++)
    {
        tile_cache_buffers[slot] = heap_caps_aligned_alloc(64,
                                                           tile_buffer_size,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!tile_cache_buffers[slot])
        {
            ESP_LOGE(TAG, "Failed to allocate tile buffer %lu (%u bytes)",
                     (unsigned long)slot,
                     (unsigned)tile_buffer_size);
            abort();
        }
    }

    // Allocate the scratch buffer for two-tile boundary composition (one screen frame).
    scratch_buffer = heap_caps_aligned_alloc(64,
                                             (size_t)LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!scratch_buffer)
    {
        ESP_LOGE(TAG, "bake_content: failed to allocate scratch buffer");
        abort();
    }

    // Warm the opening viewport and its immediate neighbors.
    tile_cache_ensure_resident(&tile_cache_ctx, 0);
    tile_cache_ensure_resident(&tile_cache_ctx, 1);
    tile_cache_ensure_resident(&tile_cache_ctx, 2);

    ESP_LOGI(TAG, "Tiles: %lu descriptors over %ld px | %u resident buffers of %u px",
             (unsigned long)doc_layout.tile_count,
             (long)doc_layout.total_height,
             (unsigned)RUNTIME_TILE_BUFFERS,
             (unsigned)TILE_HEIGHT);

    state_lock();
    is_rendering_baked = true;
    state_unlock();
}

// ---------------------------------------------------------------------------
// NVS Persistence (chapter and scroll position)
// ---------------------------------------------------------------------------
static void nvs_init_and_load(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Open NVS namespace
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    // Check version and clear if firmware updated
    uint32_t saved_version = 0;
    nvs_get_u32(nvs_handle, NVS_KEY_VERSION, &saved_version);

    if (saved_version != FIRMWARE_VERSION)
    {
        ESP_LOGW(TAG, "Firmware version changed (%lu -> %d), clearing saved state",
                 (unsigned long)saved_version, FIRMWARE_VERSION);
        nvs_erase_all(nvs_handle);
        nvs_set_u32(nvs_handle, NVS_KEY_VERSION, FIRMWARE_VERSION);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        return;
    }

    // Load saved state
    uint16_t saved_chapter = 0;
    int32_t saved_scroll_y = 0;
    uint8_t saved_theme = 0;
    uint8_t saved_font = 0;

    err = nvs_get_u16(nvs_handle, NVS_KEY_CHAPTER, &saved_chapter);
    if (err == ESP_OK && saved_chapter < BOOK_CHAPTER_COUNT)
    {
        active_chapter_index = saved_chapter;
        ESP_LOGI(TAG, "Restored chapter: %u", (unsigned)active_chapter_index);
    }

    err = nvs_get_i32(nvs_handle, NVS_KEY_SCROLL_Y, &saved_scroll_y);
    if (err == ESP_OK && saved_scroll_y >= 0)
    {
        scroll_y = saved_scroll_y;
        ESP_LOGI(TAG, "Restored scroll position: %ld", (long)scroll_y);
    }

    err = nvs_get_u8(nvs_handle, NVS_KEY_THEME, &saved_theme);
    if (err == ESP_OK && saved_theme < READER_THEME_LIGHT + 1)
    {
        reader_theme_set_mode((reader_theme_mode_t)saved_theme);
        ESP_LOGI(TAG, "Restored theme: %u", (unsigned)saved_theme);
    }

    err = nvs_get_u8(nvs_handle, NVS_KEY_FONT, &saved_font);
    if (err == ESP_OK && saved_font < READER_FONT_PROFILE_LARGE + 1)
    {
        reader_font_set_profile((reader_font_profile_t)saved_font);
        ESP_LOGI(TAG, "Restored font profile: %u", (unsigned)saved_font);
    }

    nvs_close(nvs_handle);
}

static void nvs_save_state(const persisted_state_t *state)
{
    if (!state)
        return;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open for save failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u16(nvs_handle, NVS_KEY_CHAPTER, state->chapter);
    nvs_set_i32(nvs_handle, NVS_KEY_SCROLL_Y, state->scroll_y);
    nvs_set_u8(nvs_handle, NVS_KEY_THEME, state->theme);
    nvs_set_u8(nvs_handle, NVS_KEY_FONT, state->font);
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs_handle);
}

static void nvs_save_task(void *arg)
{
    (void)arg;

    persisted_state_t current = {0};
    snapshot_persisted_state(&current);
    state_lock();
    last_saved_state = current;
    last_saved_state_valid = true;
    last_observed_state = current;
    last_observed_state_valid = true;
    nvs_dirty = false;
    nvs_last_change_tick = xTaskGetTickCount();
    state_unlock();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(NVS_SAVE_POLL_MS));

        snapshot_persisted_state(&current);

        bool should_save = false;
        state_lock();
        TickType_t now = xTaskGetTickCount();
        if (!last_observed_state_valid || !persisted_state_equals(&current, &last_observed_state))
        {
            nvs_last_change_tick = now;
            last_observed_state = current;
            last_observed_state_valid = true;
        }

        nvs_dirty = (!last_saved_state_valid) || !persisted_state_equals(&current, &last_saved_state);

        bool ui_busy = menu_open || chapter_list_open || is_touching;
        TickType_t elapsed = now - nvs_last_change_tick;
        should_save = nvs_dirty && !ui_busy && (elapsed >= pdMS_TO_TICKS(NVS_SAVE_DEBOUNCE_MS));
        state_unlock();

        if (should_save)
        {
            nvs_save_state(&current);
            state_lock();
            last_saved_state = current;
            last_saved_state_valid = true;
            nvs_dirty = false;
            state_unlock();
        }
    }
}

static void configure_low_leak_sleep_gpios(void)
{
    uint64_t panel_and_touch_pins =
        (1ULL << PIN_NUM_SCLK) |
        (1ULL << PIN_NUM_D0) |
        (1ULL << PIN_NUM_D1) |
        (1ULL << PIN_NUM_D2) |
        (1ULL << PIN_NUM_D3) |
        (1ULL << PIN_NUM_CS) |
        (1ULL << PIN_NUM_RST) |
        (1ULL << I2C_MASTER_SCL_IO) |
        (1ULL << I2C_MASTER_SDA_IO);

    gpio_config_t io_cfg = {
        .pin_bit_mask = panel_and_touch_pins,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Low-leak GPIO config failed: %s", esp_err_to_name(err));
    }

    // Keep configured GPIO states through deep sleep.
    gpio_deep_sleep_hold_en();
}

static void configure_deep_sleep_power_domains(void)
{
    // Power down RTC peripherals; ESP32-S3 does not support separate RTC mem PD domains.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
}

static void enter_aggressive_deep_sleep(void)
{
    persisted_state_t state = {0};
    snapshot_persisted_state(&state);
    nvs_save_state(&state);

    if (panel)
    {
        esp_err_t off_err = display_panel_sleep(panel);
        if (off_err != ESP_OK)
        {
            ESP_LOGW(TAG, "Panel sleep failed before deep sleep: %s", esp_err_to_name(off_err));
        }
    }

    configure_low_leak_sleep_gpios();
    configure_deep_sleep_power_domains();

    vTaskDelay(pdMS_TO_TICKS(40));
    esp_deep_sleep_start();
}

static void idle_sleep_task(void *arg)
{
    (void)arg;

    const TickType_t deep_sleep_threshold = pdMS_TO_TICKS(30000);
    TickType_t last_touch_tick = xTaskGetTickCount();

    while (1)
    {
        state_lock();
        bool touching = is_touching;
        state_unlock();

        if (touching)
        {
            last_touch_tick = xTaskGetTickCount();
        }
        else
        {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_touch_tick) >= deep_sleep_threshold)
            {
                ESP_LOGI(TAG, "No touch for 30s -> panel sleep + deep sleep (wake via reset button)");
                enter_aggressive_deep_sleep();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_deep_sleep_disable_rom_logging();

    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex)
    {
        ESP_LOGE(TAG, "Failed to create shared state mutex");
        abort();
    }

    display_init(&panel, &io_handle, render_flush_ready_cb, (void *)&spi_bus_busy);
    touch_init(&touch_dev);
    lv_init();

    // Load saved reading position from NVS (chapter and scroll_y)
    nvs_init_and_load();

    static lv_disp_draw_buf_t dbuf;
    static lv_disp_drv_t disp_drv;
    lv_color_t *b1 = heap_caps_aligned_alloc(64, LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!b1)
    {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffer");
        abort();
    }
    lv_disp_draw_buf_init(&dbuf, b1, NULL, LCD_H_RES * 40);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.draw_buf = &dbuf;
    lv_disp_drv_register(&disp_drv);

    // init_document_layout must run after lv_disp_drv_register so that
    // lv_txt_get_size has a valid display context for font metrics.
    init_document_layout(&doc_layout,
                         book_chapter_title_at((uint16_t)active_chapter_index),
                         book_chapter_content_at((uint16_t)active_chapter_index));
    size_t layout_meta_bytes = document_layout_metadata_bytes(&doc_layout);
    size_t layout_text_bytes = document_layout_text_bytes(&doc_layout);
    ESP_LOGI(TAG, "Document layout: %lu paragraphs, virtual height %ld px",
             (unsigned long)doc_layout.paragraph_count,
             (long)doc_layout.total_height);
    ESP_LOGI(TAG, "Layout memory: metadata %u bytes | text %u bytes | total %u bytes",
             (unsigned)layout_meta_bytes,
             (unsigned)layout_text_bytes,
             (unsigned)(layout_meta_bytes + layout_text_bytes));

    tile_cache_init_context(&tile_cache_ctx,
                            &doc_layout,
                            tile_cache_buffers,
                            RUNTIME_TILE_BUFFERS,
                            &tile_generation_counter,
                            book_chapter_title_at((uint16_t)active_chapter_index),
                            book_chapter_content_at((uint16_t)active_chapter_index));

    render_runtime_init_context(&render_ctx,
                                state_mutex,
                                &scroll_y,
                                &touch_x,
                                &touch_y,
                                &is_touching,
                                &spi_bus_busy,
                                &is_rendering_baked,
                                &menu_open,
                                &chapter_list_open,
                                &chapter_list_scroll_offset,
                                &needs_layout_rebuild,
                                do_layout_rebuild,
                                NULL,
                                get_menu_state,
                                NULL,
                                &render_task_handle,
                                &frame_count,
                                &scratch_buffer,
                                panel,
                                &doc_layout,
                                &tile_cache_ctx);

    touch_runtime_init_context(&touch_ctx,
                               touch_dev,
                               state_mutex,
                               &is_touching,
                               &touch_x,
                               &touch_y,
                               &render_task_handle,
                               handle_left_control_event,
                               NULL);

    stats_ctx.state_mutex = state_mutex;
    stats_ctx.frame_count = &frame_count;
    stats_ctx.last_frame_count = &last_frame_count;
    stats_ctx.doc_layout = &doc_layout;
    stats_ctx.count_loaded_tiles_fn = count_loaded_tiles;

    bake_content();

    touch_runtime_set_menu(&touch_ctx,
                           &menu_open,
                           handle_menu_tap,
                           NULL);

    touch_runtime_set_chapter_list(&touch_ctx,
                                   &chapter_list_open,
                                   &chapter_list_scroll_offset,
                                   handle_chapter_list_tap,
                                   NULL);

    if (xTaskCreatePinnedToCore(touch_poll_task, "touch", 4096, &touch_ctx, 2, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create touch task");
        abort();
    }
    if (xTaskCreatePinnedToCore(stats_task, "stats", 4096, &stats_ctx, 1, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create stats task");
        abort();
    }
    if (xTaskCreatePinnedToCore(render_task, "render", 8192, &render_ctx, 10, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create render task");
        abort();
    }
    if (xTaskCreatePinnedToCore(nvs_save_task, "nvs_save", 2048, NULL, 1, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create NVS save task");
        abort();
    }
    if (xTaskCreatePinnedToCore(idle_sleep_task, "idle_sleep", 2048, NULL, 1, NULL, 0) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create idle sleep task");
        abort();
    }
}