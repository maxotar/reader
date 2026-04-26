#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"

void display_init(esp_lcd_panel_handle_t *panel_out,
                  esp_lcd_panel_io_handle_t *io_out,
                  esp_lcd_panel_io_color_trans_done_cb_t flush_done_cb,
                  void *flush_done_ctx);

esp_err_t display_panel_sleep(esp_lcd_panel_handle_t panel);
esp_err_t display_panel_wake(esp_lcd_panel_handle_t panel);

#endif
