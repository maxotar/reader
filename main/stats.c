#include "stats.h"

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

void stats_task(void *arg)
{
    stats_context_t *ctx = (stats_context_t *)arg;
    if (!ctx || !ctx->frame_count || !ctx->last_frame_count || !ctx->doc_layout || !ctx->count_loaded_tiles_fn)
    {
        abort();
    }

    static char stats_buf[1024];
    int64_t last_time = esp_timer_get_time();
    *ctx->last_frame_count = *ctx->frame_count;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int64_t now = esp_timer_get_time();
        int64_t delta_time_us = now - last_time;
        uint32_t delta_frames = *ctx->frame_count - *ctx->last_frame_count;

        float real_fps = (delta_frames * 1000000.0f) / delta_time_us;

        last_time = now;
        *ctx->last_frame_count = *ctx->frame_count;

        float bus_load = (real_fps * 6.4f) / 10.0f;

        printf("\n--- SYSTEM AUDIT ---\n");
        printf("Performance: %.1f FPS | Bus Load: %.1f%%\n", real_fps, bus_load);
        printf("Content: %lu bytes | Paragraphs: %lu | Height: %ld px | Tiles: %lu/%lu loaded\n",
               (unsigned long)ctx->doc_layout->content_length,
               (unsigned long)ctx->doc_layout->paragraph_count,
               (long)ctx->doc_layout->total_height,
               (unsigned long)ctx->count_loaded_tiles_fn(),
               (unsigned long)ctx->doc_layout->tile_count);

        vTaskGetRunTimeStats(stats_buf);
        printf("Task Load:\n%s", stats_buf);
        printf("----------------------------------------\n");
    }
}
