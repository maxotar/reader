#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

void touch_init(i2c_master_dev_handle_t *touch_dev);

typedef struct
{
    i2c_master_dev_handle_t touch_dev;
    volatile bool *is_touching;
    volatile uint16_t *touch_y;
    TaskHandle_t *render_task_handle;
} touch_runtime_context_t;

void touch_runtime_init_context(touch_runtime_context_t *ctx,
                                i2c_master_dev_handle_t touch_dev,
                                volatile bool *is_touching,
                                volatile uint16_t *touch_y,
                                TaskHandle_t *render_task_handle);

void touch_poll_task(void *arg);

#endif
