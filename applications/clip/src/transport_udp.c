/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP transport for file transfer and AT command responses.
 * Uses stop-and-wait protocol with sequence numbers for reliability.
 * Includes flow control (sliding window) and CRC32 integrity checking.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/sys/crc.h>
#include "transport.h"
#include "transport_udp.h"
#include "wifi_udp.h"

LOG_MODULE_REGISTER(transport_udp, CONFIG_CLIP_LOG_LEVEL);

/* External variables from wifi_udp (shared socket) */
extern int server_sock;

/* State */
static struct k_mutex udp_mutex;
static volatile bool udp_ready;
static struct sockaddr_in udp_client_addr;
static socklen_t udp_client_len;

/* Flow control state */
static uint16_t receive_window_size = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
static uint16_t unacknowledged_frames = 0;

/* Heartbeat state */
static int64_t last_activity_time;
static struct k_timer heartbeat_timer;
static struct k_work heartbeat_work;

/* Transfer state */
static char current_filename[64];
static uint32_t current_file_size;
static uint32_t current_bytes_sent;
static uint32_t current_crc32;

/* Forward declarations */
static int udp_send(const uint8_t *data, uint16_t len);
static int udp_send_file_data(const uint8_t *data, uint16_t len);
static int udp_send_file_start(const char *session_id, const char *filename, uint32_t size);
static int udp_send_file_end(const char *filename);
static int udp_send_transfer_done(const char *session_id, uint32_t file_count);
static bool udp_is_connected(void);
static void *udp_get_conn(void);
static void heartbeat_timer_init(void);
static void send_heartbeat(struct k_work *work);
static void update_activity(void);

/* Transport operations */
static const struct transport_ops udp_ops = {
    .send = udp_send,
    .send_file_data = udp_send_file_data,
    .send_file_start = udp_send_file_start,
    .send_file_end = udp_send_file_end,
    .send_transfer_done = udp_send_transfer_done,
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

/**
 * @brief Update activity timestamp (call on any TX/RX activity)
 */
static void update_activity(void)
{
    last_activity_time = k_uptime_get();
}

/**
 * @brief Send heartbeat frame
 */
static void send_heartbeat(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t heartbeat_frame[7] = {
        FRAME_HEARTBEAT,
        0x00, 0x00,  /* reserved */
        0x00, 0x00, 0x00, 0x00  /* timestamp placeholder */
    };

    if (server_sock >= 0 && udp_client_len > 0) {
        int ret = zsock_sendto(server_sock, heartbeat_frame, sizeof(heartbeat_frame), 0,
                              (struct sockaddr *)&udp_client_addr, sizeof(udp_client_addr));
        if (ret < 0) {
            LOG_DBG("Heartbeat send failed: %d", errno);
        }
    }

    /* Schedule next heartbeat */
    k_timer_start(&heartbeat_timer, K_MSEC(CONFIG_CLIP_UDP_HEARTBEAT_INTERVAL_MS), K_NO_WAIT);
}

/**
 * @brief Initialize heartbeat timer
 */
static void heartbeat_timer_init(void)
{
    k_timer_init(&heartbeat_timer, NULL, NULL);
    k_work_init(&heartbeat_work, send_heartbeat);
    last_activity_time = k_uptime_get();
}

/**
 * @brief Send UDP packet with mutex protection
 */
static int udp_sendto(const void *buf, size_t len)
{
    int ret;

    if (server_sock < 0) {
        LOG_ERR("UDP sendto: invalid sock=%d", server_sock);
        return -EBADF;
    }
    if (udp_client_len == 0) {
        LOG_ERR("UDP sendto: no client addr, client_len=%d", udp_client_len);
        return -ENOTCONN;
    }

    k_mutex_lock(&udp_mutex, K_FOREVER);
    ret = zsock_sendto(server_sock, buf, len, 0,
                      (struct sockaddr *)&udp_client_addr, sizeof(udp_client_addr));
    if (ret < 0) {
        LOG_ERR("UDP sendto error: %d (sock=%d, len=%d)", errno, server_sock, len);
    }
    k_mutex_unlock(&udp_mutex);

    update_activity();
    return ret;
}

/**
 * @brief Wait for ACK with sequence number
 */
static int wait_for_ack(uint16_t expected_seq)
{
    struct pollfd pfd;
    uint8_t ack_buf[8];
    int ret;
    int timeout_ms = CONFIG_CLIP_UDP_ACK_TIMEOUT_MS;
    int retries = 0;

    if (server_sock < 0) {
        return -ENOTCONN;
    }

    pfd.fd = server_sock;
    pfd.events = ZSOCK_POLLIN;

    while (retries < CONFIG_CLIP_UDP_MAX_RETRIES) {
        ret = zsock_poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERR("poll error: %d", errno);
            return -1;
        }
        if (ret == 0) {
            retries++;
            LOG_WRN("ACK timeout, retry %d/%d", retries, CONFIG_CLIP_UDP_MAX_RETRIES);
            continue;
        }

        if (pfd.revents & ZSOCK_POLLIN) {
            k_mutex_lock(&udp_mutex, K_FOREVER);
            ret = zsock_recvfrom(server_sock, ack_buf, sizeof(ack_buf), 0, NULL, NULL);
            k_mutex_unlock(&udp_mutex);
            if (ret >= 4) {
                /* Check for window ack */
                if (ack_buf[0] == FRAME_WINDOW_ACK) {
                    uint16_t window = ack_buf[1] | (ack_buf[2] << 8);
                    receive_window_size = window;
                    LOG_DBG("Window update: %d", window);
                    update_activity();
                    continue;  /* Continue waiting for our ACK */
                }
                if (ack_buf[0] == FRAME_ACK) {
                    uint16_t ack_seq = ack_buf[1] | (ack_buf[2] << 8);
                    if (ack_seq == expected_seq) {
                        if (unacknowledged_frames > 0) {
                            unacknowledged_frames--;
                        }
                        update_activity();
                        return 0;
                    }
                }
            }
        }
    }
    return -ETIMEDOUT;
}

/**
 * @brief Send frame and wait for ACK (stop-and-wait)
 */
static int send_with_ack(uint8_t frame_type, uint16_t seq, const void *data, size_t len)
{
    uint8_t header[4] = {
        frame_type,
        seq & 0xFF,
        (seq >> 8) & 0xFF,
        (uint8_t)(len & 0xFF)
    };
    int ret;

    /* Check flow control - wait if window is full */
    while (unacknowledged_frames >= receive_window_size) {
        LOG_DBG("Window full (%d/%d), waiting...", unacknowledged_frames, receive_window_size);
        k_sleep(K_MSEC(100));
    }

    /* Send header */
    ret = udp_sendto(header, sizeof(header));
    if (ret < 0) {
        return ret;
    }

    /* Send data if present */
    if (len > 0 && data) {
        ret = udp_sendto(data, len);
        if (ret < 0) {
            return ret;
        }
    }

    unacknowledged_frames++;
    /* Wait for ACK */
    return wait_for_ack(seq);
}

/* Transport Operations */

static int udp_send(const uint8_t *data, uint16_t len)
{
    if (!udp_ready && udp_client_len == 0) {
        return -ENOTCONN;
    }
    return udp_sendto(data, len);
}

static int udp_send_file_data(const uint8_t *data, uint16_t len)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    /* Check flow control */
    if (unacknowledged_frames >= receive_window_size) {
        LOG_WRN("Window full, cannot send data");
        return -ENOTCONN;
    }

    /* Build frame: type(1) + seq(2) + length(2) + data */
    uint8_t frame[5 + CONFIG_CLIP_UDP_MAX_DATA_SIZE];
    if (len > CONFIG_CLIP_UDP_MAX_DATA_SIZE) {
        len = CONFIG_CLIP_UDP_MAX_DATA_SIZE;
    }

    frame[0] = FRAME_FILE_DATA_UDP;
    frame[1] = 0;  /* seq low - not used */
    frame[2] = 0;  /* seq high byte */
    frame[3] = len & 0xFF;        /* length low byte */
    frame[4] = (len >> 8) & 0xFF; /* length high byte */
    memcpy(&frame[5], data, len);

    int ret = udp_sendto(frame, 5 + len);
    if (ret < 0) {
        return ret;
    }

    /* Update CRC32 */
    current_crc32 = crc32_ieee_update(current_crc32, data, len);

    current_bytes_sent += len;
    return len;
}

static int udp_send_file_start(const char *session_id, const char *filename, uint32_t size)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    ARG_UNUSED(session_id);
    strncpy(current_filename, filename, sizeof(current_filename) - 1);
    current_filename[sizeof(current_filename) - 1] = '\0';
    current_file_size = size;
    current_bytes_sent = 0;
    current_crc32 = 0;  /* Reset CRC */

    /* Build frame: type(1) + seq(2) + fn_len(1) + filename + size(4) */
    uint8_t frame[128];
    uint8_t fn_len = strlen(filename);
    if (fn_len > 31) fn_len = 31;

    frame[0] = FRAME_FILE_START_UDP;
    frame[1] = 0;  /* seq low */
    frame[2] = 0;  /* seq high */
    frame[3] = fn_len;
    memcpy(&frame[4], filename, fn_len);
    frame[4 + fn_len] = size & 0xFF;
    frame[5 + fn_len] = (size >> 8) & 0xFF;
    frame[6 + fn_len] = (size >> 16) & 0xFF;
    frame[7 + fn_len] = (size >> 24) & 0xFF;

    /* Wait for ACK - reliable control frame */
    int ret = send_with_ack(FRAME_FILE_START_UDP, 0, frame + 4, 4 + fn_len);
    return (ret == 0) ? 0 : -1;
}

static int udp_send_file_end(const char *filename)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    ARG_UNUSED(filename);

    /* Build frame: type(1) + seq(2) + fn_len(1) + filename + crc32(4) */
    uint8_t frame[64];
    uint8_t fn_len = strlen(current_filename);
    if (fn_len > 31) fn_len = 31;

    frame[0] = FRAME_FILE_END_UDP;
    frame[1] = 0xFF;  /* seq low - use 0xFFFF for end */
    frame[2] = 0xFF;  /* seq high */
    frame[3] = fn_len;
    memcpy(&frame[4], current_filename, fn_len);
    /* Append CRC32 at end */
    frame[4 + fn_len] = current_crc32 & 0xFF;
    frame[5 + fn_len] = (current_crc32 >> 8) & 0xFF;
    frame[6 + fn_len] = (current_crc32 >> 16) & 0xFF;
    frame[7 + fn_len] = (current_crc32 >> 24) & 0xFF;

    /* Wait for ACK and CRC result */
    int ret = send_with_ack(FRAME_FILE_END_UDP, 0xFFFF, frame + 4, 4 + fn_len + 4);
    if (ret == 0) {
        current_filename[0] = '\0';
        current_file_size = 0;
        current_bytes_sent = 0;
        current_crc32 = 0;
    }
    return (ret == 0) ? 0 : -1;
}

static int udp_send_transfer_done(const char *session_id, uint32_t file_count)
{
    if (!udp_ready) {
        return -ENOTCONN;
    }

    /* Build frame: type(1) + seq(2) + sid_len(1) + session_id + file_count(4) */
    uint8_t frame[64];
    uint8_t sid_len = strlen(session_id);
    if (sid_len > 31) sid_len = 31;

    frame[0] = FRAME_TRANSFER_DONE_UDP;
    frame[1] = 0xFF;  /* seq low */
    frame[2] = 0xFF;  /* seq high */
    frame[3] = sid_len;
    memcpy(&frame[4], session_id, sid_len);
    frame[4 + sid_len] = file_count & 0xFF;
    frame[5 + sid_len] = (file_count >> 8) & 0xFF;
    frame[6 + sid_len] = (file_count >> 16) & 0xFF;
    frame[7 + sid_len] = (file_count >> 24) & 0xFF;

    /* Wait for ACK - reliable control frame */
    int ret = send_with_ack(FRAME_TRANSFER_DONE_UDP, 0xFFFF, frame + 4, 4 + sid_len);
    return (ret == 0) ? 0 : -1;
}

static bool udp_is_connected(void)
{
    /* Check connection timeout */
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

/* Public API */

int transport_udp_init(void)
{
    udp_ready = false;
    memset(&udp_client_addr, 0, sizeof(udp_client_addr));
    udp_client_len = 0;
    current_filename[0] = '\0';
    current_file_size = 0;
    current_bytes_sent = 0;
    current_crc32 = 0;
    receive_window_size = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
    unacknowledged_frames = 0;
    k_mutex_init(&udp_mutex);
    heartbeat_timer_init();

    /* UDP transport initialized */
    return 0;
}

int transport_udp_send(const uint8_t *data, uint16_t len)
{
    return udp_send(data, len);
}

int transport_udp_send_file_data(const uint8_t *data, uint16_t len)
{
    return udp_send_file_data(data, len);
}

int transport_udp_send_file_start(const char *filename, uint32_t file_size)
{
    return udp_send_file_start(NULL, filename, file_size);
}

int transport_udp_send_file_end(const char *filename)
{
    return udp_send_file_end(filename);
}

int transport_udp_send_transfer_done(const char *session_id, uint32_t file_count)
{
    return udp_send_transfer_done(session_id, file_count);
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
        current_filename[0] = '\0';
        current_file_size = 0;
        current_bytes_sent = 0;
        current_crc32 = 0;
        unacknowledged_frames = 0;
        receive_window_size = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
    } else {
        /* Start heartbeat when becoming active */
        k_timer_start(&heartbeat_timer, K_MSEC(CONFIG_CLIP_UDP_HEARTBEAT_INTERVAL_MS), K_NO_WAIT);
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

/* Notification callbacks from wifi_udp (called in wifi_udp thread context) */
void transport_udp_notify_file_start(const char *filename, uint32_t file_size)
{
    /* File start received */
    ARG_UNUSED(filename);
    ARG_UNUSED(file_size);
}

void transport_udp_notify_data(const uint8_t *data, size_t len)
{
    /* Not used for file reception in this implementation */
    ARG_UNUSED(data);
    ARG_UNUSED(len);
}

void transport_udp_notify_file_end(void)
{
    /* File end received */
}

void transport_udp_notify_transfer_done(void)
{
    /* Transfer done received */
}

void transport_udp_notify_window_ack(uint16_t window_size)
{
    receive_window_size = window_size;
    LOG_DBG("Window size updated: %d", window_size);
}

void transport_udp_notify_crc_result(uint32_t crc, uint8_t status)
{
    if (status == 0) {
        LOG_INF("File CRC verified: 0x%08x", crc);
    } else {
        LOG_ERR("File CRC mismatch: expected 0x%08x", crc);
    }
}

/**
 * @brief Send AT command response with FRAME_AT_RESPONSE framing
 *
 * This wraps the response in a FRAME_AT_RESPONSE frame so the client
 * can distinguish it from file transfer frames.
 *
 * @param data Response data
 * @param len Response length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send_response(const uint8_t *data, uint16_t len)
{
    if (server_sock < 0) {
        return -EBADF;
    }
    if (udp_client_len == 0) {
        return -ENOTCONN;
    }

    /* Build framed response: FRAME_AT_RESPONSE | len(2) | data */
    uint8_t frame[3 + len];
    frame[0] = FRAME_AT_RESPONSE;
    frame[1] = len & 0xFF;
    frame[2] = (len >> 8) & 0xFF;
    memcpy(&frame[3], data, len);

    k_mutex_lock(&udp_mutex, K_FOREVER);
    int ret = zsock_sendto(server_sock, frame, sizeof(frame), 0,
                          (struct sockaddr *)&udp_client_addr, sizeof(udp_client_addr));
    k_mutex_unlock(&udp_mutex);

    update_activity();
    return ret;
}

/* Get transport pointer for registration */
struct transport *transport_udp_get(void)
{
    return &udp_transport;
}
