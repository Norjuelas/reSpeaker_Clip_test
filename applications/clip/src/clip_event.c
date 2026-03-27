/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Central event dispatcher — table-driven state machine.
 *
 * All state transitions and side effects (audio, haptic, display)
 * are handled here. Button and AT command modules only post events.
 *
 * Events are processed in the main thread (via clip_event_process()),
 * which provides a large enough stack for WiFi ON/OFF operations.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/regulator.h>
#include "clip_event.h"
#include "clip.h"
#include "audio.h"
#include "haptic.h"
#include "display.h"
#include "wifi.h"

LOG_MODULE_REGISTER(clip_event, CONFIG_CLIP_LOG_LEVEL);

/* ========================================================================== */
/* State Transition Table                                                       */
/* ========================================================================== */

#define TRANS_INVALID  0   /* No valid transition */
#define TRANS_SAME     255 /* Stay in current state (MARK, STATUS, POWER_OFF_SHOW) */

/*
 * transition_table[current_state][event] = next_state
 *
 *                  START  STOP   PAUSE  RESUME MARK   WIFI_ON WIFI_OFF POFF_S POFF_E STATUS USB   OTA_S OTA_D
 */
static const uint8_t transition_table[CLIP_STATE_ERROR + 1][CLIP_EVENT_COUNT] = {
    /* UNINITIALIZED */ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0 },
    /* IDLE          */ { CLIP_STATE_RECORDING, 0, 0, 0, 0,
                         CLIP_STATE_WIFI_SYNC, 0,
                         TRANS_SAME, 0, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* RECORDING     */ { 0, CLIP_STATE_IDLE, CLIP_STATE_PAUSED, 0,
                         TRANS_SAME, 0, 0, TRANS_SAME, 0, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* TRANSMITTING  */ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* WIFI_SYNC     */ { 0, 0, 0, 0, 0, 0,
                         CLIP_STATE_IDLE, TRANS_SAME, 0, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* PAUSED        */ { 0, CLIP_STATE_IDLE, 0, CLIP_STATE_RECORDING,
                         TRANS_SAME, 0, 0, TRANS_SAME, 0, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* ERROR         */ { CLIP_STATE_IDLE, 0, 0, 0, 0, 0, 0,
                         TRANS_SAME, 0, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
};

/* ========================================================================== */
/* Event Queue                                                                 */
/* ========================================================================== */

struct clip_event_item {
    enum clip_event event;
    struct k_sem *done_sem;
    struct clip_event_result_info *result;
};

#define EVENT_QUEUE_SIZE 8
K_MSGQ_DEFINE(clip_ev_msgq, sizeof(struct clip_event_item),
              EVENT_QUEUE_SIZE, 4);

/* ========================================================================== */
/* Event Notification Semaphore                                               */
/* ========================================================================== */

/* Signaled when a new event is queued; main loop waits on this */
struct k_sem event_notify_sem;

/* ========================================================================== */
/* State                                                                       */
/* ========================================================================== */

static atomic_t g_state;

/* ========================================================================== */
/* Forward Declarations                                                         */
/* ========================================================================== */

static enum clip_event_result execute_transition(enum clip_event event,
                                                 enum clip_state from,
                                                 enum clip_state to);

/* ========================================================================== */
/* Init                                                                        */
/* ========================================================================== */

int clip_event_init(void)
{
    atomic_set(&g_state, CLIP_STATE_IDLE);
    k_sem_init(&event_notify_sem, 0, 1);

    LOG_INF("Event dispatcher initialized");
    return 0;
}

enum clip_state clip_event_get_state(void)
{
    return (enum clip_state)atomic_get(&g_state);
}

/* ========================================================================== */
/* Event Submission                                                            */
/* ========================================================================== */

int clip_post_event(enum clip_event event)
{
    struct clip_event_item item = { .event = event };

    int ret = k_msgq_put(&clip_ev_msgq, &item, K_NO_WAIT);
    if (ret != 0) {
        LOG_WRN("Event queue full, dropping event %d", event);
        return ret;
    }

    k_sem_give(&event_notify_sem);
    return 0;
}

int clip_post_event_sync(enum clip_event event,
                         struct clip_event_result_info *info)
{
    struct k_sem sem;
    k_sem_init(&sem, 0, 1);

    struct clip_event_item item = {
        .event = event,
        .done_sem = &sem,
        .result = info,
    };

    int ret = k_msgq_put(&clip_ev_msgq, &item, K_NO_WAIT);
    if (ret != 0) {
        if (info) {
            info->result = CLIP_EVENT_BUSY;
            info->error_code = ret;
        }
        return ret;
    }

    k_sem_give(&event_notify_sem);
    k_sem_take(&sem, K_FOREVER);
    return 0;
}

/* ========================================================================== */
/* Event Processing — called from main thread                                   */
/* ========================================================================== */

void clip_event_wait(k_timeout_t timeout)
{
    k_sem_take(&event_notify_sem, timeout);
}

void clip_event_process(void)
{
    struct clip_event_item item;

    while (k_msgq_get(&clip_ev_msgq, &item, K_NO_WAIT) == 0) {
        enum clip_state current = (enum clip_state)atomic_get(&g_state);

        if (item.event >= CLIP_EVENT_COUNT) {
            LOG_WRN("Invalid event: %d", item.event);
            goto notify;
        }

        uint8_t next = transition_table[current][item.event];
        if (next == TRANS_INVALID) {
            LOG_WRN("Invalid transition: state=%d event=%d", current, item.event);
            if (item.result) {
                item.result->result = CLIP_EVENT_INVALID;
            }
            goto notify;
        }

        enum clip_state new_state = (next == TRANS_SAME) ? current
                                                         : (enum clip_state)next;

        enum clip_event_result result = execute_transition(item.event, current, new_state);

        if (result == CLIP_EVENT_OK && next != TRANS_SAME) {
            atomic_set(&g_state, (atomic_val_t)new_state);
            clip_get_context()->state = new_state;
        }

        if (item.result) {
            item.result->result = result;
        }

notify:
        if (item.done_sem) {
            k_sem_give(item.done_sem);
        }
    }
}

/* ========================================================================== */
/* Transition Actions — single place for all side effects                     */
/* ========================================================================== */

static enum clip_event_result execute_transition(enum clip_event event,
                                                 enum clip_state from,
                                                 enum clip_state to)
{
    struct clip_context *c = clip_get_context();
    int err;

    switch (event) {
    case CLIP_EVENT_START:
    {
        enum audio_mode mode = (c->config.mode == MODE_NORMAL)
                              ? AUDIO_MODE_STEREO
                              : AUDIO_MODE_MERGE;

        err = audio_start_recording(mode);
        if (err) {
            if (err == -EBUSY) {
                return CLIP_EVENT_BUSY;
            }
            LOG_ERR("audio_start_recording failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        haptic_play_pattern(HAPTIC_SHORT);
        display_post_event(UI_EVENT_REC_START);
        display_set_recording(true, c->config.mode == MODE_ENHANCED);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_STOP:
    {
        if (!audio_is_recording()) {
            return CLIP_EVENT_INVALID;
        }

        err = audio_stop_recording();
        if (err) {
            LOG_ERR("audio_stop_recording failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        haptic_play_pattern(HAPTIC_SHORT);
        display_post_event(UI_EVENT_REC_STOP);
        display_set_recording(false, false);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_PAUSE:
    {
        if (!audio_is_recording()) {
            return CLIP_EVENT_INVALID;
        }

        err = audio_pause_recording();
        if (err) {
            LOG_ERR("audio_pause_recording failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        haptic_play_pattern(HAPTIC_SHORT);
        display_post_event(UI_EVENT_REC_PAUSE);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_RESUME:
    {
        err = audio_resume_recording();
        if (err) {
            LOG_ERR("audio_resume_recording failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        haptic_play_pattern(HAPTIC_SHORT);
        display_post_event(UI_EVENT_REC_RESUME);
        display_set_recording(true, c->config.mode == MODE_ENHANCED);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_MARK:
    {
        if (!audio_is_recording()) {
            return CLIP_EVENT_INVALID;
        }

        err = audio_add_bookmark();
        if (err) {
            LOG_ERR("audio_add_bookmark failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        display_post_event(UI_EVENT_MARK);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_WIFI_ON:
    {
        if (wifi_ap_is_running()) {
            return CLIP_EVENT_OK;
        }

        err = wifi_on();
        if (err) {
            LOG_ERR("wifi_on failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_WIFI_OFF:
    {
        err = wifi_off();
        if (err) {
            LOG_ERR("wifi_off failed: %d", err);
            return CLIP_EVENT_ERROR;
        }
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_POWER_OFF_SHOW:
    {
        display_post_event(UI_EVENT_POWER_OFF_SHOW);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_POWER_OFF_EXEC:
    {
        haptic_play_pattern(HAPTIC_DOUBLE);
        k_sleep(K_MSEC(400));
        display_post_event(UI_EVENT_TIMEOUT);

        const struct device *regulators =
            DEVICE_DT_GET(DT_NODELABEL(npm1300_regulators));
        if (!device_is_ready(regulators)) {
            LOG_ERR("Regulators not ready for ship mode");
            return CLIP_EVENT_ERROR;
        }

        err = regulator_parent_ship_mode(regulators);
        if (err) {
            LOG_ERR("Failed to enter ship mode: %d", err);
            return CLIP_EVENT_ERROR;
        }
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_STATUS_SHOW:
    {
        display_post_event(UI_EVENT_STATUS_SHOW);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_USB_CONNECTED:
    {
        display_post_event(UI_EVENT_USB_CONNECTED);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_OTA_START:
    {
        display_post_event(UI_EVENT_OTA_START);
        return CLIP_EVENT_OK;
    }

    case CLIP_EVENT_OTA_DONE:
    {
        display_post_event(UI_EVENT_OTA_DONE);
        return CLIP_EVENT_OK;
    }

    default:
        return CLIP_EVENT_INVALID;
    }
}
