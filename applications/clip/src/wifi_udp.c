/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP server for file transfer.
 * Uses stop-and-wait protocol with sequence numbers for reliability.
 * Includes flow control (sliding window) and CRC32 integrity checking.
 *
 * Protocol:
 *   Sender -> Receiver: FILE_START | filename | file_size
 *   Receiver -> Sender: ACK | seq=0
 *   Sender -> Receiver: FILE_DATA | seq=N | length | data
 *   Receiver -> Sender: ACK | seq=N
 *   ... (repeat until file complete)
 *   Sender -> Receiver: FILE_END | filename | crc32(4)
 *   Receiver -> Sender: FILE_CRC | crc32 | status
 *   Sender -> Receiver: TRANSFER_DONE | session_id | file_count
 *   Receiver -> Sender: ACK | seq=MAX
 *
 * Flow Control:
 *   Receiver -> Sender: WINDOW_ACK | window_size
 *   Sender adjusts sending based on window size
 *
 * Heartbeat:
 *   Either side can send: HEARTBEAT | timestamp
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <zephyr/sys/crc.h>
#include "wifi_udp.h"
#include "wifi.h"
#include "at_server.h"
#include "transport.h"
#include "transport_udp.h"

LOG_MODULE_REGISTER(wifi_udp, CONFIG_CLIP_LOG_LEVEL);

/* State */
static K_THREAD_STACK_DEFINE(udp_stack, CONFIG_CLIP_UDP_THREAD_STACK_SIZE);
static struct k_thread udp_thread_data;

static volatile bool server_running;
static volatile bool client_active;
int server_sock = -1;  /* Shared with transport_udp.c */
struct sockaddr_in client_addr;  /* Shared with transport_udp.c */
socklen_t client_len;  /* Shared with transport_udp.c */

/* Static buffers */
static uint8_t udp_recv_buf[CONFIG_CLIP_UDP_RECV_BUF_SIZE];

/* Sequence tracking */
static uint16_t expected_seq;

/* Flow control state */
static uint16_t receive_window_size = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
static struct k_timer window_ack_timer;
static struct k_work window_ack_work;

/* CRC state for receiving */
static uint32_t receive_crc32;
static bool receiving_file = false;

/* Forward declarations */
static void send_window_ack(struct k_work *work);
static void window_ack_timer_init(void);

/**
 * @brief Initialize window ACK timer
 */
static void window_ack_timer_init(void)
{
    k_timer_init(&window_ack_timer, NULL, NULL);
    k_work_init(&window_ack_work, send_window_ack);
}

/**
 * @brief Send window ACK frame
 */
static void send_window_ack(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t ack_frame[5] = {
        FRAME_WINDOW_ACK,
        receive_window_size & 0xFF,
        (receive_window_size >> 8) & 0xFF,
        0, 0  /* reserved */
    };

    if (server_sock >= 0 && client_len > 0) {
        int ret = zsock_sendto(server_sock, ack_frame, sizeof(ack_frame), 0,
                              (struct sockaddr *)&client_addr, sizeof(client_addr));
        if (ret < 0) {
            LOG_DBG("Window ACK send failed: %d", errno);
        } else {
            LOG_DBG("Window ACK sent: %d", receive_window_size);
        }
    }

    /* Schedule next window ACK */
    k_timer_start(&window_ack_timer, K_MSEC(CONFIG_CLIP_UDP_WINDOW_ACK_INTERVAL_MS), K_NO_WAIT);
}

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
 * @brief Send CRC result packet
 */
static int send_crc_result(uint32_t crc, uint8_t status)
{
    uint8_t crc_frame[6] = {
        FRAME_FILE_CRC,
        crc & 0xFF,
        (crc >> 8) & 0xFF,
        (crc >> 16) & 0xFF,
        (crc >> 24) & 0xFF,
        status  /* 0=OK, 1=Error */
    };

    int ret = zsock_sendto(server_sock, crc_frame, sizeof(crc_frame), 0,
                          (struct sockaddr *)&client_addr, sizeof(client_addr));

    /* Notify transport layer */
    transport_udp_notify_crc_result(crc, status);

    return ret;
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
    case FRAME_HEARTBEAT:
        /* Heartbeat frame - just acknowledge receipt */
        LOG_DBG("Heartbeat received");
        /* Update client active state */
        client_active = true;
        break;

    case FRAME_ACK:
        /* ACK from sender - handled in transport_udp */
        break;

    case FRAME_WINDOW_ACK:
        /* Window ACK from receiver - handled in transport_udp */
        if (len >= 4) {
            uint16_t window = buf[1] | (buf[2] << 8);
            transport_udp_notify_window_ack(window);
        }
        break;

    case FRAME_FILE_CRC:
        /* CRC result from receiver - handled in transport_udp */
        if (len >= 6) {
            uint32_t crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
            uint8_t status = buf[5];
            transport_udp_notify_crc_result(crc, status);
        }
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

            /* Reset CRC for new file */
            receive_crc32 = 0;
            receiving_file = true;

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
                /* Update CRC */
                if (receiving_file) {
                    receive_crc32 = crc32_ieee_update(receive_crc32, &buf[5], data_len);
                }
                transport_udp_notify_data(&buf[5], data_len);
                expected_seq++;
            }
            send_ack(seq);
        }
        break;

    case FRAME_FILE_END_UDP:
        /* File transfer end */
        /* Format: type(1) + seq(2) + fn_len(1) + filename(fn_len) + crc32(4) */
        {
            uint8_t fn_len = buf[3];
            uint32_t sent_crc = 0;

            if (len >= 4 + fn_len + 4) {
                sent_crc = buf[4 + fn_len] | (buf[5 + fn_len] << 8) |
                          (buf[6 + fn_len] << 16) | (buf[7 + fn_len] << 24);
            }

            receiving_file = false;

            /* Verify CRC */
            uint8_t status = 0;
            if (receive_crc32 != sent_crc) {
                LOG_ERR("CRC mismatch: expected 0x%08x, got 0x%08x", sent_crc, receive_crc32);
                status = 1;  /* Error */
            }

            /* Send CRC result */
            send_crc_result(receive_crc32, status);

            /* Send ACK */
            send_ack(expected_seq);
            transport_udp_notify_file_end();

            /* Reset for next file */
            expected_seq = 0;
            receive_crc32 = 0;
        }
        break;

    case FRAME_TRANSFER_DONE_UDP:
        /* All files transferred */
        send_ack(0xFFFF);
        transport_udp_notify_transfer_done();
        break;

    case FRAME_AT_RESPONSE:
        /* AT command response from server - just log for debug */
        LOG_DBG("AT response received: %d bytes", len);
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

    /* Initialize window ACK timer */
    window_ack_timer_init();
    k_timer_start(&window_ack_timer, K_MSEC(CONFIG_CLIP_UDP_WINDOW_ACK_INTERVAL_MS), K_NO_WAIT);

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
                client_active = true;
            }
            handle_packet(udp_recv_buf, ret);
        }
    }

    /* Stop window ACK timer */
    k_timer_stop(&window_ack_timer);

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
    receive_crc32 = 0;
    receiving_file = false;
    receive_window_size = CONFIG_CLIP_UDP_INITIAL_WINDOW_SIZE;
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
                    CONFIG_CLIP_UDP_THREAD_PRIORITY, 0, K_NO_WAIT);

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
