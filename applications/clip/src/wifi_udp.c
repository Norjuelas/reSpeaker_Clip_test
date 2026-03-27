/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP server for file transfer.
 * Uses stop-and-wait protocol with sequence numbers for reliability.
 *
 * Protocol:
 *   Sender -> Receiver: FILE_START | filename | file_size | session_id
 *   Receiver -> Sender: ACK | seq=0
 *   Sender -> Receiver: FILE_DATA | seq=N | length | data
 *   Receiver -> Sender: ACK | seq=N
 *   ... (repeat until file complete)
 *   Sender -> Receiver: FILE_END | filename
 *   Receiver -> Sender: ACK | seq=MAX
 *   Sender -> Receiver: TRANSFER_DONE | session_id | file_count
 *   Receiver -> Sender: ACK | seq=MAX
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include "wifi_udp.h"
#include "wifi.h"
#include "at_server.h"
#include "transport.h"
#include "transport_udp.h"

LOG_MODULE_REGISTER(wifi_udp, CONFIG_CLIP2_LOG_LEVEL);

/* Configuration */
#define UDP_THREAD_STACK_SIZE  4096
#define UDP_THREAD_PRIORITY    5
#define UDP_RECV_BUF_SIZE      1024
#define UDP_MAX_FRAME_SIZE     512   /* Max data per UDP packet */
#define UDP_ACK_TIMEOUT_MS     500
#define UDP_MAX_RETRIES        5

/* Frame types */
#define FRAME_ACK             0x80
#define FRAME_FILE_START_UDP  0x11
#define FRAME_FILE_DATA_UDP   0x12
#define FRAME_FILE_END_UDP    0x13
#define FRAME_TRANSFER_DONE_UDP 0x14

/* State */
static K_THREAD_STACK_DEFINE(udp_stack, UDP_THREAD_STACK_SIZE);
static struct k_thread udp_thread_data;

static volatile bool server_running;
static volatile bool client_active;
int server_sock = -1;  /* Shared with transport_udp.c */
struct sockaddr_in client_addr;  /* Shared with transport_udp.c */
socklen_t client_len;  /* Shared with transport_udp.c */

/* Static buffers */
static char resp_buf[256];
static uint8_t udp_recv_buf[UDP_RECV_BUF_SIZE];

/* Sequence tracking */
static uint16_t expected_seq;
static int client_socket = -1;

/* Pending ACK state */
static volatile bool awaiting_ack;
static uint8_t pending_frame_type;
static uint16_t pending_seq;
static void *pending_data;
static size_t pending_len;
static int retry_count;

extern struct at_server_context {
    uint8_t transport_type;
} at_ctx;

/**
 * @brief Send ACK packet
 */
static int send_ack(uint16_t seq)
{
    uint8_t ack_frame[5] = {
        FRAME_ACK,
        seq & 0xFF,
        (seq >> 8) & 0xFF,
        0, 0  /* reserved */
    };
    return zsock_sendto(server_sock, ack_frame, sizeof(ack_frame), 0,
                       (struct sockaddr *)&client_addr, sizeof(client_addr));
}

/**
 * @brief Send file frame with retry (stop-and-wait)
 */
static int send_frame_with_ack(uint8_t frame_type, const void *data, size_t len)
{
    int ret;
    struct pollfd pfd;
    int timeout_ms = UDP_ACK_TIMEOUT_MS;

    retry_count = 0;
    awaiting_ack = true;
    pending_frame_type = frame_type;
    pending_seq = expected_seq;
    pending_data = (void *)data;
    pending_len = len;

resend:
    /* Send the frame */
    uint8_t header[4] = {
        frame_type,
        expected_seq & 0xFF,
        (expected_seq >> 8) & 0xFF,
        (uint8_t)(len & 0xFF)
    };

    if (len > 0 && data) {
        /* For FILE_DATA: header + data */
        ret = zsock_sendto(server_sock, header, 4, 0,
                          (struct sockaddr *)&client_addr, sizeof(client_addr));
        if (ret > 0 && ret < 4) {
            /* Partial send, try rest */
            size_t sent = ret;
            const uint8_t *p = data;
            while (sent < len) {
                ret = zsock_sendto(server_sock, p + sent, len - sent, 0,
                                  (struct sockaddr *)&client_addr, sizeof(client_addr));
                if (ret < 0) break;
                sent += ret;
            }
        }
    } else {
        /* For control frames without data payload */
        ret = zsock_sendto(server_sock, header, 4, 0,
                          (struct sockaddr *)&client_addr, sizeof(client_addr));
    }

    if (ret < 0) {
        LOG_ERR("UDP send failed: %d", errno);
        awaiting_ack = false;
        return -1;
    }

    /* Wait for ACK */
    pfd.fd = server_sock;
    pfd.events = ZSOCK_POLLIN;

    while (awaiting_ack && retry_count < UDP_MAX_RETRIES) {
        ret = zsock_poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            LOG_ERR("poll error: %d", errno);
            awaiting_ack = false;
            return -1;
        }
        if (ret == 0) {
            /* Timeout - retransmit */
            retry_count++;
            LOG_WRN("ACK timeout, retry %d/%d", retry_count, UDP_MAX_RETRIES);
            goto resend;
        }

        if (pfd.revents & ZSOCK_POLLIN) {
            uint8_t ack_buf[8];
            ret = zsock_recvfrom(server_sock, ack_buf, sizeof(ack_buf), 0,
                               NULL, NULL);
            if (ret >= 4 && ack_buf[0] == FRAME_ACK) {
                uint16_t ack_seq = ack_buf[1] | (ack_buf[2] << 8);
                if (ack_seq == expected_seq) {
                    awaiting_ack = false;
                    expected_seq++;
                    return 0;
                }
            }
        }
    }

    awaiting_ack = false;
    LOG_ERR("Max retries exceeded");
    return -ETIMEDOUT;
}

/**
 * @brief Handle incoming UDP packet
 */
static void handle_packet(const uint8_t *buf, size_t len)
{
    if (len < 4) {
        return;
    }

    uint8_t frame_type = buf[0];
    uint16_t seq = buf[1] | (buf[2] << 8);

    switch (frame_type) {
    case FRAME_ACK:
        /* ACK from receiver - not used in current protocol */
        awaiting_ack = false;
        break;

    case FRAME_FILE_START_UDP:
        /* File transfer start */
        {
            uint8_t fn_len = buf[3];
            if (len < 4 + fn_len + 4) {
                LOG_ERR("Invalid FILE_START frame");
                send_ack(expected_seq);
                break;
            }
            char filename[64] = {0};
            memcpy(filename, &buf[4], fn_len);
            uint32_t file_size = buf[4 + fn_len] | (buf[5 + fn_len] << 8) |
                                (buf[6 + fn_len] << 16) | (buf[7 + fn_len] << 24);

            /* File transfer start received */
            send_ack(expected_seq);

            /* Notify transport layer */
            transport_udp_notify_file_start(filename, file_size);
        }
        break;

    case FRAME_FILE_DATA_UDP:
        /* File data packet */
        {
            uint16_t data_len = buf[3] | (buf[4] << 8);
            if (len < 5 + data_len) {
                LOG_ERR("Invalid FILE_DATA frame");
                send_ack(seq);
                break;
            }
            if (seq == expected_seq) {
                transport_udp_notify_data(&buf[5], data_len);
                expected_seq++;
            }
            send_ack(seq);
        }
        break;

    case FRAME_FILE_END_UDP:
        /* File transfer end */
        /* File transfer end received */
        send_ack(expected_seq);
        transport_udp_notify_file_end();
        break;

    case FRAME_TRANSFER_DONE_UDP:
        /* All files transferred */
        /* Transfer complete received */
        send_ack(expected_seq);
        transport_udp_notify_transfer_done();
        break;

    default:
        /* Check if it's an AT command (text format) */
        if (frame_type == 'A' || frame_type == 'a') {
            /* AT command */
            char *line = (char *)buf;
            line[len] = '\0';
            at_server_submit_cmd((uint8_t *)line, len, TRANSPORT_TYPE_UDP);
        }
        break;
    }
}

/**
 * @brief UDP server thread
 */
static void udp_server_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(WIFI_AP_UDP_PORT),
        .sin_addr   = { .s_addr = INADDR_ANY },
    };

    server_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_sock < 0) {
        LOG_ERR("socket() failed: %d", errno);
        server_running = false;
        return;
    }

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    zsock_setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (zsock_bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind() failed: %d", errno);
        zsock_close(server_sock);
        server_sock = -1;
        server_running = false;
        return;
    }

    LOG_INF("UDP server listening on port %d (sock=%d)", WIFI_AP_UDP_PORT, server_sock);

    while (server_running) {
        /* Blocking recv with 1 second timeout */
        memset(udp_recv_buf, 0, sizeof(udp_recv_buf));
        client_len = sizeof(client_addr);
        int ret = zsock_recvfrom(server_sock, udp_recv_buf, sizeof(udp_recv_buf) - 1,
                               0, (struct sockaddr *)&client_addr, &client_len);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout - continue loop */
                continue;
            }
            LOG_ERR("UDP recvfrom error: %d", errno);
            k_sleep(K_MSEC(100));
            continue;
        }

        if (ret > 0) {
            /* Update transport with client address for responses */
            if (client_len > 0) {
                transport_udp_update_client_addr((struct sockaddr *)&client_addr, client_len);
                transport_udp_update_active(true);
            }
            handle_packet(udp_recv_buf, ret);
        }
    }

    if (server_sock >= 0) {
        zsock_close(server_sock);
        server_sock = -1;
    }

    server_running = false;
    LOG_INF("UDP server stopped");
}

/* Public API */

int wifi_udp_init(void)
{
    server_running = false;
    client_active = false;
    server_sock = -1;
    client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));
    expected_seq = 0;
    awaiting_ack = false;
    retry_count = 0;
    return 0;
}

int wifi_udp_start(void)
{
    if (server_running) {
        LOG_DBG("UDP server already running");
        return 0;
    }

    server_running = true;
    expected_seq = 0;

    k_thread_create(&udp_thread_data, udp_stack,
                    K_THREAD_STACK_SIZEOF(udp_stack),
                    udp_server_thread,
                    NULL, NULL, NULL,
                    UDP_THREAD_PRIORITY, 0, K_NO_WAIT);

    k_thread_name_set(&udp_thread_data, "wifi_udp");

    LOG_INF("UDP server starting");
    return 0;
}

void wifi_udp_stop(void)
{
    if (!server_running) {
        return;
    }

    server_running = false;
    client_active = false;

    if (server_sock >= 0) {
        zsock_close(server_sock);
        server_sock = -1;
    }

    k_sleep(K_MSEC(100));
    LOG_INF("UDP server stopped");
}

bool wifi_udp_is_running(void)
{
    return server_running;
}

bool wifi_udp_is_active(void)
{
    return client_active;
}