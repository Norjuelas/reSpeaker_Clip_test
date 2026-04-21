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
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <nrfx_clock.h>
#include "clip_event.h"
#include "clip.h"
#include "audio.h"
#include "haptic.h"
#include "display.h"
#include "ble.h"
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
static const uint8_t transition_table[CLIP_STATE_OTA + 1][CLIP_EVENT_COUNT] = {
    /* UNINITIALIZED */ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0 },
    /* IDLE          */ { CLIP_STATE_RECORDING, 0, 0, 0, 0,
                         CLIP_STATE_WIFI_SYNC, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* RECORDING     */ { 0, CLIP_STATE_IDLE, CLIP_STATE_PAUSED, 0,
                         TRANS_SAME, 0, 0, TRANS_SAME, TRANS_SAME, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* TRANSMITTING  */ { 0, 0, 0, 0, 0, 0, 0, TRANS_SAME, TRANS_SAME, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* WIFI_SYNC     */ { 0, 0, 0, 0, 0, 0,
                         CLIP_STATE_IDLE, TRANS_SAME, TRANS_SAME, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* PAUSED        */ { 0, CLIP_STATE_IDLE, 0, CLIP_STATE_RECORDING,
                         TRANS_SAME, 0, 0, TRANS_SAME, TRANS_SAME, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* ERROR         */ { CLIP_STATE_IDLE, 0, 0, 0, 0, 0, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME },
    /* OTA           */ { 0, 0, 0, 0, 0, 0, 0,
                         TRANS_SAME, TRANS_SAME, TRANS_SAME,
                         0, TRANS_SAME, CLIP_STATE_IDLE },
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
/* OTA Progress Polling (Work Queue)                                             */
/* ========================================================================== */

#define OTA_PROGRESS_POLL_INTERVAL_MS  200

static struct k_work_delayable ota_progress_work;
static bool ota_in_progress = false;

static void ota_progress_work_handler(struct k_work *work)
{
    if (ota_in_progress && g_img_mgmt_state.size > 0) {
        size_t offset = g_img_mgmt_state.off;
        size_t total = g_img_mgmt_state.size;
        uint8_t pct = 0;

        if (offset <= total) {
            pct = (uint8_t)((offset * 100) / total);
            display_set_ota_progress(pct);
        }

        /* Reschedule work if OTA still in progress and not complete */
        if (ota_in_progress && pct < 100) {
            k_work_schedule(&ota_progress_work, K_MSEC(OTA_PROGRESS_POLL_INTERVAL_MS));
        }
    }
}

/* ========================================================================== */
/* State                                                                       */
/* ========================================================================== */

static atomic_t g_state;
static atomic_t g_boost_refcnt;

/* OTA progress tracking - store total size for percentage calculation */
static size_t g_ota_total_size = 0;
/* OTA chunk counter for fallback progress display */
static uint32_t g_ota_chunk_count = 0;

/* ========================================================================== */
/* Forward Declarations                                                         */
/* ========================================================================== */

static enum clip_event_result execute_transition(enum clip_event event,
                                                 enum clip_state from,
                                                 enum clip_state to);

/* ========================================================================== */
/* Init                                                                        */
/* ========================================================================== */

/* MCUmgr DFU callback — notifies display on OTA start/progress/pending */
static enum mgmt_cb_return mcumgr_dfu_cb(uint32_t event, enum mgmt_cb_return prev_status,
                                          int32_t *rc, uint16_t *group, bool *abort_more,
                                          void *data, size_t data_size)
{
    if (event == MGMT_EVT_OP_IMG_MGMT_DFU_STARTED) {
        LOG_INF("OTA started");
        g_ota_total_size = 0;
        g_ota_chunk_count = 0;
        ota_in_progress = true;
        /* Start periodic work to poll g_img_mgmt_state */
        k_work_schedule(&ota_progress_work, K_MSEC(OTA_PROGRESS_POLL_INTERVAL_MS));
        clip_post_event(CLIP_EVENT_OTA_START);
    } else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK) {
        /*
         * data contains struct img_mgmt_upload_check with:
         * - action->size: total image size
         * - req->off: current offset
         */
        LOG_DBG("MCUmgr: DFU CHUNK event, data=%p, data_size=%zu",
                data, data_size);

        if (data && data_size >= sizeof(struct img_mgmt_upload_check)) {
            struct img_mgmt_upload_check *check = (struct img_mgmt_upload_check *)data;

            LOG_DBG("check=%p, action=%p, req=%p",
                    check, check->action, check->req);

            /* Get total size from action (only once, on first chunk) */
            if (check->action && g_ota_total_size == 0) {
                unsigned long long action_size = check->action->size;
                if (action_size > 0) {
                    g_ota_total_size = (size_t)action_size;
                    LOG_INF("OTA size: %zu", g_ota_total_size);
                }
            }

            /* Calculate and update progress using offset if available */
            if (check->req) {
                size_t offset = check->req->off;
                size_t req_size = check->req->size;

                LOG_DBG("req: off=%zu, size=%zu", offset, req_size);

                /* Use req->size if total size not yet set */
                if (g_ota_total_size == 0 && req_size > 0 && req_size != SIZE_MAX) {
                    g_ota_total_size = req_size;
                    LOG_INF("OTA size(req): %zu", g_ota_total_size);
                }

                if (g_ota_total_size > 0 && offset <= g_ota_total_size) {
                    uint8_t pct = (uint8_t)((offset * 100) / g_ota_total_size);
                    LOG_DBG("OTA progress: %u%% (offset=%zu/%zu)",
                            pct, offset, g_ota_total_size);
                    display_set_ota_progress(pct);
                }
            }

            /* Fallback: use chunk count for visual feedback */
            g_ota_chunk_count++;
            if (g_ota_total_size == 0 && g_ota_chunk_count > 1) {
                /* Show animated progress based on chunk count (0-90%) */
                uint8_t pct = (g_ota_chunk_count % 90);
                display_set_ota_progress(pct);
                LOG_DBG("OTA fallback progress: %u%% (chunk %u)",
                        pct, g_ota_chunk_count);
            }
        } else {
            LOG_WRN("DFU: invalid data ptr");
        }
    } else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING) {
        LOG_INF("OTA done, pending reboot (%u chunks)",
                g_ota_chunk_count);
        g_ota_total_size = 0;
        g_ota_chunk_count = 0;
        ota_in_progress = false;
        /* Cancel the progress work */
        k_work_cancel_delayable(&ota_progress_work);
        display_set_ota_progress(100);
    }

    return MGMT_CB_OK;
}

static struct mgmt_callback mcumgr_dfu_cb_handler = {
    .callback = mcumgr_dfu_cb,
    .event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED | MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK |
                MGMT_EVT_OP_IMG_MGMT_DFU_PENDING,
};

int clip_event_init(void)
{
    atomic_set(&g_state, CLIP_STATE_IDLE);
    k_sem_init(&event_notify_sem, 0, 1);

    k_work_init_delayable(&ota_progress_work, ota_progress_work_handler);

    mgmt_callback_register(&mcumgr_dfu_cb_handler);

    LOG_INF("Event dispatcher initialized");
    return 0;
}

/* ========================================================================== */
/* CPU Boost (reference counted)                                                */
/* ========================================================================== */

void clip_cpu_boost_acquire(void)
{
    int ref = atomic_inc(&g_boost_refcnt);
    if (ref == 0) {
#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
        nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
        LOG_INF("CPU boost ON (128MHz)");
#endif
    }
}

void clip_cpu_boost_release(void)
{
    int ref = atomic_dec(&g_boost_refcnt);
    if (ref == 1) {
#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
        nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_2);
        LOG_INF("CPU boost OFF (64MHz)");
#endif
    }
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
            /* Sanity check: warn if state leaves RECORDING while audio active */
            if (current == CLIP_STATE_RECORDING && audio_is_recording()) {
                LOG_WRN("State leaving RECORDING (%d->%d) while audio is active",
                         current, new_state);
            }
            atomic_set(&g_state, (atomic_val_t)new_state);
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
        enum audio_mode mode = AUDIO_MODE_MERGE;  /* Both modes use merge (L+R → mono) */

        err = audio_start_recording(mode);
        if (err) {
            if (err == -EBUSY) {
                return CLIP_EVENT_BUSY;
            }
            LOG_ERR("audio_start_recording failed: %d", err);
            display_post_error("Rec Fail");
            return CLIP_EVENT_ERROR;
        }
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
            display_post_error("WiFi Fail");
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
        /* Stop recording if active, to save file before shutdown */
        if (audio_is_recording()) {
            LOG_INF("Stopping recording before power off");
            audio_stop_recording();
            k_sleep(K_MSEC(100));
        }

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
        ble_adv_restart_fast();
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
