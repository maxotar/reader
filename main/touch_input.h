#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

void touch_init(i2c_master_dev_handle_t *touch_dev);

typedef enum
{
    LEFT_CONTROL_EVENT_TAP = 0,
    LEFT_CONTROL_EVENT_HOLD,
} left_control_event_t;

typedef void (*left_control_event_cb_t)(left_control_event_t event,
                                        uint16_t x,
                                        uint16_t y,
                                        void *user_ctx);

typedef void (*menu_tap_cb_t)(uint16_t x, uint16_t y, void *user_ctx);

typedef struct
{
    i2c_master_dev_handle_t touch_dev;
    volatile bool *is_touching;
    volatile uint16_t *touch_x;
    volatile uint16_t *touch_y;
    TaskHandle_t *render_task_handle;
    left_control_event_cb_t left_control_cb;
    void *left_control_user_ctx;
    volatile bool *menu_open;
    menu_tap_cb_t menu_tap_cb;
    void *menu_tap_user_ctx;
    volatile bool *chapter_list_open;
    volatile int32_t *chapter_list_scroll_offset;
    menu_tap_cb_t chapter_list_tap_cb;
    void *chapter_list_tap_user_ctx;
} touch_runtime_context_t;

void touch_runtime_init_context(touch_runtime_context_t *ctx,
                                i2c_master_dev_handle_t touch_dev,
                                volatile bool *is_touching,
                                volatile uint16_t *touch_x,
                                volatile uint16_t *touch_y,
                                TaskHandle_t *render_task_handle,
                                left_control_event_cb_t left_control_cb,
                                void *left_control_user_ctx);

void touch_runtime_set_menu(touch_runtime_context_t *ctx,
                            volatile bool *menu_open,
                            menu_tap_cb_t menu_tap_cb,
                            void *menu_tap_user_ctx);

void touch_runtime_set_chapter_list(touch_runtime_context_t *ctx,
                                    volatile bool *chapter_list_open,
                                    volatile int32_t *chapter_list_scroll_offset,
                                    menu_tap_cb_t chapter_list_tap_cb,
                                    void *chapter_list_tap_user_ctx);

void touch_poll_task(void *arg);

#endif
