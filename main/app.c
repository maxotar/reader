/*
    ESP32-S3 AMOLED E-Reader Rendering Pipeline (High-Level Design)

    This project implements a low-power, high-performance e-reader UI targeting
    a 1.91" AMOLED display driven by the SH8601 controller on an ESP32-S3 platform.

    The goal is to render text-centric content (EPUB chapters, documents, and
    long-form reading material) with a visually minimal aesthetic:
        - Pure black background (pixel-off AMOLED state)
            - Text typography adapted per active theme
            - Theme system (dark/light) with sepia light palette
            - Settings menu (long-press left side): theme toggle and font size selection
        - Scrollable continuous reading surface
        - Touch-driven, low-latency redraw during interaction and scrolling

    The system is designed around constrained embedded hardware resources:

    Hardware characteristics:
        - ESP32-S3 dual-core MCU @ up to 240 MHz
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
#include "freertos/task.h"
#include "esp_heap_caps.h"
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
#include <stdio.h>
#include <stdlib.h>

// --- Shared State ---
static volatile int32_t scroll_y = 0;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool is_touching = false;
static volatile bool spi_bus_busy = false;
static volatile bool is_rendering_baked = false;
static volatile bool menu_open = false;
static volatile bool needs_layout_rebuild = false;

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

static void do_layout_rebuild(void *user_ctx)
{
    (void)user_ctx;
    scroll_y = 0;
    reset_document_layout(&doc_layout);
    init_document_layout(&doc_layout, chapter_title, chapter_content);
    tile_cache_invalidate_all(&tile_cache_ctx);
    printf("Layout rebuilt: %lu paragraphs, %ld px total\n",
           (unsigned long)doc_layout.paragraph_count,
           (long)doc_layout.total_height);
}

static void handle_menu_tap(uint16_t x, uint16_t y, void *user_ctx)
{
    (void)user_ctx;
    reader_menu_action_t action = reader_menu_hit_test(x, y);
    menu_open = false;

    switch (action)
    {
    case READER_MENU_ACTION_SET_THEME_DARK:
        if (reader_theme_get_mode() != READER_THEME_DARK)
        {
            reader_theme_toggle();
            tile_cache_invalidate_all(&tile_cache_ctx);
        }
        break;
    case READER_MENU_ACTION_SET_THEME_LIGHT:
        if (reader_theme_get_mode() != READER_THEME_LIGHT)
        {
            reader_theme_toggle();
            tile_cache_invalidate_all(&tile_cache_ctx);
        }
        break;
    case READER_MENU_ACTION_SET_FONT_SMALL:
        if (reader_font_get_profile() != READER_FONT_PROFILE_SMALL)
        {
            reader_font_set_profile(READER_FONT_PROFILE_SMALL);
            needs_layout_rebuild = true;
        }
        break;
    case READER_MENU_ACTION_SET_FONT_MEDIUM:
        if (reader_font_get_profile() != READER_FONT_PROFILE_MEDIUM)
        {
            reader_font_set_profile(READER_FONT_PROFILE_MEDIUM);
            needs_layout_rebuild = true;
        }
        break;
    case READER_MENU_ACTION_SET_FONT_LARGE:
        if (reader_font_get_profile() != READER_FONT_PROFILE_LARGE)
        {
            reader_font_set_profile(READER_FONT_PROFILE_LARGE);
            needs_layout_rebuild = true;
        }
        break;
    case READER_MENU_ACTION_NONE:
    default:
        break;
    }

    if (render_task_handle)
        xTaskNotifyGive(render_task_handle);
    printf("Menu closed: action=%d at (%u,%u)\n", (int)action, (unsigned)x, (unsigned)y);
}

static void handle_left_control_event(left_control_event_t event,
                                      uint16_t x,
                                      uint16_t y,
                                      void *user_ctx)
{
    (void)user_ctx;
    switch (event)
    {
    case LEFT_CONTROL_EVENT_HOLD:
        menu_open = true;
        if (render_task_handle)
            xTaskNotifyGive(render_task_handle);
        printf("Menu opened at (%u,%u)\n", (unsigned)x, (unsigned)y);
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
            printf("Failed to allocate tile buffer %lu (%u bytes)\n",
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
        printf("bake_content: failed to allocate scratch buffer\n");
        abort();
    }

    // Warm the opening viewport and its immediate neighbors.
    tile_cache_ensure_resident(&tile_cache_ctx, 0);
    tile_cache_ensure_resident(&tile_cache_ctx, 1);
    tile_cache_ensure_resident(&tile_cache_ctx, 2);

    printf("Tiles: %lu descriptors over %ld px | %u resident buffers of %u px\n",
           (unsigned long)doc_layout.tile_count,
           (long)doc_layout.total_height,
           (unsigned)RUNTIME_TILE_BUFFERS,
           (unsigned)TILE_HEIGHT);

    is_rendering_baked = true;
}

void app_main(void)
{
    display_init(&panel, &io_handle, render_flush_ready_cb, (void *)&spi_bus_busy);
    touch_init(&touch_dev);
    lv_init();

    static lv_disp_draw_buf_t dbuf;
    static lv_disp_drv_t disp_drv;
    lv_color_t *b1 = heap_caps_aligned_alloc(64, LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!b1)
    {
        printf("Failed to allocate LVGL draw buffer\n");
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
    init_document_layout(&doc_layout, chapter_title, chapter_content);
    size_t layout_meta_bytes = document_layout_metadata_bytes(&doc_layout);
    size_t layout_text_bytes = document_layout_text_bytes(&doc_layout);
    printf("Document layout: %lu paragraphs, virtual height %ld px\n",
           (unsigned long)doc_layout.paragraph_count,
           (long)doc_layout.total_height);
    printf("Layout memory: metadata %u bytes | text %u bytes | total %u bytes\n",
           (unsigned)layout_meta_bytes,
           (unsigned)layout_text_bytes,
           (unsigned)(layout_meta_bytes + layout_text_bytes));

    tile_cache_init_context(&tile_cache_ctx,
                            &doc_layout,
                            tile_cache_buffers,
                            RUNTIME_TILE_BUFFERS,
                            &tile_generation_counter,
                            chapter_title,
                            chapter_content);

    render_runtime_init_context(&render_ctx,
                                &scroll_y,
                                &touch_x,
                                &touch_y,
                                &is_touching,
                                &spi_bus_busy,
                                &is_rendering_baked,
                                &menu_open,
                                &needs_layout_rebuild,
                                do_layout_rebuild,
                                NULL,
                                &render_task_handle,
                                &frame_count,
                                &scratch_buffer,
                                panel,
                                &doc_layout,
                                &tile_cache_ctx);

    touch_runtime_init_context(&touch_ctx,
                               touch_dev,
                               &is_touching,
                               &touch_x,
                               &touch_y,
                               &render_task_handle,
                               handle_left_control_event,
                               NULL);

    stats_ctx.frame_count = &frame_count;
    stats_ctx.last_frame_count = &last_frame_count;
    stats_ctx.doc_layout = &doc_layout;
    stats_ctx.count_loaded_tiles_fn = count_loaded_tiles;

    bake_content();

    touch_runtime_set_menu(&touch_ctx,
                           &menu_open,
                           handle_menu_tap,
                           NULL);

    if (xTaskCreatePinnedToCore(touch_poll_task, "touch", 4096, &touch_ctx, 2, NULL, 0) != pdPASS)
    {
        printf("Failed to create touch task\n");
        abort();
    }
    if (xTaskCreatePinnedToCore(stats_task, "stats", 4096, &stats_ctx, 1, NULL, 0) != pdPASS)
    {
        printf("Failed to create stats task\n");
        abort();
    }
    if (xTaskCreatePinnedToCore(render_task, "render", 8192, &render_ctx, 10, NULL, 1) != pdPASS)
    {
        printf("Failed to create render task\n");
        abort();
    }
}