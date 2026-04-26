#include "stats.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "stats";

void stats_task(void *arg)
{
    stats_context_t *ctx = (stats_context_t *)arg;
    if (!ctx || !ctx->state_mutex || !ctx->frame_count || !ctx->last_frame_count || !ctx->doc_layout || !ctx->count_loaded_tiles_fn)
    {
        abort();
    }

    static char stats_buf[1024];
    int64_t last_time = esp_timer_get_time();
    xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
    *ctx->last_frame_count = *ctx->frame_count;
    xSemaphoreGive(ctx->state_mutex);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int64_t now = esp_timer_get_time();
        int64_t delta_time_us = now - last_time;
        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
        uint32_t frame_count_now = *ctx->frame_count;
        uint32_t delta_frames = frame_count_now - *ctx->last_frame_count;
        *ctx->last_frame_count = frame_count_now;
        xSemaphoreGive(ctx->state_mutex);

        float real_fps = (delta_frames * 1000000.0f) / delta_time_us;

        last_time = now;

        float bus_load = (real_fps * 6.4f) / 10.0f;

        ESP_LOGI(TAG, "--- SYSTEM AUDIT ---");
        ESP_LOGI(TAG, "Performance: %.1f FPS | Bus Load: %.1f%%", real_fps, bus_load);
        ESP_LOGI(TAG, "Content: %lu bytes | Paragraphs: %lu | Height: %ld px | Tiles: %lu/%lu loaded",
                 (unsigned long)ctx->doc_layout->content_length,
                 (unsigned long)ctx->doc_layout->paragraph_count,
                 (long)ctx->doc_layout->total_height,
                 (unsigned long)ctx->count_loaded_tiles_fn(),
                 (unsigned long)ctx->doc_layout->tile_count);

        vTaskGetRunTimeStats(stats_buf);
        ESP_LOGI(TAG, "Task Load:\n%s", stats_buf);
        ESP_LOGI(TAG, "----------------------------------------");
    }
}
