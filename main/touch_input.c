#include "touch_input.h"

#include "reader_config.h"
#include "reader_menu.h"
#include "esp_check.h"
#include <stdlib.h>

static uint16_t abs_diff_u16(uint16_t a, uint16_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static void dispatch_left_control_event(const touch_runtime_context_t *ctx,
                                        left_control_event_t event,
                                        uint16_t x,
                                        uint16_t y)
{
    if (ctx->left_control_cb)
    {
        ctx->left_control_cb(event, x, y, ctx->left_control_user_ctx);
    }
}

void touch_runtime_init_context(touch_runtime_context_t *ctx,
                                i2c_master_dev_handle_t touch_dev,
                                SemaphoreHandle_t state_mutex,
                                bool *is_touching,
                                uint16_t *touch_x,
                                uint16_t *touch_y,
                                TaskHandle_t *render_task_handle,
                                left_control_event_cb_t left_control_cb,
                                void *left_control_user_ctx)
{
    ctx->touch_dev = touch_dev;
    ctx->state_mutex = state_mutex;
    ctx->is_touching = is_touching;
    ctx->touch_x = touch_x;
    ctx->touch_y = touch_y;
    ctx->render_task_handle = render_task_handle;
    ctx->left_control_cb = left_control_cb;
    ctx->left_control_user_ctx = left_control_user_ctx;
    ctx->menu_open = NULL;
    ctx->menu_tap_cb = NULL;
    ctx->menu_tap_user_ctx = NULL;
    ctx->chapter_list_open = NULL;
    ctx->chapter_list_scroll_offset = NULL;
    ctx->chapter_list_scroll_max = 0;
    ctx->chapter_list_tap_cb = NULL;
    ctx->chapter_list_tap_user_ctx = NULL;
}

void touch_runtime_set_menu(touch_runtime_context_t *ctx,
                            bool *menu_open,
                            menu_tap_cb_t menu_tap_cb,
                            void *menu_tap_user_ctx)
{
    ctx->menu_open = menu_open;
    ctx->menu_tap_cb = menu_tap_cb;
    ctx->menu_tap_user_ctx = menu_tap_user_ctx;
}

void touch_runtime_set_chapter_list(touch_runtime_context_t *ctx,
                                    bool *chapter_list_open,
                                    int32_t *chapter_list_scroll_offset,
                                    menu_tap_cb_t chapter_list_tap_cb,
                                    void *chapter_list_tap_user_ctx)
{
    ctx->chapter_list_open = chapter_list_open;
    ctx->chapter_list_scroll_offset = chapter_list_scroll_offset;
    ctx->chapter_list_tap_cb = chapter_list_tap_cb;
    ctx->chapter_list_tap_user_ctx = chapter_list_tap_user_ctx;
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
    if (!ctx || !ctx->touch_dev || !ctx->is_touching || !ctx->touch_x || !ctx->touch_y || !ctx->render_task_handle || !ctx->state_mutex)
    {
        abort();
    }

    uint8_t reg = 0x02;
    uint8_t data[6];
    bool last_touching = false;
    uint16_t last_touch_x_sample = 0;
    uint16_t last_touch_sample = 0;

    bool left_control_tracking = false;
    bool left_control_hold_fired = false;
    bool menu_opened_by_hold = false;
    uint16_t left_control_start_x = 0;
    uint16_t left_control_start_y = 0;
    TickType_t left_control_start_tick = 0;
    int32_t chapter_list_last_y = 0;
    bool chapter_list_scrolling = false;
    bool chapter_list_dragged = false;

    // Chapter-list gesture zones (must match reader_menu.c layout semantics)
    uint16_t chapter_list_top_buttons_end_y = 66;
    uint16_t chapter_list_scroll_start_y = 98;
    uint16_t chapter_list_select_button_end_x = 64;
    reader_menu_chapter_list_gesture_bounds(&chapter_list_top_buttons_end_y,
                                            &chapter_list_scroll_start_y,
                                            &chapter_list_select_button_end_x);

    while (1)
    {
        bool next_touching = false;
        uint16_t next_touch_x = 0;
        uint16_t next_touch_y = 0;
        if (i2c_master_transmit_receive(ctx->touch_dev, &reg, 1, data, 6, pdMS_TO_TICKS(TOUCH_POLL_MS)) == ESP_OK)
        {
            next_touching = (data[0] & 0x0F) > 0;
            if (next_touching)
            {
                next_touch_x = ((data[1] & 0x0F) << 8) | data[2];
                next_touch_y = ((data[3] & 0x0F) << 8) | data[4];
            }
        }

        xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
        *ctx->is_touching = next_touching;
        *ctx->touch_x = next_touch_x;
        *ctx->touch_y = next_touch_y;
        bool menu_open_now = (ctx->menu_open && *ctx->menu_open);
        bool chapter_list_open_now = (ctx->chapter_list_open && *ctx->chapter_list_open);
        xSemaphoreGive(ctx->state_mutex);

        if (next_touching && !last_touching)
        {
            menu_opened_by_hold = false;
            if (menu_open_now)
            {
                // Menu is open: suppress hold detection, just track touch for selection.
                left_control_tracking = false;
                left_control_hold_fired = true;
            }
            else if (chapter_list_open_now)
            {
                // Chapter list is open:
                // - top strip is for Confirm/Cancel taps
                // - left strip is for chapter select buttons
                // - title area (right side) is for vertical scrolling
                left_control_tracking = false;
                left_control_hold_fired = true;
                chapter_list_scrolling =
                    (next_touch_y >= chapter_list_scroll_start_y) &&
                    (next_touch_x > chapter_list_select_button_end_x);
                chapter_list_dragged = false;
                chapter_list_last_y = next_touch_y;
            }
            else
            {
                left_control_tracking = (next_touch_x < (LCD_H_RES / 2));
                left_control_hold_fired = false;
            }
            left_control_start_x = next_touch_x;
            left_control_start_y = next_touch_y;
            left_control_start_tick = xTaskGetTickCount();
        }

        // Handle chapter list scrolling
        if (next_touching && chapter_list_scrolling && ctx->chapter_list_scroll_offset)
        {
            int32_t delta_y = (int32_t)next_touch_y - chapter_list_last_y;
            if (delta_y != 0)
                chapter_list_dragged = true;
            xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
            *ctx->chapter_list_scroll_offset -= delta_y; // Invert: dragging down scrolls up

            // Clamp scroll offset to reasonable bounds
            if (*ctx->chapter_list_scroll_offset < 0)
                *ctx->chapter_list_scroll_offset = 0;
            if (ctx->chapter_list_scroll_max > 0 &&
                *ctx->chapter_list_scroll_offset > ctx->chapter_list_scroll_max)
                *ctx->chapter_list_scroll_offset = ctx->chapter_list_scroll_max;
            xSemaphoreGive(ctx->state_mutex);

            chapter_list_last_y = next_touch_y;

            if (ctx->render_task_handle)
                xTaskNotifyGive(*ctx->render_task_handle);
        }

        if (next_touching && left_control_tracking)
        {
            if (next_touch_x >= (LCD_H_RES / 2))
            {
                left_control_tracking = false;
            }
            else
            {
                uint16_t move_x = abs_diff_u16(next_touch_x, left_control_start_x);
                uint16_t move_y = abs_diff_u16(next_touch_y, left_control_start_y);
                if (move_x > LEFT_CONTROL_MAX_MOVE_PX || move_y > LEFT_CONTROL_MAX_MOVE_PX)
                {
                    left_control_tracking = false;
                }
                else if (!left_control_hold_fired)
                {
                    TickType_t elapsed = xTaskGetTickCount() - left_control_start_tick;
                    if (elapsed >= pdMS_TO_TICKS(LEFT_CONTROL_HOLD_MS))
                    {
                        dispatch_left_control_event(ctx,
                                                    LEFT_CONTROL_EVENT_HOLD,
                                                    next_touch_x,
                                                    next_touch_y);
                        left_control_hold_fired = true;
                        menu_opened_by_hold = true;
                    }
                }
            }
        }

        if (!next_touching && last_touching)
        {
            chapter_list_scrolling = false;
            xSemaphoreTake(ctx->state_mutex, portMAX_DELAY);
            bool chapter_list_open_now = (ctx->chapter_list_open && *ctx->chapter_list_open);
            bool menu_open_now = (ctx->menu_open && *ctx->menu_open);
            xSemaphoreGive(ctx->state_mutex);

            if (chapter_list_open_now)
            {
                bool is_top_button_tap = (last_touch_sample < chapter_list_top_buttons_end_y);
                bool is_select_button_tap =
                    (last_touch_sample >= chapter_list_scroll_start_y) &&
                    (last_touch_x_sample <= chapter_list_select_button_end_x);

                // Dispatch chapter-list taps only for top buttons or row select-buttons.
                if ((is_top_button_tap || is_select_button_tap) && !chapter_list_dragged && ctx->chapter_list_tap_cb)
                {
                    ctx->chapter_list_tap_cb(last_touch_x_sample, last_touch_sample, ctx->chapter_list_tap_user_ctx);
                }
            }
            else if (menu_open_now)
            {
                if (menu_opened_by_hold)
                {
                    // Swallow the lift that ends the hold gesture — menu just opened.
                    menu_opened_by_hold = false;
                }
                else if (ctx->menu_tap_cb)
                {
                    ctx->menu_tap_cb(last_touch_x_sample, last_touch_sample, ctx->menu_tap_user_ctx);
                }
            }
            else if (left_control_tracking && !left_control_hold_fired)
            {
                dispatch_left_control_event(ctx,
                                            LEFT_CONTROL_EVENT_TAP,
                                            last_touch_x_sample,
                                            last_touch_sample);
            }
            left_control_tracking = false;
            left_control_hold_fired = false;
        }

        if (*ctx->render_task_handle &&
            ((next_touching != last_touching) ||
             (next_touching && (next_touch_x != last_touch_x_sample || next_touch_y != last_touch_sample))))
        {
            xTaskNotifyGive(*ctx->render_task_handle);
        }

        last_touching = next_touching;
        last_touch_x_sample = next_touch_x;
        last_touch_sample = next_touch_y;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}
