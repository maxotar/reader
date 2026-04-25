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

#define CONTENT_X_PADDING 6
#define CONTENT_WIDTH (LCD_H_RES - (CONTENT_X_PADDING * 2))
#define TITLE_Y 8
#define BODY_Y 58
#define BODY_LINE_SPACE 4
#define BODY_PARAGRAPH_GAP 18
#define CONTENT_BOTTOM_PADDING 10

#define SCROLL_ACTIVE_FRAME_MS 1
#define SCROLL_IDLE_SLEEP_MS 8
#define SCROLL_REDRAW_THRESHOLD_PX 1
#define TOUCH_POLL_MS 5

// Left-half touch gestures for menu/control entry points.
#define LEFT_CONTROL_HOLD_MS 2000
#define LEFT_CONTROL_MAX_MOVE_PX 12

// Touch-position scroll control:
// - top 1/6 of screen => fixed reverse speed
// - next 1/6 => deadzone
// - lower 4/6 => nonlinear forward speed mapping (sqrt curve in runtime)
#define TOUCH_SCROLL_REVERSE_PX_PER_FRAME 1
#define TOUCH_SCROLL_FORWARD_MAX_PX_PER_FRAME 1

#define I2C_MASTER_SCL_IO 39
#define I2C_MASTER_SDA_IO 40
#define I2C_ADDR_FT3168 0x38

#endif
