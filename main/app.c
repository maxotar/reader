#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "lvgl.h"

static const char *TAG = "amoled_reader";

#define LCD_HOST SPI2_HOST
#define PIN_NUM_SCLK 47
#define PIN_NUM_D0 18
#define PIN_NUM_D1 7
#define PIN_NUM_D2 48
#define PIN_NUM_D3 5
#define PIN_NUM_CS 6
#define PIN_NUM_RST 17

#define I2C_MASTER_SCL_IO 39
#define I2C_MASTER_SDA_IO 40
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_ADDR_FT3168 0x38

#define LCD_H_RES 240
#define LCD_V_RES 536

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_mux;
esp_lcd_panel_handle_t panel_handle = NULL;

LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_24);

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

void touch_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
}

uint8_t getTouch(uint16_t *x, uint16_t *y)
{
    uint8_t touch_points_num = 0;
    uint8_t data_buf[4];
    uint8_t reg_status = 0x02;
    uint8_t reg_coords = 0x03;

    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, I2C_ADDR_FT3168,
                                                 &reg_status, 1, &touch_points_num, 1,
                                                 pdMS_TO_TICKS(10));

    if (ret == ESP_OK && (touch_points_num & 0x0F) > 0)
    {
        ret = i2c_master_write_read_device(I2C_MASTER_NUM, I2C_ADDR_FT3168,
                                           &reg_coords, 1, data_buf, 4,
                                           pdMS_TO_TICKS(10));

        if (ret == ESP_OK)
        {
            // 1. Extract raw values from the buffer
            uint16_t raw_y = (((uint16_t)data_buf[0] & 0x0f) << 8) | (uint16_t)data_buf[1];
            uint16_t raw_x = (((uint16_t)data_buf[2] & 0x0f) << 8) | (uint16_t)data_buf[3];

            // 2. SWAP X and Y because the controller is rotated 90 degrees
            // Map the touch 'X' to the display 'X' and touch 'Y' to display 'Y'
            *x = raw_y;
            *y = raw_x;

            return 1;
        }
    }
    return 0;
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t tp_x, tp_y;
    if (getTouch(&tp_x, &tp_y))
    {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PR;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lv_tick_task(void *arg)
{
    lv_tick_inc(1);
}

static void lvgl_handler_task(void *arg)
{
    while (1)
    {
        uint32_t task_delay_ms = 5;
        if (xSemaphoreTake(lvgl_mux, 0) == pdTRUE)
        {
            task_delay_ms = lv_timer_handler();
            xSemaphoreGive(lvgl_mux);
        }
        if (task_delay_ms == 0)
            task_delay_ms = 1;
        if (task_delay_ms > 30)
            task_delay_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

void lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing QSPI Bus...");

    // 1. Bus Config using Macro
    // We use the full screen size for max_transfer_sz because we enabled PSRAM DMA in menuconfig
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3,
        LCD_H_RES * LCD_V_RES * sizeof(uint16_t));

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 2. IO Config using Macro
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        PIN_NUM_CS,
        notify_lvgl_flush_ready,
        &disp_drv);

    // --- PERFORMANCE OVERRIDES ---
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10; // Allow deeper pipeline for smoother frames
    // -----------------------------

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    // 3. Vendor & Panel Setup
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

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Extra stability: short delay before turning display on
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

void lvgl_setup(void)
{
    lv_init();
    lvgl_mux = xSemaphoreCreateMutex();

    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf1 || !buf2)
    {
        ESP_LOGE(TAG, "PSRAM Allocation Failed!");
        abort();
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LCD_V_RES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1;
    // disp_drv.direct_mode = 1;
    lv_disp_drv_register(&disp_drv);

    // Tick & Task
    const esp_timer_create_args_t tick_args = {.callback = &lv_tick_task, .name = "lvgl_tick"};
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    // Touch Setup
    touch_i2c_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    xTaskCreatePinnedToCore(lvgl_handler_task, "LVGL", 8192, NULL, 7, NULL, 1);
}

void create_ui(void)
{
    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE)
    {
        lv_color_t book_red = lv_color_make(255, 0, 0);
        lv_color_t book_black = lv_color_make(0, 0, 0); // Pure AMOLED Black

        // 1. Create a reusable "Opaque" style
        static lv_style_t style_opaque;
        lv_style_init(&style_opaque);
        lv_style_set_bg_opa(&style_opaque, LV_OPA_COVER);
        lv_style_set_bg_color(&style_opaque, book_black);
        lv_style_set_text_color(&style_opaque, book_red);

        // 2. Setup Screen - FORCE BLACK HERE
        lv_obj_t *scr = lv_scr_act();
        lv_obj_remove_style_all(scr); // Strips any default/theme leftovers
        lv_obj_add_style(scr, &style_opaque, 0);
        lv_obj_set_style_bg_color(scr, book_black, 0); // Double-down on Black

        // 3. Setup Scrolling Container
        lv_obj_t *cont = lv_obj_create(scr);
        lv_obj_remove_style_all(cont); // Strips default grey borders/backgrounds
        lv_obj_add_style(cont, &style_opaque, 0);

        lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
        lv_obj_set_pos(cont, 0, 0);

        // Kill all default styling aggressively
        lv_obj_set_style_bg_color(cont, book_black, 0);
        lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_outline_width(cont, 0, 0);
        lv_obj_set_style_shadow_width(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 15, 0);

        // Layout
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);

        // 4. Chapter Title
        lv_obj_t *title = lv_label_create(cont);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, book_red, 0);
        lv_obj_set_style_pad_bottom(title, 20, 0);
        // lv_label_set_text(title, "CHAPTER ONE");

        // 5. Body Text
        lv_obj_t *body = lv_label_create(cont);
        lv_obj_add_style(body, &style_opaque, 0);
        lv_obj_set_width(body, 210); // Adjust this to your screen width - padding
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        // lv_label_set_text(body, "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
        //                         "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
        //                         "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
        //                         "nisi ut aliquip ex ea commodo consequat. \n\n"
        //                         "Duis aute irure dolor in reprehenderit in voluptate velit esse "
        //                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
        //                         "cupidatat non proident, sunt in culpa qui officia deserunt mollit "
        //                         "anim id est laborum.\n\n"
        //                         "Duis aute irure dolor in reprehenderit in voluptate velit esse "
        //                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
        //                         "cupidatat non proident, sunt in culpa qui officia deserunt mollit "
        //                         "anim id est laborum.\n\n"
        //                         "Duis aute irure dolor in reprehenderit in voluptate velit esse "
        //                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
        //                         "cupidatat non proident, sunt in culpa qui officia deserunt mollit "
        //                         "anim id est laborum.\n\n"
        //                         "Duis aute irure dolor in reprehenderit in voluptate velit esse "
        //                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
        //                         "cupidatat non proident, sunt in culpa qui officia deserunt mollit "
        //                         "anim id est laborum.\n\n"
        //                         "Duis aute irure dolor in reprehenderit in voluptate velit esse "
        //                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat "
        //                         "cupidatat non proident, sunt in culpa qui officia deserunt mollit "
        //                         "anim id est laborum.\n\n"
        //                         "By scrolling with your finger, you can now see the power of "
        //                         "this S3 AMOLED reader!");

        lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(body, book_red, 0);

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