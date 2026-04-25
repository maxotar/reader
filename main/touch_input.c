#include "touch_input.h"

#include "reader_config.h"
#include "esp_check.h"
#include <stdlib.h>

void touch_runtime_init_context(touch_runtime_context_t *ctx,
                                i2c_master_dev_handle_t touch_dev,
                                volatile bool *is_touching,
                                volatile uint16_t *touch_y,
                                TaskHandle_t *render_task_handle)
{
    ctx->touch_dev = touch_dev;
    ctx->is_touching = is_touching;
    ctx->touch_y = touch_y;
    ctx->render_task_handle = render_task_handle;
}

void touch_init(i2c_master_dev_handle_t *touch_dev)
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
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, touch_dev));
}

void touch_poll_task(void *arg)
{
    touch_runtime_context_t *ctx = (touch_runtime_context_t *)arg;
    if (!ctx || !ctx->touch_dev || !ctx->is_touching || !ctx->touch_y || !ctx->render_task_handle)
    {
        abort();
    }

    uint8_t reg = 0x02;
    uint8_t data[6];
    bool last_touching = false;
    uint16_t last_touch_sample = 0;

    while (1)
    {
        bool next_touching = false;
        uint16_t next_touch_y = 0;
        if (i2c_master_transmit_receive(ctx->touch_dev, &reg, 1, data, 6, pdMS_TO_TICKS(TOUCH_POLL_MS)) == ESP_OK)
        {
            next_touching = (data[0] & 0x0F) > 0;
            if (next_touching)
                next_touch_y = ((data[3] & 0x0F) << 8) | data[4];
        }

        *ctx->is_touching = next_touching;
        *ctx->touch_y = next_touch_y;

        if (*ctx->render_task_handle &&
            ((next_touching != last_touching) || (next_touching && next_touch_y != last_touch_sample)))
        {
            xTaskNotifyGive(*ctx->render_task_handle);
        }

        last_touching = next_touching;
        last_touch_sample = next_touch_y;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}
