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

/* --- Hardware Pin Definitions --- */
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

/* --- LVGL Globals --- */
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_mutex;
esp_lcd_panel_handle_t panel_handle = NULL;

LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_24);

/* --- Display Initialization Commands --- */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* --- Touch Driver (FT3168) --- */
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

uint8_t ft3168_read_touch(uint16_t *x, uint16_t *y)
{
    uint8_t touch_points_num = 0;
    uint8_t data_buf[4];
    uint8_t reg_status = 0x02;
    uint8_t reg_coords = 0x03;

    // Check how many touch points are currently active
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, I2C_ADDR_FT3168,
                                                 &reg_status, 1, &touch_points_num, 1,
                                                 pdMS_TO_TICKS(10));

    if (ret == ESP_OK && (touch_points_num & 0x0F) > 0)
    {
        // Read the actual coordinates for the first touch point
        ret = i2c_master_write_read_device(I2C_MASTER_NUM, I2C_ADDR_FT3168,
                                           &reg_coords, 1, data_buf, 4,
                                           pdMS_TO_TICKS(10));

        if (ret == ESP_OK)
        {
            uint16_t raw_y = (((uint16_t)data_buf[0] & 0x0F) << 8) | (uint16_t)data_buf[1];
            uint16_t raw_x = (((uint16_t)data_buf[2] & 0x0F) << 8) | (uint16_t)data_buf[3];

            // Swap X and Y due to the physical rotation of the touch controller vs display
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
    if (ft3168_read_touch(&tp_x, &tp_y))
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

/* --- LVGL Display Flushing --- */
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

/* --- LVGL System Tasks --- */
static void lv_tick_task(void *arg)
{
    lv_tick_inc(1);
}

static void lvgl_handler_task(void *arg)
{
    while (1)
    {
        uint32_t task_delay_ms = 5;
        if (xSemaphoreTake(lvgl_mutex, 0) == pdTRUE)
        {
            task_delay_ms = lv_timer_handler();
            xSemaphoreGive(lvgl_mutex);
        }

        // Ensure delay is within reasonable bounds (1ms to 30ms)
        if (task_delay_ms == 0)
            task_delay_ms = 1;
        else if (task_delay_ms > 30)
            task_delay_ms = 30;

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/* --- Initialization Sequences --- */
void lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing QSPI Bus...");

    // Bus Config using Macro (max_transfer_sz leverages PSRAM DMA)
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3,
        LCD_H_RES * LCD_V_RES * sizeof(uint16_t));

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // IO Config using Macro
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        PIN_NUM_CS,
        notify_lvgl_flush_ready,
        &disp_drv);

    // Performance Overrides
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10;

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    // Vendor & Panel Setup
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

    // Short delay before turning display on for stability
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

void lvgl_setup(void)
{
    lv_init();
    lvgl_mutex = xSemaphoreCreateMutex();

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
    lv_disp_drv_register(&disp_drv);

    // Tick Timer
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
    if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE)
    {
        lv_obj_t *scr = lv_scr_act();

        // 1. Create a Scrolling Container (The "Page")
        lv_obj_t *cont = lv_obj_create(scr);
        lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));

        // Strip out manual background styling. We make the container transparent
        // and borderless so it inherits the default dark theme of the active screen.
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 20, 0); // Keep padding for readability

        // Layout: Vertical stack
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);

        // Set scrollbar mode to AUTO so it only appears when scrolling
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

        // 2. Chapter Title
        lv_obj_t *title = lv_label_create(cont);
        lv_label_set_text(title, "CHAPTER II:\nTHE VOID");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_pad_bottom(title, 20, 0);

        // 3. Body Text Content (Greatly expanded to force scrolling)
        lv_obj_t *body = lv_label_create(cont);
        lv_obj_set_width(body, LV_PCT(100)); // Wrap text to container width
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_line_space(body, 8, 0);

        lv_label_set_text(body,
                          "The S3 processor hummed silently as the pixels transitioned. "
                          "In the realm of AMOLED, black is not a color—it is an absence. "
                          "Every diode turned off, saving power and preserving the deep, "
                          "infinite darkness of the display.\n\n"
                          "This is the advantage of the SH8601. When the driver is tuned correctly, "
                          "the text appears to float in the physical air of the room, unburdened "
                          "by the backlight glow of traditional LCD panels.\n\n"
                          "By relying entirely on the native LVGL Dark Theme, we allow the "
                          "framework to handle the styling autonomously. The internal memory "
                          "buffers remain pristine, preventing unnecessary styling overrides "
                          "from clogging the CPU cycles before the DMA fires.\n\n"
                          "To properly test the bounds of this interface, we must extend "
                          "our data payload. As you drag your finger across the capacitive "
                          "glass, the FT3168 touch controller interprets the disruption in "
                          "its electromagnetic field. It calculates the X and Y coordinates "
                          "in real-time, firing off an interrupt over the I2C bus.\n\n"
                          "The ESP32 catches this signal, processes it through the input "
                          "device driver, and translates the raw capacitive data into a "
                          "fluid, kinetic scroll. \n\n"
                          "If everything is calibrated perfectly, the transition should be "
                          "seamless. The pixels will ignite and extinguish at 80MHz, pushing "
                          "data across the Quad-SPI bus fast enough to trick the human eye "
                          "into seeing motion where there is only light and the void.");

        xSemaphoreGive(lvgl_mutex);
    }
}

void app_main(void)
{
    lcd_init();
    lvgl_setup();
    create_ui();
    ESP_LOGI(TAG, "Application UI created. Memory Free: %u bytes", (unsigned int)esp_get_free_heap_size());
}