#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "lvgl.h"
#include "content.h"

static const char *TAG = "reader";

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
#define BUFFER_HEIGHT 64
#define DRAW_PIXELS (LCD_H_RES * BUFFER_HEIGHT)

#define I2C_MASTER_SCL_IO 39
#define I2C_MASTER_SDA_IO 40
#define I2C_ADDR_FT3168 0x38

/* ── Colors ──────────────────────────────────────────────────── */
#define C_BG 0x000000
#define C_TITLE 0xFF2200     /* bright red for the chapter heading   */
#define C_BODY 0xCC1100      /* slightly dimmer red for body copy    */
#define C_SCROLLBAR 0x440000 /* near-invisible deep red scrollbar    */
#define C_DIVIDER 0x330000

/* ── Fonts ───────────────────────────────────────────────────── */
/* Montserrat 18 for title, 16 for body — both ship with LVGL     */
#define FONT_TITLE (&lv_font_montserrat_18)
#define FONT_BODY (&lv_font_montserrat_16)

/* ── Padding / layout ────────────────────────────────────────── */
#define PAD_H 14 /* horizontal inner padding (each side) */
#define PAD_TOP 20
#define PAD_BOT 40
#define LINE_SPACE 4 /* extra px between lines               */

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t io_handle;
static i2c_master_dev_handle_t touch_dev;

/* ── LCD init table ──────────────────────────────────────────── */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static bool flush_ready_cb(esp_lcd_panel_io_handle_t io,
                           esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    lv_disp_flush_ready(&disp_drv);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static void tick_cb(void *arg) { lv_tick_inc(1); }

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

static bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x02, data[6];
    esp_err_t r = i2c_master_transmit_receive(touch_dev, &reg, 1,
                                              data, 6, pdMS_TO_TICKS(10));
    if (r != ESP_OK || (data[0] & 0x0F) == 0)
        return false;
    *x = ((data[1] & 0x0F) << 8) | data[2];
    *y = ((data[3] & 0x0F) << 8) | data[4];
    return true;
}

static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_read(&x, &y))
    {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lcd_init(void)
{
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_SCLK, PIN_NUM_D0, PIN_NUM_D1, PIN_NUM_D2, PIN_NUM_D3,
        LCD_H_RES * BUFFER_HEIGHT * sizeof(lv_color_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_CS, flush_ready_cb, &disp_drv);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    sh8601_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1};
    esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = PIN_NUM_RST,
        .bits_per_pixel = 16,
        .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &cfg, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);
}

static void lvgl_init_display(void)
{
    lv_init();

    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

    if (!buf1 || !buf2)
    {
        ESP_LOGE(TAG, "PSRAM Allocation Failed!");
        abort();
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LCD_V_RES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    // disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t t_args = {.callback = tick_cb};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000));

    touch_init();
    static lv_indev_drv_t indev;
    lv_indev_drv_init(&indev);
    indev.type = LV_INDEV_TYPE_POINTER;
    indev.read_cb = touch_cb;
    lv_indev_drv_register(&indev);
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    /* Remove any default padding on the screen */
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ── Scrollable page container ────────────────────────────── */
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Transparent, borderless — the screen IS the page */
    lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_left(page, PAD_H, 0);
    lv_obj_set_style_pad_right(page, PAD_H, 0);
    lv_obj_set_style_pad_top(page, PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(page, PAD_BOT, 0);

    /* Vertical-only scrolling with momentum */
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN);

    /* Scrollbar: thin deep-red strip on the right */
    lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(page, lv_color_hex(C_SCROLLBAR), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(page, 0, LV_PART_SCROLLBAR);

    /* ── Column layout so children stack vertically ───────────── */
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page,
                          LV_FLEX_ALIGN_START,  /* main axis  */
                          LV_FLEX_ALIGN_START,  /* cross axis */
                          LV_FLEX_ALIGN_START); /* track      */
    lv_obj_set_style_pad_row(page, 10, 0);      /* gap between flex children */

    /* ── Chapter title ────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, chapter_title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LCD_H_RES - PAD_H * 2);
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_text_line_space(title, LINE_SPACE + 2, 0);

    /* ── Thin divider line under title ───────────────────────── */
    lv_obj_t *div = lv_obj_create(page);
    lv_obj_set_size(div, LCD_H_RES - PAD_H * 2, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(C_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Body text ────────────────────────────────────────────── */
    lv_obj_t *body = lv_label_create(page);
    lv_label_set_text(body, chapter_content);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LCD_H_RES - PAD_H * 2);
    lv_obj_set_style_text_font(body, FONT_BODY, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(C_BODY), 0);
    lv_obj_set_style_text_line_space(body, LINE_SPACE, 0);
}

static void lvgl_task(void *arg)
{
    TickType_t t = xTaskGetTickCount();
    while (1)
    {
        lv_timer_handler();
        vTaskDelayUntil(&t, pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    lcd_init();
    lvgl_init_display();
    create_ui();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);
}