#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_err.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"

static const char *TAG = "amoled";

/* QSPI pins from Waveshare schematic */

#define LCD_HOST SPI2_HOST

#define PIN_NUM_SCLK 47

#define PIN_NUM_D0 18
#define PIN_NUM_D1 7
#define PIN_NUM_D2 48
#define PIN_NUM_D3 5

#define PIN_NUM_CS 6
#define PIN_NUM_RST 17

/* portrait resolution */

#define LCD_H_RES 240
#define LCD_V_RES 536

/* portrait initialization sequence */

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

esp_lcd_panel_handle_t panel;

/* initialize display */

void lcd_init(void)
{
    ESP_LOGI(TAG, "init QSPI bus");

    spi_bus_config_t buscfg =
        SH8601_PANEL_BUS_QSPI_CONFIG(
            PIN_NUM_SCLK,
            PIN_NUM_D0,
            PIN_NUM_D1,
            PIN_NUM_D2,
            PIN_NUM_D3,
            LCD_H_RES * 80 * sizeof(uint16_t));

    ESP_ERROR_CHECK(
        spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "install panel io");

    esp_lcd_panel_io_handle_t io_handle;

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(
            PIN_NUM_CS,
            NULL,
            NULL);

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            LCD_HOST,
            &io_config,
            &io_handle));

    ESP_LOGI(TAG, "install SH8601 driver");

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1}};

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config};

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_sh8601(
            io_handle,
            &panel_config,
            &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "display ready");
}

static inline uint16_t rgb565(uint16_t c)
{
    return (c << 8) | (c >> 8);
}

void fill_color(uint16_t color)
{
    static uint16_t buffer[LCD_H_RES * 40];

    for (int i = 0; i < LCD_H_RES * 40; i++)
        buffer[i] = rgb565(color);

    for (int y = 0; y < LCD_V_RES; y += 40)
    {
        esp_lcd_panel_draw_bitmap(
            panel,
            0,
            y,
            LCD_H_RES,
            y + 40,
            buffer);
    }
}

void draw_rect(int x, int y, int w, int h, uint16_t color)
{
    static uint16_t line[LCD_H_RES];

    uint16_t c = rgb565(color);

    for (int i = 0; i < w; i++)
        line[i] = c;

    for (int j = 0; j < h; j++)
    {
        esp_lcd_panel_draw_bitmap(
            panel,
            x,
            y + j,
            x + w,
            y + j + 1,
            line);
    }
}

void app_main(void)
{
    lcd_init();

    int rect_w = 120;
    int rect_h = 200;

    int x = (LCD_H_RES - rect_w) / 2;
    int y = (LCD_V_RES - rect_h) / 2;

    uint16_t colors[] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFFF  // white
    };

    int idx = 0;

    while (1)
    {
        draw_rect(x, y, rect_w, rect_h, colors[idx]);

        idx = (idx + 1) % 4;

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}