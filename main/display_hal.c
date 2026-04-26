#include "display_hal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_sh8601.h"
#include "reader_config.h"

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

esp_err_t display_panel_sleep(esp_lcd_panel_handle_t panel)
{
    if (!panel)
        return ESP_ERR_INVALID_ARG;

    return esp_lcd_panel_disp_on_off(panel, false);
}

esp_err_t display_panel_wake(esp_lcd_panel_handle_t panel)
{
    if (!panel)
        return ESP_ERR_INVALID_ARG;

    vTaskDelay(pdMS_TO_TICKS(120));
    return esp_lcd_panel_disp_on_off(panel, true);
}

void display_init(esp_lcd_panel_handle_t *panel_out,
                  esp_lcd_panel_io_handle_t *io_out,
                  esp_lcd_panel_io_color_trans_done_cb_t flush_done_cb,
                  void *flush_done_ctx)
{
    spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_NUM_SCLK,
                                                           PIN_NUM_D0,
                                                           PIN_NUM_D1,
                                                           PIN_NUM_D2,
                                                           PIN_NUM_D3,
                                                           (LCD_H_RES * LCD_V_RES * 2));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_CS, flush_done_cb, flush_done_ctx);
    io_config.trans_queue_depth = 3;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, io_out));

    sh8601_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = PIN_NUM_RST,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(*io_out, &cfg, panel_out));
    esp_lcd_panel_reset(*panel_out);
    esp_lcd_panel_init(*panel_out);
    ESP_ERROR_CHECK(display_panel_wake(*panel_out));
}
