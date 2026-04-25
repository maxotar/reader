#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

void display_init(esp_lcd_panel_handle_t *panel_out,
                  esp_lcd_panel_io_handle_t *io_out,
                  esp_lcd_panel_io_color_trans_done_cb_t flush_done_cb,
                  void *flush_done_ctx);

#endif
