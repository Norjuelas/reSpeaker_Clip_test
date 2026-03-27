/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_TRANSPORT_UDP_H
#define CLIP_TRANSPORT_UDP_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

/**
 * @brief CLIP UDP Transfer Protocol v2 Frame Types
 *
 * Efficient sliding-window protocol with per-frame CRC32,
 * selective ACK, and flow control for reliable UDP file transfer.
 */
#define UDP_FRAME_DATA          0x01  /* Server→Client: file data with seq + CRC */
#define UDP_FRAME_ACK           0x02  /* Client→Server: cumulative ACK + bitmap + window */
#define UDP_FRAME_FILE_START    0x10  /* Server→Client: begin file transfer */
#define UDP_FRAME_FILE_END      0x11  /* Server→Client: end file (full-file CRC32) */
#define UDP_FRAME_TRANSFER_DONE 0x12  /* Server→Client: all files complete */
#define UDP_FRAME_AT_RESP       0x20  /* Server→Client: AT command response (JSON) */
#define UDP_FRAME_HEARTBEAT     0x30  /* Bidirectional: keepalive */

/**
 * @brief Frame header sizes
 */
#define UDP_DATA_HEADER_SIZE    9   /* type(1) + seq(2) + len(2) + crc32(4) */
#define UDP_ACK_FRAME_SIZE      5   /* type(1) + ack_seq(2) + window(1) + bitmap(1) */
#define UDP_HEARTBEAT_SIZE      5   /* type(1) + timestamp(4) */

/**
 * @brief Protocol limits
 */
#define UDP_MAX_DATA_PER_FRAME  486  /* Max data payload (512 - 9 header - 17 UDP/IP overhead) */
#define UDP_SEQ_MODULO          4096 /* Sequence number space (12-bit effective) */
#define UDP_MAX_RETRIES         5    /* Max retransmissions per frame before abort */

/**
 * @brief DATA frame format (Server→Client)
 *
 * [type: 1][seq_lo: 1][seq_hi: 1][len_lo: 1][len_hi: 1][crc32: 4][data: len]
 *
 * - type: UDP_FRAME_DATA (0x01)
 * - seq: uint16 LE, per-file sequence number
 * - len: uint16 LE, data length (0 to UDP_MAX_DATA_PER_FRAME)
 * - crc32: IEEE CRC32 of data field only (initial 0xFFFFFFFF)
 * - data: raw file data
 */

/**
 * @brief ACK frame format (Client→Server)
 *
 * [type: 1][ack_seq_lo: 1][ack_seq_hi: 1][window: 1][bitmap: 1]
 *
 * - type: UDP_FRAME_ACK (0x02)
 * - ack_seq: uint16 LE, cumulative ACK (all seq < ack_seq received)
 * - window: uint8, available receive window in frames (0 = pause)
 * - bitmap: uint8, bit i = 1 → frame (ack_seq + i) received
 */

/**
 * @brief FILE_START frame format (Server→Client)
 *
 * [type: 1][fn_len: 1][filename: fn_len][file_size: 4 bytes LE]
 */

/**
 * @brief FILE_END frame format (Server→Client)
 *
 * [type: 1][crc32: 4 bytes LE]
 *
 * CRC32 of complete file data (IEEE, initial 0xFFFFFFFF).
 * Client independently computes and compares.
 */

/**
 * @brief TRANSFER_DONE frame format (Server→Client)
 *
 * [type: 1][sid_len: 1][session_id: sid_len][file_count: 4 bytes LE]
 */

/**
 * @brief AT_RESP frame format (Server→Client)
 *
 * [type: 1][len_lo: 1][len_hi: 1][json_data: len]
 */

/**
 * @brief HEARTBEAT frame format (Bidirectional)
 *
 * [type: 1][timestamp: 4 bytes LE]
 */

/* ---- Public API ---- */

/**
 * @brief Initialize UDP transport
 *
 * @return 0 on success, negative error code on failure
 */
int transport_udp_init(void);

/**
 * @brief Send data through UDP (for AT command responses)
 *
 * @param data Data to send
 * @param len Data length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send(const uint8_t *data, uint16_t len);

/**
 * @brief Send file data through UDP (with sliding window + CRC)
 *
 * Blocks if send window is full. Automatically handles retransmission.
 *
 * @param data Data to send
 * @param len Data length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send_file_data(const uint8_t *data, uint16_t len);

/**
 * @brief Send file start frame (reliable, stop-and-wait)
 *
 * @param filename Filename
 * @param file_size File size in bytes
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_file_start(const char *filename, uint32_t file_size);

/**
 * @brief Send file end frame with full-file CRC32 (reliable, stop-and-wait)
 *
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_file_end(void);

/**
 * @brief Send transfer done frame (reliable, stop-and-wait)
 *
 * @param session_id Session ID
 * @param file_count Number of files transferred
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_transfer_done(const char *session_id, uint32_t file_count);

/**
 * @brief Send AT command response with FRAME_AT_RESP framing
 *
 * @param data Response data
 * @param len Response length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send_response(const uint8_t *data, uint16_t len);

/**
 * @brief Check if UDP transport is active
 */
bool transport_udp_is_active(void);

/**
 * @brief Update UDP connection state (called from wifi_udp)
 */
void transport_udp_update_active(bool active);

/**
 * @brief Update client address for responses (called from wifi_udp)
 */
void transport_udp_update_client_addr(const struct sockaddr *addr, socklen_t len);

/**
 * @brief Notify ACK received (called from wifi_udp server thread)
 *
 * Updates sliding window, releases flow control semaphore,
 * and triggers retransmission if needed.
 *
 * @param ack_seq Cumulative ACK sequence number
 * @param window Available receive window size (frames)
 * @param bitmap Selective ACK bitmap (8 bits)
 */
void transport_udp_notify_ack(uint16_t ack_seq, uint8_t window, uint8_t bitmap);

/**
 * @brief Get UDP transport pointer for registration
 */
struct transport *transport_udp_get(void);

/**
 * @brief Reset per-file transfer state (call at start of each file)
 */
void transport_udp_reset_file_state(void);

#endif /* CLIP_TRANSPORT_UDP_H */
