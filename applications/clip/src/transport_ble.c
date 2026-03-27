/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
* SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include "transport_ble.h"
#include "ble.h"

LOG_MODULE_REGISTER(transport_ble, CONFIG_CLIP_LOG_LEVEL);

/* BLE Transport Context */
static struct {
    struct transport tp;
    transport_event_cb_t event_cb;
    void *user_data;
} ble_ctx = {
    .tp = {
        .type = TRANSPORT_TYPE_BLE,
        .ready = false,
        .conn = NULL,
        .ops = NULL,
    },
};

/* BLE Transport Operations */
static int ble_transport_send(const uint8_t *data, uint16_t len)
{
    return ble_send(data, len);
}

static int ble_transport_send_file_data(const uint8_t *data, uint16_t len)
{
    return ble_send_file_data(data, len);
}

static bool ble_transport_is_connected(void)
{
    /* BLE is ready for file transfer only if all conditions are met */
    bool conn = ble_is_connected();
    bool notify = ble_is_notify_enabled();
    bool file_data_notify = ble_is_file_data_notify_enabled();

    return conn && notify && file_data_notify;
}

static void *ble_transport_get_conn(void)
{
    return ble_get_connection();
}

static int ble_transport_send_file_start(const char *session_id,
                                          const char *filename, uint32_t size)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"event\":\"file_ready\","
                       "\"session\":\"%s\",\"filename\":\"%s\",\"size\":%u}",
                       session_id, filename, size);
    if (len < 0 || len >= (int)sizeof(buf)) {
        return -EINVAL;
    }
    return ble_send((const uint8_t *)buf, len);
}

static int ble_transport_send_file_end(const char *filename)
{
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"event\":\"file_complete\","
                       "\"filename\":\"%s\"}",
                       filename);
    if (len < 0 || len >= (int)sizeof(buf)) {
        return -EINVAL;
    }
    return ble_send((const uint8_t *)buf, len);
}

static int ble_transport_send_transfer_done(const char *session_id,
                                             uint32_t file_count)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"ok\":true,\"event\":\"transfer_complete\","
                       "\"session_id\":\"%s\",\"files\":%u}",
                       session_id, file_count);
    if (len < 0 || len >= (int)sizeof(buf)) {
        return -EINVAL;
    }
    return ble_send((const uint8_t *)buf, len);
}

static const struct transport_ops ble_transport_ops = {
    .send = ble_transport_send,
    .send_file_data = ble_transport_send_file_data,
    .send_file_start = ble_transport_send_file_start,
    .send_file_end = ble_transport_send_file_end,
    .send_transfer_done = ble_transport_send_transfer_done,
    .is_connected = ble_transport_is_connected,
    .get_conn = ble_transport_get_conn,
};

/* BLE command callback wrapper */
static int ble_cmd_callback(const uint8_t *data, uint16_t len)
{
    LOG_DBG("BLE received: %u bytes", len);

    /* Notify transport layer */
    if (ble_ctx.event_cb) {
        ble_ctx.event_cb(TRANSPORT_TYPE_BLE, TRANSPORT_EVT_DATA_RECEIVED,
                        data, len, ble_ctx.user_data);
    }

    return 0;
}

/* Initialize BLE transport */
int transport_ble_init(void)
{
    ble_ctx.tp.ops = &ble_transport_ops;
    ble_ctx.tp.ready = false;
    ble_ctx.tp.conn = NULL;

    LOG_INF("BLE transport initialized");
    return 0;
}

/* Register BLE event callback */
int transport_ble_register_callback(transport_event_cb_t callback)
{
    ble_ctx.tp.event_cb = callback;
    ble_ctx.tp.user_data = &ble_ctx.tp;
    ble_ctx.event_cb = callback;
    ble_ctx.user_data = &ble_ctx.tp;

    /* Register BLE command callback */
    return ble_register_cmd_callback(ble_cmd_callback);
}

/* Update BLE connection status */
void transport_ble_update_connection(void *conn, bool ready)
{
    bool was_ready = ble_ctx.tp.ready;

    ble_ctx.tp.conn = conn;
    ble_ctx.tp.ready = ready;

    if (ready && !was_ready) {
        LOG_INF("BLE transport ready");
        if (ble_ctx.event_cb) {
            ble_ctx.event_cb(TRANSPORT_TYPE_BLE, TRANSPORT_EVT_READY,
                           NULL, 0, ble_ctx.user_data);
        }
    } else if (!ready && was_ready) {
        LOG_INF("BLE transport not ready");
        if (ble_ctx.event_cb) {
            ble_ctx.event_cb(TRANSPORT_TYPE_BLE, TRANSPORT_EVT_NOT_READY,
                           NULL, 0, ble_ctx.user_data);
        }
    }
}

/* Get BLE transport structure */
struct transport *transport_ble_get(void)
{
    return &ble_ctx.tp;
}
