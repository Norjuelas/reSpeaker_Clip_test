/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_HEALTH_H
#define CLIP_HEALTH_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Write the device's health as JSON
 *
 * Everything an operator needs to decide whether a device in the field is
 * fine, low, full, or stuck: battery, storage, network, what it is doing, how
 * long it has been up, and why it last restarted.
 *
 * @param buf Destination
 * @param len Size of @p buf
 * @return bytes written, or negative on error
 */
int health_snapshot_json(char *buf, size_t len);

/**
 * @brief Start the periodic heartbeat to the configured endpoint
 *
 * Posts the same JSON to /health every CONFIG_CLIP_HEALTH_INTERVAL_S seconds.
 * The device pushes rather than serving a page of its own: an HTTP server on
 * the device would cost FLASH the image does not have, and a device behind a
 * home router is not reachable from outside anyway. Pushing works from any
 * network that can reach the service.
 */
int health_init(void);

/**
 * @brief Send one heartbeat now, off the caller's thread
 *
 * @retval 0        queued
 * @retval -EAGAIN  the heartbeat is not running
 */
int health_beat_now(void);

/**
 * @brief Turn the periodic heartbeat on or off at runtime
 */
void health_set_enabled(bool on);

/**
 * @brief Whether the periodic heartbeat is running
 */
bool health_is_enabled(void);

#endif /* CLIP_HEALTH_H */
