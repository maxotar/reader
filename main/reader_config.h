#ifndef READER_CONFIG_H
#define READER_CONFIG_H

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

// lv_img_header_t encodes width/height in 11-bit fields: max 2047px. Silent truncation above this.
// TILE_HEIGHT stays below that limit so every runtime tile remains safe to rasterize with LVGL.
#define TILE_HEIGHT 1024
#define RUNTIME_TILE_BUFFERS 3

#define CONTENT_X_PADDING 14
#define CONTENT_WIDTH (LCD_H_RES - (CONTENT_X_PADDING * 2))
#define TITLE_Y 20
#define BODY_Y 80
#define BODY_LINE_SPACE 4
#define BODY_PARAGRAPH_GAP 18
#define CONTENT_BOTTOM_PADDING 24

#define SCROLL_ACTIVE_FRAME_MS 1
#define SCROLL_IDLE_SLEEP_MS 8
#define SCROLL_REDRAW_THRESHOLD_PX 1
#define TOUCH_POLL_MS 5

#define I2C_MASTER_SCL_IO 39
#define I2C_MASTER_SDA_IO 40
#define I2C_ADDR_FT3168 0x38

#endif
