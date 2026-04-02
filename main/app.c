#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "lvgl.h"

static const char *TAG = "amoled_reader";

/* ── Hardware Definitions ── */
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

/* ── LVGL Globals ── */
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_mux;
esp_lcd_panel_handle_t panel_handle = NULL;

/* Declare fonts manually to assist the compiler */
LV_FONT_DECLARE(lv_font_montserrat_8);
LV_FONT_DECLARE(lv_font_montserrat_10);
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_22);
LV_FONT_DECLARE(lv_font_montserrat_24);

/* ── LCD Initialization Commands ── */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* ── Callbacks ── */

// This fires when the QSPI finishes sending a frame of data
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // Forward the buffer to the hardware driver
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

// Higher precision tick using ESP Timer (Hardware ISR)
static void lv_tick_task(void *arg)
{
    lv_tick_inc(1);
}

/* ── Tasks ── */

static void lvgl_handler_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL main loop");
    while (1)
    {
        if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE)
        {
            uint32_t task_delay_ms = lv_timer_handler();
            xSemaphoreGive(lvgl_mux);

            // Limit delay to prevent task starvation
            if (task_delay_ms > 50)
                task_delay_ms = 50;
            if (task_delay_ms < 5)
                task_delay_ms = 5;

            vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
        }
    }
}

/* ── Main Init Functions ── */

void lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing QSPI Bus...");
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3,
        LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Installing Panel IO...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_CS, NULL, NULL);

    // Register the DMA callback so LVGL knows when the transfer is done
    io_config.on_color_trans_done = notify_lvgl_flush_ready;
    io_config.user_ctx = &disp_drv;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_LOGI(TAG, "Installing SH8601 Panel Driver...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "AMOLED Initialized");
}

void lvgl_setup(void)
{
    lv_init();

    // Allocation in PSRAM (SPIRAM) for the S3
    // We use two full-size buffers for double buffering (No tearing/flicker)
    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);

    lv_color_t *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf1 == NULL || buf2 == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate display buffers in PSRAM!");
        abort();
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LCD_V_RES);

    /* Initialize the display driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1; // Critical for smooth AMOLED updates
    lv_disp_drv_register(&disp_drv);

    /* Create High-Precision Tick Timer */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lv_tick_task,
        .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, 1000); // 1ms

    lvgl_mux = xSemaphoreCreateMutex();

    /* Create Handler Task on Core 1 (keep Core 0 for Wi-Fi/System) */
    xTaskCreatePinnedToCore(lvgl_handler_task, "LVGL", 8192, NULL, 5, NULL, 1);
}

void create_ui(void)
{
    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE)
    {

        // 1. Set the screen to a vertical layout (Flex)
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN); // Stack vertically
        lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(scr, 20, 0); // Add 20px gap between items

        // 2. Main Title
        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "Hello, World!");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);

        // 3. Body Text (The one from my previous tip)
        lv_obj_t *body_text = lv_label_create(scr);
        lv_label_set_long_mode(body_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(body_text, 220);
        lv_label_set_text(body_text, "This text is rendered with subpixel antialiasing. "
                                     "On an AMOLED screen, it should look sharp.");
        lv_obj_set_style_text_font(body_text, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(body_text, lv_color_white(), 0);
        lv_obj_set_style_text_align(body_text, LV_TEXT_ALIGN_CENTER, 0);

        xSemaphoreGive(lvgl_mux);
    }
}

void app_main(void)
{
    lcd_init();
    lvgl_setup();
    create_ui();

    ESP_LOGI(TAG, "Application UI created. Memory Free: %u bytes", (unsigned int)esp_get_free_heap_size());
}