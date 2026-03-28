/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/input/button.h>

#include "button.h"
#include "clip_event.h"

LOG_MODULE_REGISTER(button, CONFIG_CLIP_LOG_LEVEL);

/* Button device from device tree */
static const struct device *button_dev = DEVICE_DT_GET(DT_NODELABEL(usr_btn));

/* Callback function and user data */
static button_callback_t button_cb = NULL;
static void *button_user_data = NULL;

/* Track power-off screen state for two-step shutdown */
static atomic_t poweroff_screen_active = ATOMIC_INIT(0);

static void button_event_callback(const struct device *dev, enum button_action action)
{
    ARG_UNUSED(dev);
    enum clip_state state = clip_event_get_state();

    switch (action) {
    case BUTTON_SINGLE_CLICK:
        if (state == CLIP_STATE_RECORDING || state == CLIP_STATE_PAUSED) {
            clip_post_event(CLIP_EVENT_MARK);
        } else if (state == CLIP_STATE_IDLE || state == CLIP_STATE_ERROR
                   || state == CLIP_STATE_WIFI_SYNC) {
            clip_post_event(CLIP_EVENT_STATUS_SHOW);
        }
        break;

    case BUTTON_LONG_PRESS:
        if (state == CLIP_STATE_RECORDING) {
            clip_post_event(CLIP_EVENT_STOP);
        } else if (state == CLIP_STATE_IDLE || state == CLIP_STATE_ERROR) {
            clip_post_event(CLIP_EVENT_START);
        }
        break;

    case BUTTON_LONG_PRESS_LEVEL_1:
    case BUTTON_LONG_PRESS_LEVEL_2:
    case BUTTON_LONG_PRESS_LEVEL_3:
        clip_post_event(CLIP_EVENT_POWER_OFF_SHOW);
        atomic_set(&poweroff_screen_active, 1);
        break;

    case BUTTON_RELEASE:
        if (atomic_cas(&poweroff_screen_active, 1, 0)) {
            clip_post_event(CLIP_EVENT_POWER_OFF_EXEC);
        }
        break;

    case BUTTON_DOUBLE_CLICK:
        break;

    default:
        break;
    }

    if (button_cb) {
        button_cb(action, button_user_data);
    }
}

int button_init(void)
{
    int err;

    if (!device_is_ready(button_dev)) {
        LOG_ERR("Button device not ready");
        return -ENODEV;
    }

    err = button_callback_register(button_dev, button_event_callback);
    if (err < 0) {
        LOG_ERR("Failed to register button callback: %d", err);
        return err;
    }

    LOG_INF("Button handler initialized");
    return 0;
}

bool button_is_ready(void)
{
    return device_is_ready(button_dev);
}

int button_register_callback(button_callback_t callback, void *user_data)
{
    if (!callback) {
        return -EINVAL;
    }

    button_cb = callback;
    button_user_data = user_data;

    return 0;
}
