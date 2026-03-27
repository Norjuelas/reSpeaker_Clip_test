/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Central event dispatcher for CLIP device.
 *
 * Button presses and AT commands post events here.
 * The event handler validates state transitions, updates state,
 * and triggers all side effects (audio, haptic, display).
 */

#ifndef CLIP_EVENT_H
#define CLIP_EVENT_H

#include <zephyr/kernel.h>
#include "clip.h"

/**
 * @brief Device-level events from user input or AT commands.
 */
enum clip_event {
    CLIP_EVENT_START = 0,
    CLIP_EVENT_STOP,
    CLIP_EVENT_PAUSE,
    CLIP_EVENT_RESUME,
    CLIP_EVENT_MARK,
    CLIP_EVENT_WIFI_ON,
    CLIP_EVENT_WIFI_OFF,
    CLIP_EVENT_POWER_OFF_SHOW,
    CLIP_EVENT_POWER_OFF_EXEC,
    CLIP_EVENT_STATUS_SHOW,
    CLIP_EVENT_USB_CONNECTED,
    CLIP_EVENT_OTA_START,
    CLIP_EVENT_OTA_DONE,
    CLIP_EVENT_COUNT,
};

/**
 * @brief Event result codes
 */
enum clip_event_result {
    CLIP_EVENT_OK = 0,
    CLIP_EVENT_INVALID,
    CLIP_EVENT_BUSY,
    CLIP_EVENT_ERROR,
};

/**
 * @brief Event handler result info (for sync callers)
 */
struct clip_event_result_info {
    enum clip_event_result result;
    int error_code;
};

/**
 * @brief Post an event (non-blocking, for button presses)
 */
int clip_post_event(enum clip_event event);

/**
 * @brief Post an event and wait for result (blocking, for AT commands)
 */
int clip_post_event_sync(enum clip_event event,
                         struct clip_event_result_info *info);

/**
 * @brief Get current device state (thread-safe)
 */
enum clip_state clip_event_get_state(void);

/**
 * @brief Initialize the event dispatcher
 */
int clip_event_init(void);

#endif /* CLIP_EVENT_H */
