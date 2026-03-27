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
#include <zephyr/drivers/mbox.h>

/**
 * @brief Frame types for UDP protocol
 */
#define FRAME_ACK              0x80
#define FRAME_FILE_START_UDP   0x11
#define FRAME_FILE_DATA_UDP    0x12
#define FRAME_FILE_END_UDP     0x13
#define FRAME_TRANSFER_DONE_UDP 0x14
#define FRAME_WINDOW_ACK       0x15
#define FRAME_FILE_CRC         0x16
#define FRAME_AT_RESPONSE      0x17
#define FRAME_HEARTBEAT        0xFF

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
 * @brief Send file data through UDP
 *
 * @param data Data to send
 * @param len Data length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send_file_data(const uint8_t *data, uint16_t len);

/**
 * @brief Send file start frame
 *
 * @param filename Filename
 * @param file_size File size in bytes
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_file_start(const char *filename, uint32_t file_size);

/**
 * @brief Send file end frame
 *
 * @param filename Filename
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_file_end(const char *filename);

/**
 * @brief Send transfer done frame
 *
 * @param session_id Session ID
 * @param file_count Number of files transferred
 * @return 0 on success, negative error code on failure
 */
int transport_udp_send_transfer_done(const char *session_id, uint32_t file_count);

/**
 * @brief Check if UDP transport is active
 *
 * @return true if active, false otherwise
 */
bool transport_udp_is_active(void);

/**
 * @brief Update UDP connection state (called from wifi_udp)
 *
 * @param active true if client is active
 */
void transport_udp_update_active(bool active);

/**
 * @brief Update client address for responses (called from wifi_udp)
 *
 * @param addr Client address structure
 * @param len Length of address
 */
void transport_udp_update_client_addr(const struct sockaddr *addr, socklen_t len);

/**
 * @brief Notify file start (called from wifi_udp server thread)
 *
 * @param filename Filename
 * @param file_size File size
 */
void transport_udp_notify_file_start(const char *filename, uint32_t file_size);

/**
 * @brief Notify data received (called from wifi_udp server thread)
 *
 * @param data Data buffer
 * @param len Data length
 */
void transport_udp_notify_data(const uint8_t *data, size_t len);

/**
 * @brief Notify file end (called from wifi_udp server thread)
 */
void transport_udp_notify_file_end(void);

/**
 * @brief Notify transfer done (called from wifi_udp server thread)
 */
void transport_udp_notify_transfer_done(void);

/**
 * @brief Notify window size update (called from wifi_udp server thread)
 *
 * @param window_size New window size
 */
void transport_udp_notify_window_ack(uint16_t window_size);

/**
 * @brief Notify CRC verification result (called from wifi_udp server thread)
 *
 * @param crc CRC32 value
 * @param status 0=OK, 1=Error
 */
void transport_udp_notify_crc_result(uint32_t crc, uint8_t status);

/**
 * @brief Send AT command response with FRAME_AT_RESPONSE framing
 *
 * @param data Response data
 * @param len Response length
 * @return Bytes sent on success, negative error code on failure
 */
int transport_udp_send_response(const uint8_t *data, uint16_t len);

/**
 * @brief Get UDP transport pointer for registration
 *
 * @return Transport pointer
 */
struct transport *transport_udp_get(void);

#endif /* CLIP_TRANSPORT_UDP_H */