#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "lvgl.h"

#include "content.h"

static const char *TAG = "amoled_canvas";

/* --- Hardware Pins --- */
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
#define I2C_ADDR_FT3168 0x38

#define LCD_H_RES 240
#define LCD_V_RES 536

/* --- LVGL Configuration --- */
#define DRAW_BUF_HEIGHT (LCD_V_RES / 4)
#define DRAW_BUF_STRIDE (LCD_H_RES * DRAW_BUF_HEIGHT)

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static SemaphoreHandle_t lvgl_mutex;
esp_lcd_panel_handle_t panel_handle = NULL;
i2c_master_dev_handle_t dev_handle;

/* --- Display Commands --- */
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
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_FT3168,
        .scl_speed_hz = 400000,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));
}

uint8_t ft3168_read_touch(uint16_t *x, uint16_t *y)
{
    uint8_t data_buf[6];
    uint8_t reg_start = 0x02;

    esp_err_t ret = i2c_master_transmit_receive(dev_handle, &reg_start, 1, data_buf, 6, -1);

    if (ret == ESP_OK && (data_buf[0] & 0x0F) > 0)
    {
        uint16_t raw_y = ((uint16_t)(data_buf[1] & 0x0F) << 8) | data_buf[2];
        uint16_t raw_x = ((uint16_t)(data_buf[3] & 0x0F) << 8) | data_buf[4];
        *x = raw_y;
        *y = raw_x;
        return 1;
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

/* --- LVGL Callbacks --- */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lv_tick_task(void *arg) { lv_tick_inc(1); }

static void lvgl_handler_task(void *arg)
{
    while (1)
    {
        uint32_t delay = 10;
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            delay = lv_timer_handler();
            xSemaphoreGive(lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(delay < 5 ? 5 : delay));
    }
}

/* --- Core Logic --- */
void lcd_init(void)
{
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3, LCD_H_RES * 80);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_CS, notify_lvgl_flush_ready, &disp_drv);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {.init_cmds = lcd_init_cmds, .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]), .flags.use_qspi_interface = 1};
    esp_lcd_panel_dev_config_t panel_config = {.reset_gpio_num = PIN_NUM_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vendor_config};
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_disp_on_off(panel_handle, true);
}

static DMA_ATTR uint16_t draw_buf_1[DRAW_BUF_STRIDE];
static DMA_ATTR uint16_t draw_buf_2[DRAW_BUF_STRIDE];

void lvgl_setup(void)
{
    lv_init();
    lvgl_mutex = xSemaphoreCreateMutex();
    lv_disp_draw_buf_init(&draw_buf, draw_buf_1, draw_buf_2, DRAW_BUF_STRIDE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {.callback = &lv_tick_task, .name = "tick"};
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    touch_i2c_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    xTaskCreatePinnedToCore(lvgl_handler_task, "LVGL", 8192, NULL, 5, NULL, 1);
}

void create_ui(void)
{
    if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE)
    {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

        // Scrollable Container
        lv_obj_t *cont = lv_obj_create(scr);
        lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_bg_opa(cont, 0, 0);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);

        // Canvas Setup (Height 2000px)
        uint16_t canvas_h = 2000;
        size_t cbuf_size = LCD_H_RES * canvas_h * sizeof(lv_color_t);
        lv_color_t *cbuf = heap_caps_malloc(cbuf_size, MALLOC_CAP_SPIRAM);

        if (cbuf)
        {
            lv_obj_t *canvas = lv_canvas_create(cont);
            lv_canvas_set_buffer(canvas, cbuf, LCD_H_RES, canvas_h, LV_IMG_CF_TRUE_COLOR);
            lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);
            lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);

            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.color = lv_color_hex(0xFFFFFF);
            dsc.font = &lv_font_montserrat_24;
            lv_canvas_draw_text(canvas, 10, 20, 220, &dsc, book_title);

            dsc.font = &lv_font_montserrat_18;
            dsc.color = lv_color_hex(0xD0D0D0);
            // Drawing the body text from content.h
            lv_canvas_draw_text(canvas, 10, 70, 220, &dsc, book_content);
        }

        xSemaphoreGive(lvgl_mutex);
        ESP_LOGI(TAG, "UI created with single canvas in PSRAM.");
    }
}

void app_main(void)
{
    lcd_init();
    lvgl_setup();
    create_ui();
}