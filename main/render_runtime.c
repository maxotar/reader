#include "render_runtime.h"

#include <string.h>
#include <stdlib.h>

#include "reader_config.h"

void render_runtime_init_context(render_runtime_context_t *ctx,
                                 volatile int32_t *scroll_y,
                                 volatile uint16_t *touch_x,
                                 volatile uint16_t *touch_y,
                                 volatile bool *is_touching,
                                 volatile bool *spi_bus_busy,
                                 volatile bool *is_rendering_baked,
                                 TaskHandle_t *render_task_handle,
                                 uint32_t *frame_count,
                                 lv_color_t **scratch_buffer,
                                 esp_lcd_panel_handle_t panel,
                                 document_layout_t *layout,
                                 tile_cache_context_t *tile_cache)
{
    ctx->scroll_y = scroll_y;
    ctx->touch_x = touch_x;
    ctx->touch_y = touch_y;
    ctx->is_touching = is_touching;
    ctx->spi_bus_busy = spi_bus_busy;
    ctx->is_rendering_baked = is_rendering_baked;
    ctx->render_task_handle = render_task_handle;
    ctx->frame_count = frame_count;
    ctx->scratch_buffer = scratch_buffer;
    ctx->panel = panel;
    ctx->layout = layout;
    ctx->tile_cache = tile_cache;
}

IRAM_ATTR bool render_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *ctx)
{
    (void)io;
    (void)edata;
    volatile bool *spi_bus_busy = (volatile bool *)ctx;
    *spi_bus_busy = false;
    return false;
}

static float touch_position_scroll_velocity(uint16_t touch_y)
{
    int32_t y = (int32_t)touch_y;
    if (y > (LCD_V_RES - 1))
        y = LCD_V_RES - 1;

    int32_t reverse_zone_end = LCD_V_RES / 6;
    int32_t deadzone_end = (LCD_V_RES * 2) / 6;

    if (y < reverse_zone_end)
        return -(float)TOUCH_SCROLL_REVERSE_PX_PER_FRAME;
    if (y < deadzone_end)
        return 0.0f;

    float denom = (float)((LCD_V_RES - 1) - deadzone_end);
    if (denom <= 0.0f)
        return 0.0f;

    float normalized = (float)(y - deadzone_end) / denom;
    if (normalized < 0.0f)
        normalized = 0.0f;
    if (normalized > 1.0f)
        normalized = 1.0f;

    // Quadratic ramp: very fine low-end control, then faster acceleration near bottom.
    float curved = normalized * normalized;
    float max_speed = (float)TOUCH_SCROLL_FORWARD_MAX_PX_PER_FRAME;
    if (max_speed < 1.0f)
        max_speed = 1.0f;

    return curved * max_speed;
}

void render_task(void *arg)
{
    render_runtime_context_t *ctx = (render_runtime_context_t *)arg;
    if (!ctx || !ctx->scroll_y || !ctx->touch_x || !ctx->touch_y || !ctx->is_touching || !ctx->spi_bus_busy ||
        !ctx->is_rendering_baked || !ctx->render_task_handle || !ctx->frame_count ||
        !ctx->scratch_buffer || !ctx->layout || !ctx->tile_cache)
    {
        abort();
    }

    *ctx->render_task_handle = xTaskGetCurrentTaskHandle();
    int32_t last_blit_scroll_y = -1;
    float scroll_accumulator = 0.0f;

    while (1)
    {
        int32_t max_scroll_y = ctx->layout->total_height - LCD_V_RES;
        if (max_scroll_y < 0)
            max_scroll_y = 0;

        bool scroll_touch_active = *ctx->is_touching && (*ctx->touch_x >= (LCD_H_RES / 2));

        if (scroll_touch_active)
        {
            float velocity = touch_position_scroll_velocity(*ctx->touch_y);
            scroll_accumulator += velocity;

            int32_t position_step = (int32_t)scroll_accumulator;
            if (position_step != 0)
            {
                scroll_accumulator -= (float)position_step;
                *ctx->scroll_y += position_step;
                if (*ctx->scroll_y < 0)
                {
                    *ctx->scroll_y = 0;
                    scroll_accumulator = 0.0f;
                }
                if (*ctx->scroll_y > max_scroll_y)
                {
                    *ctx->scroll_y = max_scroll_y;
                    scroll_accumulator = 0.0f;
                }
            }
        }
        else
        {
            scroll_accumulator = 0.0f;
        }

        int32_t scroll_step = (last_blit_scroll_y < 0) ? 0 : (*ctx->scroll_y - last_blit_scroll_y);
        int32_t delta_scroll = scroll_step;
        if (delta_scroll < 0)
            delta_scroll = -delta_scroll;

        bool should_redraw = (last_blit_scroll_y < 0) || (delta_scroll >= SCROLL_REDRAW_THRESHOLD_PX);
        if (*ctx->is_rendering_baked && *ctx->scratch_buffer && !*ctx->spi_bus_busy && should_redraw)
        {
            int32_t tile_idx = find_tile_for_y(ctx->layout, *ctx->scroll_y);
            if (tile_idx >= 0 && tile_cache_ensure_resident(ctx->tile_cache, tile_idx))
            {
                *ctx->spi_bus_busy = true;
                render_tile_t *tile = &ctx->layout->tiles[tile_idx];
                int32_t viewport_end_y = *ctx->scroll_y + LCD_V_RES;

                if (viewport_end_y <= tile->end_y)
                {
                    lv_color_t *ptr = &tile->buffer[(*ctx->scroll_y - tile->start_y) * LCD_H_RES];
                    esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, LCD_H_RES, LCD_V_RES, ptr);
                }
                else
                {
                    int32_t next_idx = find_tile_for_y(ctx->layout, tile->end_y);
                    if (next_idx < 0 || !tile_cache_ensure_resident(ctx->tile_cache, next_idx))
                    {
                        *ctx->spi_bus_busy = false;
                        goto render_wait;
                    }

                    int32_t rows_from_current = tile->end_y - *ctx->scroll_y;
                    memcpy(*ctx->scratch_buffer,
                           &tile->buffer[(*ctx->scroll_y - tile->start_y) * LCD_H_RES],
                           (size_t)rows_from_current * LCD_H_RES * sizeof(lv_color_t));

                    int32_t rows_from_next = LCD_V_RES - rows_from_current;
                    if (ctx->layout->tiles[next_idx].valid)
                    {
                        memcpy(&(*ctx->scratch_buffer)[rows_from_current * LCD_H_RES],
                               ctx->layout->tiles[next_idx].buffer,
                               (size_t)rows_from_next * LCD_H_RES * sizeof(lv_color_t));
                    }
                    else
                    {
                        memset(&(*ctx->scratch_buffer)[rows_from_current * LCD_H_RES],
                               0,
                               (size_t)rows_from_next * LCD_H_RES * sizeof(lv_color_t));
                    }
                    esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, LCD_H_RES, LCD_V_RES, *ctx->scratch_buffer);
                }
                last_blit_scroll_y = *ctx->scroll_y;
                (*ctx->frame_count)++;

                tile_cache_prefetch_neighbors(ctx->tile_cache, tile_idx, (scroll_step < 0) ? -1 : 1);
            }
        }

    render_wait:
        uint32_t wait_ms = *ctx->is_touching ? SCROLL_ACTIVE_FRAME_MS : SCROLL_IDLE_SLEEP_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}
