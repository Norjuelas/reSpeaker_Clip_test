/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_DISPLAY_H
#define CLIP_DISPLAY_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @file display.h
 * @brief UI display module for clip2
 *
 * Event-driven UI system with state machine for display management.
 */

/* =============================================================================
 * UI States
 * ============================================================================= */

/**
 * @brief UI display states
 */
enum ui_state {
	UI_STATE_OFF = 0,              /**< Display off */
	UI_STATE_PAIRING_GUIDE,        /**< Pairing guide (BLE not bonded) */
	UI_STATE_STATUS_BAR,           /**< Status bar with battery/connection */
	UI_STATE_REC_WAVE,             /**< Recording with wave animation */
	UI_STATE_REC_DOT,              /**< Recording with dot animation */
	UI_STATE_MARKING,              /**< Bookmark animation */
	UI_STATE_PAUSED,               /**< Paused recording */
	UI_STATE_POWER_OFF,            /**< Power-off confirmation */
};

/* =============================================================================
 * UI Events
 * ============================================================================= */

/**
 * @brief UI events for state transitions
 */
enum ui_event {
	UI_EVENT_REC_START = 0,        /**< Recording started */
	UI_EVENT_REC_STOP,             /**< Recording stopped */
	UI_EVENT_REC_PAUSE,            /**< Recording paused */
	UI_EVENT_REC_RESUME,           /**< Recording resumed */
	UI_EVENT_MARK,                 /**< Bookmark added */
	UI_EVENT_STATUS_SHOW,          /**< Show status bar */
	UI_EVENT_BONDED,               /**< BLE bonded */
	UI_EVENT_POWER_OFF_SHOW,       /**< Show power-off screen */
	UI_EVENT_TIMEOUT,              /**< State timeout */
};

/* =============================================================================
 * Display Status Structure
 * ============================================================================= */

/**
 * @brief Display status information
 */
struct display_status {
	uint8_t battery_percent;       /**< Battery percentage (0-100) */
	bool battery_charging;         /**< Battery charging status */
	bool ble_connected;            /**< BLE connected */
	bool transferring;             /**< File transfer in progress */
};

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Initialize display module
 * @return 0 on success, negative errno on failure
 */
int display_init(void);

/**
 * @brief Check if display is ready
 * @return true if display device is ready
 */
bool display_is_ready(void);

/**
 * @brief Post event to UI queue
 * @param event Event to post
 * @return 0 on success, negative errno on failure
 */
int display_post_event(enum ui_event event);

/**
 * @brief Update display status
 * @param status New status information
 * @return 0 on success, negative errno on failure
 */
int display_update_status(const struct display_status *status);

/**
 * @brief Set recording mode display
 * @param recording True if recording, false otherwise
 * @param enhanced_mode True for enhanced mode (fast animation)
 * @return 0 on success, negative errno on failure
 */
int display_set_recording(bool recording, bool enhanced_mode);

/**
 * @brief Clear display (turn off)
 * @return 0 on success, negative errno on failure
 */
int display_turn_off(void);

#endif /* CLIP_DISPLAY_H */
