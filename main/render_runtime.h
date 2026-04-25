#ifndef RENDER_RUNTIME_H
#define RENDER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "document_layout.h"
#include "tile_cache.h"
#include "reader_menu.h"

typedef struct
{
    volatile int32_t *scroll_y;
    volatile uint16_t *touch_x;
    volatile uint16_t *touch_y;
    volatile bool *is_touching;
    volatile bool *spi_bus_busy;
    volatile bool *is_rendering_baked;
    volatile bool *menu_open;
    volatile bool *chapter_list_open;
    volatile int32_t *chapter_list_scroll_offset;
    volatile bool *needs_layout_rebuild;
    void (*layout_rebuild_fn)(void *user_ctx);
    void *layout_rebuild_user_ctx;
    void (*menu_state_fn)(void *user_ctx, reader_menu_state_t *state_out);
    void *menu_state_user_ctx;
    TaskHandle_t *render_task_handle;
    uint32_t *frame_count;
    lv_color_t **scratch_buffer;
    esp_lcd_panel_handle_t panel;
    document_layout_t *layout;
    tile_cache_context_t *tile_cache;
} render_runtime_context_t;

void render_runtime_init_context(render_runtime_context_t *ctx,
                                 volatile int32_t *scroll_y,
                                 volatile uint16_t *touch_x,
                                 volatile uint16_t *touch_y,
                                 volatile bool *is_touching,
                                 volatile bool *spi_bus_busy,
                                 volatile bool *is_rendering_baked,
                                 volatile bool *menu_open,
                                 volatile bool *chapter_list_open,
                                 volatile int32_t *chapter_list_scroll_offset,
                                 volatile bool *needs_layout_rebuild,
                                 void (*layout_rebuild_fn)(void *user_ctx),
                                 void *layout_rebuild_user_ctx,
                                 void (*menu_state_fn)(void *user_ctx, reader_menu_state_t *state_out),
                                 void *menu_state_user_ctx,
                                 TaskHandle_t *render_task_handle,
                                 uint32_t *frame_count,
                                 lv_color_t **scratch_buffer,
                                 esp_lcd_panel_handle_t panel,
                                 document_layout_t *layout,
                                 tile_cache_context_t *tile_cache);

bool render_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                           esp_lcd_panel_io_event_data_t *edata,
                           void *ctx);

void render_task(void *arg);

#endif
