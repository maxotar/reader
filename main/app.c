/*
    ESP32-S3 AMOLED E-Reader Rendering Pipeline (High-Level Design)

    This project implements a low-power, high-performance e-reader UI targeting
    a 1.91" AMOLED display driven by the SH8601 controller on an ESP32-S3 platform.

    The goal is to render text-centric content (EPUB chapters, documents, and
    long-form reading material) with a visually minimal aesthetic:
        - Pure black background (pixel-off AMOLED state)
        - High-contrast red typography for reduced eye strain and clarity
        - Scrollable continuous reading surface
        - Target refresh rate: stable ~100 FPS during interaction/scrolling

    The system is designed around constrained embedded hardware resources:

    Hardware characteristics:
        - ESP32-S3 dual-core MCU @ up to 240 MHz
        - 8 MB external PSRAM (primary framebuffer and tile cache storage)
        - Internal SRAM/IRAM for time-critical execution paths
        - 16 MB external SPI flash (firmware, assets, and persistent storage)
        - QSPI-connected SH8601 AMOLED display controller (240x536 resolution)
        - Capacitive touch input (FT3168 via I2C)
        - Portrait orientation with vertical scrolling as the primary interaction model

    The system is intended to evolve toward:
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
            (8 ms active / 50 ms idle) keeps idle CPU near zero while delivering
            target FPS during scroll.

        Touch I2C fail-safe:
            If the FT3168 I2C read fails, is_touching must be cleared to false.
            Leaving it stale keeps the render task in 8 ms active pacing indefinitely,
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
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "lvgl.h"
#include "content.h"
#include <stdio.h>
#include <string.h>

#define LCD_HOST SPI2_HOST
#define PIN_NUM_SCLK 47
#define PIN_NUM_D0 18
#define PIN_NUM_D1 7
#define PIN_NUM_D2 48
#define PIN_NUM_D3 5
#define PIN_NUM_CS 6
#define PIN_NUM_RST 17

#define LCD_H_RES 240
#define LCD_V_RES 536

// lv_img_header_t encodes width/height in 11-bit fields: max 2047px. Silent truncation above this.
// TILE_HEIGHT stays below that limit so every runtime tile remains safe to rasterize with LVGL.
#define MAX_TILE_DESCRIPTORS 32 // Max tile descriptors to cover full document
#define TILE_HEIGHT 1024        // Must exceed LCD_V_RES so most frames stay on the direct-blit fast path
#define RUNTIME_TILE_BUFFERS 3  // Previous, current, next
#define MAX_PARAGRAPHS 128
#define MAX_LAYOUT_LINES 2048

#define CONTENT_X_PADDING 14
#define CONTENT_WIDTH (LCD_H_RES - (CONTENT_X_PADDING * 2))
#define TITLE_Y 20
#define BODY_Y 80
#define BODY_LINE_SPACE 4
#define BODY_PARAGRAPH_GAP 18
#define CONTENT_BOTTOM_PADDING 24
#define SCROLL_ACTIVE_FRAME_MS 1
#define SCROLL_IDLE_SLEEP_MS 50
#define SCROLL_REDRAW_THRESHOLD_PX 1
#define TOUCH_POLL_MS 5

#define I2C_MASTER_SCL_IO 39
#define I2C_MASTER_SDA_IO 40
#define I2C_ADDR_FT3168 0x38

// --- Shared State ---
static volatile int32_t scroll_y = 0;
static volatile uint16_t touch_y = 0;
static volatile bool is_touching = false;
static volatile bool spi_bus_busy = false;
static volatile bool is_rendering_baked = false;

typedef struct
{
    uint32_t start_offset;
    uint32_t end_offset;
    int32_t y_offset;
    uint16_t height;
} paragraph_span_t;

typedef struct
{
    uint32_t start_offset;
    uint32_t end_offset;
    int32_t y_offset;
    uint16_t height;
} wrapped_line_t;

typedef struct
{
    lv_color_t *buffer;
    uint16_t pixel_height;
    uint32_t start_line;
    uint32_t end_line;
    int32_t start_y;
    int32_t end_y;
    uint32_t generation;
    bool valid;
} render_tile_t;

typedef struct
{
    uint32_t content_length;
    uint32_t paragraph_count;
    uint32_t line_count;
    uint16_t title_height;
    int32_t total_height;
    paragraph_span_t paragraphs[MAX_PARAGRAPHS];
    wrapped_line_t lines[MAX_LAYOUT_LINES];
    uint32_t tile_count; // Number of tile descriptors covering total_height
    render_tile_t tiles[MAX_TILE_DESCRIPTORS];
} document_layout_t;

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

static uint32_t frame_count = 0;
static uint32_t last_frame_count = 0;

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static char *copy_paragraph_text(const char *text, paragraph_span_t span)
{
    size_t len = span.end_offset - span.start_offset;
    char *buffer = malloc(len + 1);
    if (!buffer)
        return NULL;

    memcpy(buffer, text + span.start_offset, len);
    buffer[len] = '\0';
    return buffer;
}

static int32_t compute_body_height(const document_layout_t *layout)
{
    if (layout->paragraph_count == 0)
        return 0;

    const paragraph_span_t *last_paragraph = &layout->paragraphs[layout->paragraph_count - 1];
    return (last_paragraph->y_offset + last_paragraph->height) - BODY_Y;
}

static void compute_paragraph_layout(document_layout_t *layout, const char *text)
{
    int32_t paragraph_y = BODY_Y;

    for (uint32_t index = 0; index < layout->paragraph_count; index++)
    {
        paragraph_span_t *paragraph_span = &layout->paragraphs[index];
        char *paragraph = copy_paragraph_text(text, *paragraph_span);
        if (!paragraph)
        {
            paragraph_span->y_offset = paragraph_y;
            paragraph_span->height = 0;
            continue;
        }

        lv_point_t paragraph_size;
        lv_txt_get_size(&paragraph_size,
                        paragraph,
                        &lv_font_montserrat_16,
                        0,
                        BODY_LINE_SPACE,
                        CONTENT_WIDTH,
                        LV_TEXT_FLAG_NONE);
        free(paragraph);

        paragraph_span->y_offset = paragraph_y;
        paragraph_span->height = (uint16_t)paragraph_size.y;
        paragraph_y += paragraph_size.y + BODY_PARAGRAPH_GAP;
    }
}

static int32_t compute_document_height(const document_layout_t *layout)
{
    int32_t body_height = compute_body_height(layout);
    int32_t title_bottom = TITLE_Y + layout->title_height;
    int32_t body_bottom = BODY_Y + body_height + CONTENT_BOTTOM_PADDING;
    int32_t total_height = (body_bottom > title_bottom) ? body_bottom : title_bottom;

    return (total_height > LCD_V_RES) ? total_height : LCD_V_RES;
}

static void build_paragraph_index(document_layout_t *layout, const char *text)
{
    const char *paragraph_start = text;
    const char *cursor = text;

    layout->paragraph_count = 0;
    layout->content_length = (uint32_t)strlen(text);

    while (*cursor && layout->paragraph_count < MAX_PARAGRAPHS)
    {
        bool at_boundary = (cursor[0] == '\n' && cursor[1] == '\n');
        if (at_boundary)
        {
            if (cursor > paragraph_start)
            {
                paragraph_span_t *span = &layout->paragraphs[layout->paragraph_count++];
                span->start_offset = (uint32_t)(paragraph_start - text);
                span->end_offset = (uint32_t)(cursor - text);
            }

            if (at_boundary)
            {
                cursor += 2;
                paragraph_start = cursor;
                continue;
            }
        }

        if (*cursor == '\0')
            break;
        cursor++;
    }

    if (paragraph_start < cursor && layout->paragraph_count < MAX_PARAGRAPHS)
    {
        paragraph_span_t *span = &layout->paragraphs[layout->paragraph_count++];
        span->start_offset = (uint32_t)(paragraph_start - text);
        span->end_offset = (uint32_t)(cursor - text);
    }
}

// Divide the virtual document into fixed-height tile descriptors.
// Buffer pointers remain NULL until a descriptor is baked into one of the runtime tile buffers.
static void assign_tile_ranges(document_layout_t *layout)
{
    layout->tile_count = 0;
    int32_t y = 0;
    while (y < layout->total_height && layout->tile_count < MAX_TILE_DESCRIPTORS)
    {
        render_tile_t *tile = &layout->tiles[layout->tile_count];
        tile->start_y = y;
        tile->end_y = y + TILE_HEIGHT;
        if (tile->end_y > layout->total_height)
            tile->end_y = layout->total_height;
        tile->pixel_height = (uint16_t)(tile->end_y - tile->start_y);
        tile->buffer = NULL;
        tile->valid = false;
        tile->generation = 0;
        layout->tile_count++;
        y += TILE_HEIGHT;
    }
}

static void init_document_layout(document_layout_t *layout)
{
    memset(layout, 0, sizeof(*layout));
    build_paragraph_index(layout, chapter_content);
    lv_point_t title_size;
    lv_txt_get_size(&title_size,
                    chapter_title,
                    &lv_font_montserrat_18,
                    0,
                    0,
                    CONTENT_WIDTH,
                    LV_TEXT_FLAG_NONE);
    layout->title_height = (uint16_t)title_size.y;
    compute_paragraph_layout(layout, chapter_content);
    // assign_tile_ranges depends on total_height so must run after compute_document_height.
    layout->total_height = compute_document_height(layout);
    assign_tile_ranges(layout);
}

// Returns the tile index whose y-range covers virtual_y, or -1 if not found.
static int32_t find_tile_for_y(int32_t virtual_y)
{
    for (uint32_t i = 0; i < doc_layout.tile_count; i++)
    {
        if (virtual_y >= doc_layout.tiles[i].start_y && virtual_y < doc_layout.tiles[i].end_y)
            return (int32_t)i;
    }
    return -1;
}

static uint32_t count_loaded_tiles(void)
{
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < doc_layout.tile_count; i++)
    {
        if (doc_layout.tiles[i].valid && doc_layout.tiles[i].buffer)
            loaded++;
    }
    return loaded;
}

static lv_color_t *acquire_tile_buffer(void)
{
    for (uint32_t slot = 0; slot < RUNTIME_TILE_BUFFERS; slot++)
    {
        lv_color_t *buffer = tile_cache_buffers[slot];
        bool in_use = false;

        for (uint32_t index = 0; index < doc_layout.tile_count; index++)
        {
            if (doc_layout.tiles[index].valid && doc_layout.tiles[index].buffer == buffer)
            {
                in_use = true;
                break;
            }
        }

        if (!in_use)
            return buffer;
    }

    int32_t oldest_index = -1;
    uint32_t oldest_generation = 0;
    for (uint32_t index = 0; index < doc_layout.tile_count; index++)
    {
        render_tile_t *tile = &doc_layout.tiles[index];
        if (!tile->valid || !tile->buffer)
            continue;

        if (oldest_index < 0 || tile->generation < oldest_generation)
        {
            oldest_index = (int32_t)index;
            oldest_generation = tile->generation;
        }
    }

    if (oldest_index < 0)
        return NULL;

    lv_color_t *buffer = doc_layout.tiles[oldest_index].buffer;
    doc_layout.tiles[oldest_index].buffer = NULL;
    doc_layout.tiles[oldest_index].valid = false;
    doc_layout.tiles[oldest_index].generation = 0;
    return buffer;
}

static bool bake_tile_bitmap(render_tile_t *tile)
{
    if (!tile || !tile->buffer || tile->pixel_height == 0)
        return false;

    size_t tile_size = (size_t)LCD_H_RES * (size_t)tile->pixel_height * sizeof(lv_color_t);
    memset(tile->buffer, 0, tile_size);

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    if (!canvas)
        return false;
    lv_canvas_set_buffer(canvas, tile->buffer, LCD_H_RES, tile->pixel_height, LV_IMG_CF_TRUE_COLOR);

    lv_draw_label_dsc_t title_dsc;
    lv_draw_label_dsc_init(&title_dsc);
    title_dsc.color = lv_color_hex(0xFF2200);
    title_dsc.font = &lv_font_montserrat_18;

    int32_t title_bottom = TITLE_Y + doc_layout.title_height;
    if (TITLE_Y < tile->end_y && title_bottom > tile->start_y)
    {
        lv_canvas_draw_text(canvas,
                            CONTENT_X_PADDING,
                            TITLE_Y - tile->start_y,
                            CONTENT_WIDTH,
                            &title_dsc,
                            chapter_title);
    }

    lv_draw_label_dsc_t body_dsc;
    lv_draw_label_dsc_init(&body_dsc);
    body_dsc.color = lv_color_hex(0xCC1100);
    body_dsc.font = &lv_font_montserrat_16;
    body_dsc.line_space = BODY_LINE_SPACE;

    for (uint32_t index = 0; index < doc_layout.paragraph_count; index++)
    {
        paragraph_span_t paragraph_span = doc_layout.paragraphs[index];
        char *paragraph = copy_paragraph_text(chapter_content, paragraph_span);
        if (!paragraph)
            break;

        int32_t paragraph_bottom = paragraph_span.y_offset + paragraph_span.height;
        if (paragraph_span.y_offset < tile->end_y && paragraph_bottom > tile->start_y)
        {
            lv_canvas_draw_text(canvas,
                                CONTENT_X_PADDING,
                                paragraph_span.y_offset - tile->start_y,
                                CONTENT_WIDTH,
                                &body_dsc,
                                paragraph);
        }

        free(paragraph);

        if (paragraph_span.y_offset >= tile->end_y)
            break;
    }

    lv_obj_del(canvas);
    tile->valid = true;
    tile->generation = ++tile_generation_counter;
    return true;
}

static bool ensure_tile_resident(int32_t tile_idx)
{
    if (tile_idx < 0 || (uint32_t)tile_idx >= doc_layout.tile_count)
        return false;

    render_tile_t *tile = &doc_layout.tiles[tile_idx];
    if (tile->valid && tile->buffer)
    {
        tile->generation = ++tile_generation_counter;
        return true;
    }

    lv_color_t *buffer = acquire_tile_buffer();
    if (!buffer)
        return false;

    for (uint32_t index = 0; index < doc_layout.tile_count; index++)
    {
        if ((int32_t)index == tile_idx)
            continue;
        if (doc_layout.tiles[index].buffer == buffer)
        {
            doc_layout.tiles[index].buffer = NULL;
            doc_layout.tiles[index].valid = false;
            doc_layout.tiles[index].generation = 0;
        }
    }

    tile->buffer = buffer;
    return bake_tile_bitmap(tile);
}

static void prefetch_neighbor_tiles(int32_t center_tile_idx, int32_t direction)
{
    int32_t primary_idx = center_tile_idx + direction;
    int32_t secondary_idx = center_tile_idx - direction;

    if (primary_idx >= 0 && (uint32_t)primary_idx < doc_layout.tile_count)
        ensure_tile_resident(primary_idx);
    if (secondary_idx >= 0 && (uint32_t)secondary_idx < doc_layout.tile_count)
        ensure_tile_resident(secondary_idx);
}

IRAM_ATTR static bool flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    spi_bus_busy = false;
    return false;
}

static void touch_poll_task(void *arg)
{
    (void)arg;
    uint8_t reg = 0x02, data[6];
    bool last_touching = false;
    uint16_t last_touch_sample = 0;
    while (1)
    {
        bool next_touching = false;
        uint16_t next_touch_y = 0;
        if (i2c_master_transmit_receive(touch_dev, &reg, 1, data, 6, pdMS_TO_TICKS(TOUCH_POLL_MS)) == ESP_OK)
        {
            next_touching = (data[0] & 0x0F) > 0;
            if (next_touching)
                next_touch_y = ((data[3] & 0x0F) << 8) | data[4];
        }

        is_touching = next_touching;
        touch_y = next_touch_y;

        if (render_task_handle &&
            ((next_touching != last_touching) || (next_touching && next_touch_y != last_touch_sample)))
        {
            xTaskNotifyGive(render_task_handle);
        }

        last_touching = next_touching;
        last_touch_sample = next_touch_y;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

IRAM_ATTR static void render_task(void *arg)
{
    (void)arg;
    render_task_handle = xTaskGetCurrentTaskHandle();
    uint16_t last_touch_y = 0;
    int32_t last_blit_scroll_y = -1;

    while (1)
    {
        int32_t max_scroll_y = doc_layout.total_height - LCD_V_RES;
        if (max_scroll_y < 0)
            max_scroll_y = 0;

        if (is_touching)
        {
            if (last_touch_y != 0)
            {
                scroll_y -= (touch_y - last_touch_y);
                if (scroll_y < 0)
                    scroll_y = 0;
                if (scroll_y > max_scroll_y)
                    scroll_y = max_scroll_y;
            }
            last_touch_y = touch_y;
        }
        else
        {
            last_touch_y = 0;
        }

        int32_t scroll_step = (last_blit_scroll_y < 0) ? 0 : (scroll_y - last_blit_scroll_y);
        int32_t delta_scroll = scroll_step;
        if (delta_scroll < 0)
            delta_scroll = -delta_scroll;

        bool should_redraw = (last_blit_scroll_y < 0) || (delta_scroll >= SCROLL_REDRAW_THRESHOLD_PX);
        if (is_rendering_baked && scratch_buffer && !spi_bus_busy && should_redraw)
        {
            int32_t tile_idx = find_tile_for_y(scroll_y);
            if (tile_idx >= 0 && ensure_tile_resident(tile_idx))
            {
                spi_bus_busy = true;
                render_tile_t *tile = &doc_layout.tiles[tile_idx];
                int32_t viewport_end_y = scroll_y + LCD_V_RES;

                if (viewport_end_y <= tile->end_y)
                {
                    // Fast path: viewport is fully within one tile, blit directly.
                    lv_color_t *ptr = &tile->buffer[(scroll_y - tile->start_y) * LCD_H_RES];
                    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, ptr);
                }
                else
                {
                    // Boundary path: viewport crosses two tiles.
                    // Compose both halves into scratch_buffer then blit in one call.
                    int32_t next_idx = find_tile_for_y(tile->end_y);
                    if (next_idx < 0 || !ensure_tile_resident(next_idx))
                    {
                        spi_bus_busy = false;
                        goto render_wait;
                    }

                    int32_t rows_from_current = tile->end_y - scroll_y;
                    memcpy(scratch_buffer,
                           &tile->buffer[(scroll_y - tile->start_y) * LCD_H_RES],
                           (size_t)rows_from_current * LCD_H_RES * sizeof(lv_color_t));

                    int32_t rows_from_next = LCD_V_RES - rows_from_current;
                    if (doc_layout.tiles[next_idx].valid)
                    {
                        memcpy(&scratch_buffer[rows_from_current * LCD_H_RES],
                               doc_layout.tiles[next_idx].buffer,
                               (size_t)rows_from_next * LCD_H_RES * sizeof(lv_color_t));
                    }
                    else
                    {
                        // Next tile not yet available; fill remainder black.
                        memset(&scratch_buffer[rows_from_current * LCD_H_RES], 0,
                               (size_t)rows_from_next * LCD_H_RES * sizeof(lv_color_t));
                    }
                    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, scratch_buffer);
                }
                last_blit_scroll_y = scroll_y;
                frame_count++;

                // Keep the direction of travel warm so crossing into the next tile does not stall.
                prefetch_neighbor_tiles(tile_idx, (scroll_step < 0) ? -1 : 1);
            }
        }

    render_wait:
        uint32_t wait_ms = is_touching ? SCROLL_ACTIVE_FRAME_MS : SCROLL_IDLE_SLEEP_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    static char stats_buf[1024];
    int64_t last_time = esp_timer_get_time();
    last_frame_count = frame_count;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000)); // Sample every 5 seconds for stability

        int64_t now = esp_timer_get_time();
        int64_t delta_time_us = now - last_time;
        uint32_t delta_frames = frame_count - last_frame_count;

        // Calculate actual FPS: (Frames / Microseconds) * 1,000,000
        float real_fps = (delta_frames * 1000000.0f) / delta_time_us;

        last_time = now;
        last_frame_count = frame_count;

        // Bus Load: (FPS * 6.4ms transfer time) / 1000ms
        float bus_load = (real_fps * 6.4f) / 10.0f;

        printf("\n--- SYSTEM AUDIT ---\n");
        printf("Performance: %.1f FPS | Bus Load: %.1f%%\n", real_fps, bus_load);
        printf("Content: %lu bytes | Paragraphs: %lu | Height: %ld px | Tiles: %lu/%lu loaded\n",
               (unsigned long)doc_layout.content_length,
               (unsigned long)doc_layout.paragraph_count,
               (long)doc_layout.total_height,
               (unsigned long)count_loaded_tiles(),
               (unsigned long)doc_layout.tile_count);

        vTaskGetRunTimeStats(stats_buf);
        printf("Task Load:\n%s", stats_buf);
        printf("----------------------------------------\n");
    }
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
            return;
        }
    }

    // Allocate the scratch buffer for two-tile boundary composition (one screen frame).
    scratch_buffer = heap_caps_aligned_alloc(64,
                                             (size_t)LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!scratch_buffer)
    {
        printf("bake_content: failed to allocate scratch buffer\n");
        return;
    }

    // Warm the opening viewport and its immediate neighbors.
    ensure_tile_resident(0);
    ensure_tile_resident(1);
    ensure_tile_resident(2);

    printf("Tiles: %lu descriptors over %ld px | %u resident buffers of %u px\n",
           (unsigned long)doc_layout.tile_count,
           (long)doc_layout.total_height,
           (unsigned)RUNTIME_TILE_BUFFERS,
           (unsigned)TILE_HEIGHT);

    is_rendering_baked = true;
}

static void lcd_init(void)
{
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3, (LCD_H_RES * LCD_V_RES * 2));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_CS, flush_ready_cb, NULL);
    io_config.trans_queue_depth = 3;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));
    sh8601_vendor_config_t vendor = {.init_cmds = lcd_init_cmds, .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]), .flags.use_qspi_interface = 1};
    esp_lcd_panel_dev_config_t cfg = {.reset_gpio_num = PIN_NUM_RST, .bits_per_pixel = 16, .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &cfg, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);
}

static void touch_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true};

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_FT3168,
        .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &touch_dev));
}

void app_main(void)
{
    lcd_init();
    touch_init();
    lv_init();

    static lv_disp_draw_buf_t dbuf;
    static lv_disp_drv_t disp_drv;
    lv_color_t *b1 = heap_caps_aligned_alloc(64, LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!b1)
    {
        printf("Failed to allocate LVGL draw buffer\n");
        return;
    }
    lv_disp_draw_buf_init(&dbuf, b1, NULL, LCD_H_RES * 40);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.draw_buf = &dbuf;
    lv_disp_drv_register(&disp_drv);

    // init_document_layout must run after lv_disp_drv_register so that
    // lv_txt_get_size has a valid display context for font metrics.
    init_document_layout(&doc_layout);
    printf("Document layout: %lu paragraphs, virtual height %ld px\n",
           (unsigned long)doc_layout.paragraph_count,
           (long)doc_layout.total_height);

    bake_content();

    xTaskCreatePinnedToCore(touch_poll_task, "touch", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 10, NULL, 1);
}