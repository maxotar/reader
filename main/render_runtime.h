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

typedef struct
{
    volatile int32_t *scroll_y;
    volatile uint16_t *touch_y;
    volatile bool *is_touching;
    volatile bool *spi_bus_busy;
    volatile bool *is_rendering_baked;
    TaskHandle_t *render_task_handle;
    uint32_t *frame_count;
    lv_color_t **scratch_buffer;
    esp_lcd_panel_handle_t panel;
    document_layout_t *layout;
    tile_cache_context_t *tile_cache;
} render_runtime_context_t;

void render_runtime_init_context(render_runtime_context_t *ctx,
                                 volatile int32_t *scroll_y,
                                 volatile uint16_t *touch_y,
                                 volatile bool *is_touching,
                                 volatile bool *spi_bus_busy,
                                 volatile bool *is_rendering_baked,
                                 TaskHandle_t *render_task_handle,
                                 uint32_t *frame_count,
                                 lv_color_t **scratch_buffer,
                                 esp_lcd_panel_handle_t panel,
                                 document_layout_t *layout,
                                 tile_cache_context_t *tile_cache);

IRAM_ATTR bool render_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *ctx);

void render_task(void *arg);

#endif
