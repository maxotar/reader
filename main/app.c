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
#define CANVAS_HEIGHT 4000

#define SCROLL_ACTIVE_FRAME_MS 8
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

static lv_color_t *canvas_buffer = NULL;
static esp_lcd_panel_handle_t panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static i2c_master_dev_handle_t touch_dev = NULL;

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
    while (1)
    {
        if (i2c_master_transmit_receive(touch_dev, &reg, 1, data, 6, pdMS_TO_TICKS(TOUCH_POLL_MS)) == ESP_OK)
        {
            is_touching = (data[0] & 0x0F) > 0;
            if (is_touching)
            {
                touch_y = ((data[3] & 0x0F) << 8) | data[4];
            }
        }
        else
        {
            is_touching = false;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

IRAM_ATTR static void render_task(void *arg)
{
    (void)arg;
    uint16_t last_touch_y = 0;
    int32_t last_blit_scroll_y = -1;
    int32_t max_scroll_y = CANVAS_HEIGHT - LCD_V_RES;
    if (max_scroll_y < 0)
        max_scroll_y = 0;

    while (1)
    {
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

        int32_t delta_scroll = scroll_y - last_blit_scroll_y;
        if (delta_scroll < 0)
            delta_scroll = -delta_scroll;

        bool should_redraw = (last_blit_scroll_y < 0) || (delta_scroll >= SCROLL_REDRAW_THRESHOLD_PX);
        if (is_rendering_baked && canvas_buffer && !spi_bus_busy && should_redraw)
        {
            spi_bus_busy = true;
            lv_color_t *ptr = &canvas_buffer[scroll_y * LCD_H_RES];
            esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, ptr);
            last_blit_scroll_y = scroll_y;
            frame_count++;
        }

        if (is_touching)
        {
            vTaskDelay(pdMS_TO_TICKS(SCROLL_ACTIVE_FRAME_MS));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(SCROLL_IDLE_SLEEP_MS));
        }
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

        vTaskGetRunTimeStats(stats_buf);
        printf("Task Load:\n%s", stats_buf);
        printf("----------------------------------------\n");
    }
}

static void bake_content(void)
{
    size_t sz = LCD_H_RES * CANVAS_HEIGHT * sizeof(lv_color_t);
    canvas_buffer = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!canvas_buffer)
    {
        printf("Failed to allocate canvas buffer (%u bytes)\n", (unsigned)sz);
        return;
    }
    memset(canvas_buffer, 0, sz);

    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas, canvas_buffer, LCD_H_RES, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(0xFF2200);
    label_dsc.font = &lv_font_montserrat_18;
    lv_canvas_draw_text(canvas, 14, 20, LCD_H_RES - 28, &label_dsc, chapter_title);

    label_dsc.color = lv_color_hex(0xCC1100);
    label_dsc.font = &lv_font_montserrat_16;
    label_dsc.line_space = 4;
    lv_canvas_draw_text(canvas, 14, 80, LCD_H_RES - 28, &label_dsc, chapter_content);

    lv_obj_del(canvas);
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

    bake_content();

    xTaskCreatePinnedToCore(touch_poll_task, "touch", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 10, NULL, 1);
}