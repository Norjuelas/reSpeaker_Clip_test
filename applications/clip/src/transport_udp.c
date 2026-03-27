/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP Transport v2 — Sliding window with selective ACK, per-frame CRC32,
 * automatic retransmission, and flow control.
 *
 * Protocol: see docs/udp_protocol.md (CLIP UDP Transfer Protocol v2)
 *
 * Memory usage: ~17KB for send window buffer (32 frames × ~520 bytes each).
 * Retransmission reads from this buffer — no SD card re-read needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <errno.h>
#include "transport.h"
#include "transport_udp.h"
#include "wifi_udp.h"

LOG_MODULE_REGISTER(transport_udp, CONFIG_CLIP_LOG_LEVEL);

/* Shared socket from wifi_udp.c */
extern int server_sock;

/* ---- Sliding window configuration (from Kconfig) ---- */
#define WINDOW_SIZE          CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE
#define MAX_FRAME_SIZE       (UDP_DATA_HEADER_SIZE + UDP_MAX_DATA_PER_FRAME)
#define RETRANSMIT_INTERVAL  100   /* ms between retransmit checks */
#define RETRANSMIT_TIMEOUT   200   /* ms before a frame is considered lost */
#define ACK_WAIT_TIMEOUT     2000  /* ms to wait for control-frame ACK */

/* ---- Send window slot ---- */
struct frame_slot {
    uint16_t seq;
    uint16_t len;           /* total frame length (header + data) */
    uint8_t frame[MAX_FRAME_SIZE];
    bool valid;
    int retries;
    int64_t last_sent;      /* uptime ms when last sent */
};

/* ---- State ---- */
static struct k_mutex udp_mutex;
static volatile bool udp_ready;
static struct sockaddr_in udp_client_addr;
static socklen_t udp_client_len;

/* Sliding window */
static struct frame_slot send_window[WINDOW_SIZE];
static uint16_t send_base;        /* oldest unACKed seq */
static uint16_t next_seq;         /* next seq to assign */
static uint8_t peer_window;       /* receiver's advertised window */

/* Flow control: semaphore counts available window slots */
static struct k_sem window_sem;

/* Control frame ACK: used by wait_for_control_ack via notify_ack */
static struct k_sem control_ack_sem;
static volatile uint16_t control_ack_expected;

/* Retransmission timer */
static struct k_work_delayable retransmit_work;

/* Heartbeat */
static int64_t last_activity_time;
static struct k_timer heartbeat_timer;
static struct k_work heartbeat_work;

/* Per-file transfer state */
static char current_filename[64];
static uint32_t current_file_size;
static uint32_t current_bytes_sent;
static uint32_t current_file_crc;

/* ---- Forward declarations ---- */
static int udp_send(const uint8_t *data, uint16_t len);
static int udp_send_file_data_impl(const uint8_t *data, uint16_t len);
static int udp_send_file_start_impl(const char *session_id, const char *filename, uint32_t size);
static int udp_send_file_end_impl(const char *filename);
static int udp_send_transfer_done_impl(const char *session_id, uint32_t file_count);
static bool udp_is_connected(void);
static void *udp_get_conn(void);
static void retransmit_handler(struct k_work *work);
static void send_heartbeat(struct k_work *work);
static void update_activity(void);
static int raw_sendto(const void *buf, size_t len);
static int wait_for_control_ack(uint16_t expected_seq);
static uint16_t seq_sub(uint16_t a, uint16_t b);

/* Transport operations (compatible with transport.h) */
static const struct transport_ops udp_ops = {
    .send = udp_send,
    .send_file_data = udp_send_file_data_impl,
    .send_file_start = udp_send_file_start_impl,
    .send_file_end = udp_send_file_end_impl,
    .send_transfer_done = udp_send_transfer_done_impl,
    .is_connected = udp_is_connected,
    .get_conn = udp_get_conn,
};

/* Transport instance */
static struct transport udp_transport = {
    .type = TRANSPORT_TYPE_UDP,
    .ready = false,
    .conn = NULL,
    .ops = &udp_ops,
    .event_cb = NULL,
    .user_data = NULL,
};

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

/** Compute IEEE CRC32 matching binascii.crc32(data) */
static inline uint32_t compute_crc32(const uint8_t *data, size_t len)
{
    return crc32_ieee_update(0, data, len);
}

/** Sequence number arithmetic (handles wrap-around in 16-bit space) */
static inline uint16_t seq_sub(uint16_t a, uint16_t b)
{
    return (a - b) & (UDP_SEQ_MODULO - 1);
}

static void update_activity(void)
{
    last_activity_time = k_uptime_get();
}

/* ========================================================================== */
/* Low-level send                                                              */
/* ========================================================================== */

static int raw_sendto(const void *buf, size_t len)
{
    if (server_sock < 0) {
        return -EBADF;
    }
    if (udp_client_len == 0) {
        return -ENOTCONN;
    }

    k_mutex_lock(&udp_mutex, K_FOREVER);
    int ret = zsock_sendto(server_sock, buf, len, 0,
                           (struct sockaddr *)&udp_client_addr,
                           sizeof(udp_client_addr));
    k_mutex_unlock(&udp_mutex);

    if (ret < 0) {
        LOG_ERR("UDP sendto error: %d", errno);
        return -errno;
    }
    update_activity();
    return ret;
}

/* ========================================================================== */
/* Control frame ACK (stop-and-wait for FILE_START/FILE_END/TRANSFER_DONE)    */
/* ========================================================================== */

/**
 * Wait for ACK that acknowledges up to expected_seq.
 * Does NOT read from the socket — waits for notify_ack() to signal.
 * Called from the transfer thread (not the recv thread).
 */
static int wait_for_control_ack(uint16_t expected_seq)
{
    control_ack_expected = expected_seq;

    int total_ms = 0;
    while (total_ms < ACK_WAIT_TIMEOUT) {
        if (k_sem_take(&control_ack_sem, K_MSEC(500)) == 0) {
            return 0;
        }
        if (!udp_ready) {
            return -ENOTCONN;
        }
        total_ms += 500;
        LOG_WRN("Waiting for control ACK (seq %d, %dms/%dms)",
                expected_seq, total_ms, ACK_WAIT_TIMEOUT);
    }

    LOG_ERR("Control ACK timeout for seq %d", expected_seq);
    return -ETIMEDOUT;
}

/* ========================================================================== */
/* Frame builders                                                              */
/* ========================================================================== */

/** Build a DATA frame in the provided buffer. Returns total frame length. */
static int build_data_frame(uint8_t *buf, uint16_t seq,
                            const uint8_t *data, uint16_t data_len)
{
    if (data_len > UDP_MAX_DATA_PER_FRAME) {
        data_len = UDP_MAX_DATA_PER_FRAME;
    }

    uint32_t crc = compute_crc32(data, data_len);

    buf[0] = UDP_FRAME_DATA;
    buf[1] = seq & 0xFF;
    buf[2] = (seq >> 8) & 0xFF;
    buf[3] = data_len & 0xFF;
    buf[4] = (data_len >> 8) & 0xFF;
    buf[5] = crc & 0xFF;
    buf[6] = (crc >> 8) & 0xFF;
    buf[7] = (crc >> 16) & 0xFF;
    buf[8] = (crc >> 24) & 0xFF;
    memcpy(&buf[UDP_DATA_HEADER_SIZE], data, data_len);

    return UDP_DATA_HEADER_SIZE + data_len;
}

/** Build FILE_START frame. Returns total frame length. */
static int build_file_start_frame(uint8_t *buf, const char *filename, uint32_t size)
{
    uint8_t fn_len = strlen(filename);
    if (fn_len > 63) fn_len = 63;

    buf[0] = UDP_FRAME_FILE_START;
    buf[1] = fn_len;
    memcpy(&buf[2], filename, fn_len);
    buf[2 + fn_len]     = size & 0xFF;
    buf[2 + fn_len + 1] = (size >> 8) & 0xFF;
    buf[2 + fn_len + 2] = (size >> 16) & 0xFF;
    buf[2 + fn_len + 3] = (size >> 24) & 0xFF;

    return 2 + fn_len + 4;
}

/** Build FILE_END frame. Returns total frame length. */
static int build_file_end_frame(uint8_t *buf, uint32_t crc32)
{
    buf[0] = UDP_FRAME_FILE_END;
    buf[1] = crc32 & 0xFF;
    buf[2] = (crc32 >> 8) & 0xFF;
    buf[3] = (crc32 >> 16) & 0xFF;
    buf[4] = (crc32 >> 24) & 0xFF;
    return 5;
}

/** Build TRANSFER_DONE frame. Returns total frame length. */
static int build_transfer_done_frame(uint8_t *buf, const char *session_id, uint32_t file_count)
{
    uint8_t sid_len = strlen(session_id);
    if (sid_len > 63) sid_len = 63;

    buf[0] = UDP_FRAME_TRANSFER_DONE;
    buf[1] = sid_len;
    memcpy(&buf[2], session_id, sid_len);
    buf[2 + sid_len]     = file_count & 0xFF;
    buf[2 + sid_len + 1] = (file_count >> 8) & 0xFF;
    buf[2 + sid_len + 2] = (file_count >> 16) & 0xFF;
    buf[2 + sid_len + 3] = (file_count >> 24) & 0xFF;

    return 2 + sid_len + 4;
}

/* ========================================================================== */
/* Retransmission                                                              */
/* ========================================================================== */

static void retransmit_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();

    if (!udp_ready) {
        return;
    }

    k_mutex_lock(&udp_mutex, K_FOREVER);

    for (int i = 0; i < WINDOW_SIZE; i++) {
        struct frame_slot *slot = &send_window[i];
        if (!slot->valid) {
            continue;
        }

        /* Check if this slot is within the current window */
        uint16_t dist = seq_sub(slot->seq, send_base);
        if (dist >= WINDOW_SIZE) {
            /* Outside window — already ACKed or stale */
            slot->valid = false;
            continue;
        }

        /* Check if frame needs retransmission */
        if (now - slot->last_sent >= RETRANSMIT_TIMEOUT) {
            if (slot->retries >= UDP_MAX_RETRIES) {
                LOG_ERR("Max retries (%d) for seq %d, aborting", UDP_MAX_RETRIES, slot->seq);
                k_mutex_unlock(&udp_mutex);
                /* TODO: signal transfer error to upper layer */
                return;
            }

            slot->retries++;
            slot->last_sent = now;
            LOG_WRN("Retransmit seq %d (retry %d/%d)", slot->seq, slot->retries, UDP_MAX_RETRIES);

            /* Send directly (already holding mutex) */
            zsock_sendto(server_sock, slot->frame, slot->len, 0,
                         (struct sockaddr *)&udp_client_addr,
                         sizeof(udp_client_addr));
        }
    }

    k_mutex_unlock(&udp_mutex);

    /* Schedule next check */
    k_work_schedule(&retransmit_work, K_MSEC(RETRANSMIT_INTERVAL));
}

/* ========================================================================== */
/* Heartbeat                                                                   */
/* ========================================================================== */

static void send_heartbeat(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t hb[UDP_HEARTBEAT_SIZE];
    hb[0] = UDP_FRAME_HEARTBEAT;
    int64_t ts = k_uptime_get();
    memcpy(&hb[1], &ts, 4);

    raw_sendto(hb, sizeof(hb));

    /* Schedule next heartbeat */
    k_timer_start(&heartbeat_timer, K_MSEC(CONFIG_CLIP_UDP_HEARTBEAT_INTERVAL_MS), K_NO_WAIT);
}

/* ========================================================================== */
/* Transport operations                                                        */
/* ========================================================================== */

static int udp_send(const uint8_t *data, uint16_t len)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }
    return raw_sendto(data, len);
}

static int udp_send_file_data_impl(const uint8_t *data, uint16_t len)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    /* Wait for window availability */
    while (seq_sub(next_seq, send_base) >= peer_window || peer_window == 0) {
        if (k_sem_take(&window_sem, K_MSEC(50)) != 0) {
            /* Check if still connected */
            if (!udp_ready) {
                return -ENOTCONN;
            }
            continue;
        }
    }

    uint8_t frame[MAX_FRAME_SIZE];
    uint16_t data_len = len;
    if (data_len > UDP_MAX_DATA_PER_FRAME) {
        data_len = UDP_MAX_DATA_PER_FRAME;
    }

    int frame_len = build_data_frame(frame, next_seq, data, data_len);

    /* Store in send window buffer */
    int slot_idx = next_seq % WINDOW_SIZE;
    struct frame_slot *slot = &send_window[slot_idx];
    slot->seq = next_seq;
    slot->len = frame_len;
    memcpy(slot->frame, frame, frame_len);
    slot->valid = true;
    slot->retries = 0;
    slot->last_sent = k_uptime_get();

    next_seq = (next_seq + 1) & (UDP_SEQ_MODULO - 1);

    /* Send */
    int ret = raw_sendto(frame, frame_len);
    if (ret < 0) {
        slot->valid = false;
        return ret;
    }

    /* Update per-file CRC */
    current_file_crc = crc32_ieee_update(current_file_crc, data, data_len);
    current_bytes_sent += data_len;

    return data_len;
}

static int udp_send_file_start_impl(const char *session_id, const char *filename, uint32_t size)
{
    ARG_UNUSED(session_id);

    if (!udp_ready) {
        return -ENOTCONN;
    }

    /* Reset file state */
    transport_udp_reset_file_state();
    strncpy(current_filename, filename, sizeof(current_filename) - 1);
    current_filename[sizeof(current_filename) - 1] = '\0';
    current_file_size = size;

    /* Build and send frame */
    uint8_t frame[128];
    int frame_len = build_file_start_frame(frame, filename, size);
    int ret = raw_sendto(frame, frame_len);
    if (ret < 0) {
        return ret;
    }

    /* Wait for ACK */
    ret = wait_for_control_ack(send_base);
    return (ret == 0) ? 0 : -1;
}

static int udp_send_file_end_impl(const char *filename)
{
    ARG_UNUSED(filename);

    if (!udp_ready) {
        return -ENOTCONN;
    }

    /* Wait for all outstanding DATA frames to be ACKed before sending FILE_END */
    while (seq_sub(next_seq, send_base) > 0) {
        if (k_sem_take(&window_sem, K_MSEC(100)) != 0) {
            if (!udp_ready) {
                return -ENOTCONN;
            }
            continue;
        }
    }

    /* Finalize CRC — current_file_crc already matches binascii.crc32(full_data)
     * because crc32_ieee_update handles ~crc at both start and end,
     * so accumulation across chunks is correct. */
    uint32_t final_crc = current_file_crc;

    /* Build and send frame */
    uint8_t frame[8];
    int frame_len = build_file_end_frame(frame, final_crc);
    int ret = raw_sendto(frame, frame_len);
    if (ret < 0) {
        return ret;
    }

    ret = wait_for_control_ack(next_seq);
    if (ret == 0) {
        transport_udp_reset_file_state();
    }
    return (ret == 0) ? 0 : -1;
}

static int udp_send_transfer_done_impl(const char *session_id, uint32_t file_count)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    uint8_t frame[128];
    int frame_len = build_transfer_done_frame(frame, session_id, file_count);
    int ret = raw_sendto(frame, frame_len);
    if (ret < 0) {
        return ret;
    }

    ret = wait_for_control_ack(next_seq);
    return (ret == 0) ? 0 : -1;
}

static bool udp_is_connected(void)
{
    if (udp_ready) {
        int64_t elapsed = k_uptime_get() - last_activity_time;
        if (elapsed > CONFIG_CLIP_UDP_CONNECTION_TIMEOUT_MS) {
            LOG_WRN("Connection timeout (%lld ms)", elapsed);
            udp_ready = false;
            udp_transport.ready = false;
        }
    }
    return udp_ready;
}

static void *udp_get_conn(void)
{
    return (void *)&udp_client_addr;
}

/* ========================================================================== */
/* Public API                                                                  */
/* ========================================================================== */

int transport_udp_init(void)
{
    udp_ready = false;
    memset(&udp_client_addr, 0, sizeof(udp_client_addr));
    udp_client_len = 0;
    k_mutex_init(&udp_mutex);

    /* Sliding window */
    memset(send_window, 0, sizeof(send_window));
    send_base = 0;
    next_seq = 0;
    peer_window = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
    k_sem_init(&window_sem, 0, WINDOW_SIZE);
    k_sem_init(&control_ack_sem, 0, 1);
    control_ack_expected = 0;

    /* Retransmit */
    k_work_init_delayable(&retransmit_work, retransmit_handler);

    /* Heartbeat */
    k_timer_init(&heartbeat_timer, NULL, NULL);
    k_work_init(&heartbeat_work, send_heartbeat);
    last_activity_time = k_uptime_get();

    /* File state */
    memset(current_filename, 0, sizeof(current_filename));
    current_file_size = 0;
    current_bytes_sent = 0;
    current_file_crc = 0;

    LOG_INF("UDP transport v2 initialized");
    return 0;
}

void transport_udp_reset_file_state(void)
{
    /* Clear send window */
    for (int i = 0; i < WINDOW_SIZE; i++) {
        send_window[i].valid = false;
    }
    send_base = 0;
    next_seq = 0;
    k_sem_reset(&window_sem);

    /* Reset file state */
    current_filename[0] = '\0';
    current_file_size = 0;
    current_bytes_sent = 0;
    current_file_crc = 0;
}

int transport_udp_send(const uint8_t *data, uint16_t len)
{
    return udp_send(data, len);
}

int transport_udp_send_file_data(const uint8_t *data, uint16_t len)
{
    return udp_send_file_data_impl(data, len);
}

int transport_udp_send_file_start(const char *filename, uint32_t file_size)
{
    return udp_send_file_start_impl(NULL, filename, file_size);
}

int transport_udp_send_file_end(void)
{
    return udp_send_file_end_impl(NULL);
}

int transport_udp_send_transfer_done(const char *session_id, uint32_t file_count)
{
    return udp_send_transfer_done_impl(session_id, file_count);
}

int transport_udp_send_response(const uint8_t *data, uint16_t len)
{
    if (server_sock < 0) {
        return -EBADF;
    }
    if (udp_client_len == 0) {
        return -ENOTCONN;
    }

    /* Build AT_RESP frame: type(1) + len(2) + data */
    uint8_t frame[3 + len];
    frame[0] = UDP_FRAME_AT_RESP;
    frame[1] = len & 0xFF;
    frame[2] = (len >> 8) & 0xFF;
    memcpy(&frame[3], data, len);

    return raw_sendto(frame, sizeof(frame));
}

bool transport_udp_is_active(void)
{
    return udp_ready;
}

void transport_udp_update_active(bool active)
{
    udp_ready = active;
    udp_transport.ready = active;

    if (!active) {
        transport_udp_reset_file_state();
        peer_window = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
        k_work_cancel_delayable(&retransmit_work);
    } else {
        /* Start heartbeat and retransmit when becoming active */
        k_timer_start(&heartbeat_timer, K_MSEC(CONFIG_CLIP_UDP_HEARTBEAT_INTERVAL_MS), K_NO_WAIT);
        k_work_schedule(&retransmit_work, K_MSEC(RETRANSMIT_INTERVAL));
    }
    update_activity();
}

void transport_udp_update_client_addr(const struct sockaddr *addr, socklen_t len)
{
    if (addr && len > 0) {
        k_mutex_lock(&udp_mutex, K_FOREVER);
        memcpy(&udp_client_addr, addr, len < sizeof(udp_client_addr) ? len : sizeof(udp_client_addr));
        udp_client_len = len;
        k_mutex_unlock(&udp_mutex);
        update_activity();
    }
}

void transport_udp_notify_ack(uint16_t ack_seq, uint8_t window, uint8_t bitmap)
{
    LOG_DBG("ACK: seq=%d, window=%d, bitmap=0x%02x", ack_seq, window, bitmap);

    /* Update peer window */
    peer_window = window;
    update_activity();

    /* Signal control frame ACK waiters */
    if (seq_sub(ack_seq, control_ack_expected) < (UDP_SEQ_MODULO / 2)) {
        k_sem_give(&control_ack_sem);
    }

    /* Advance send_base using cumulative ACK */
    if (seq_sub(ack_seq, send_base) > 0) {
        uint16_t old_base = send_base;
        send_base = ack_seq;

        /* Release semaphore for newly available window slots */
        uint16_t freed = seq_sub(send_base, old_base);
        for (uint16_t i = 0; i < freed; i++) {
            k_sem_give(&window_sem);
        }

        /* Invalidate ACKed slots */
        for (int i = 0; i < WINDOW_SIZE; i++) {
            struct frame_slot *slot = &send_window[i];
            if (!slot->valid) {
                continue;
            }
            if (seq_sub(ack_seq, slot->seq) > 0) {
                slot->valid = false;
            }
        }
    }

    /* Process selective ACK bitmap */
    if (bitmap != 0) {
        for (int bit = 0; bit < 8; bit++) {
            if (bitmap & (1 << bit)) {
                uint16_t selective_seq = (ack_seq + bit) & (UDP_SEQ_MODULO - 1);
                int slot_idx = selective_seq % WINDOW_SIZE;
                struct frame_slot *slot = &send_window[slot_idx];
                if (slot->valid && slot->seq == selective_seq) {
                    slot->valid = false;
                    /* If this fills a gap, advance send_base */
                    /* (simplified: bitmap covers only +0 to +7 from ack_seq) */
                }
            }
        }
    }
}

struct transport *transport_udp_get(void)
{
    return &udp_transport;
}
