/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_H
#define CLIP_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Clip2 error codes
 */
#define CLIP_OK           0
#define CLIP_ERR_INVALID -1
#define CLIP_ERR_BUSY    -2
#define CLIP_ERR_NO_MEM  -3
#define CLIP_ERR_IO      -4
#define CLIP_ERR_TIMEOUT -5

/**
 * @brief Device states
 */
enum clip_state {
    CLIP_STATE_UNINITIALIZED = 0,
    CLIP_STATE_IDLE,
    CLIP_STATE_RECORDING,
    CLIP_STATE_TRANSMITTING,
    CLIP_STATE_WIFI_SYNC,
    CLIP_STATE_PAUSED,
    CLIP_STATE_ERROR,
};

/**
 * @brief Recording modes
 */
enum recording_mode {
    MODE_NORMAL = 0,
    MODE_ENHANCED,
};

/**
 * @brief Event handler callback type
 */
typedef void (*event_handler_t)(int event, void *data);

/**
 * @brief Clip2 context structure
 *
 * Central structure that holds all application state.
 * This replaces global variables and enables better modularity.
 */
struct clip_context {
    /* Device state */
    enum clip_state state;

    /* Configuration */
    struct clip_config {
        uint16_t bitrate;           /* Opus bitrate in bps */
        uint8_t complexity;         /* Opus complexity (0-10) */
        uint8_t mode;               /* Recording mode (normal/enhanced) */
        uint8_t noise_suppress;     /* Noise suppression level (dB) */
        uint16_t chunk_size;        /* Transfer chunk size */
        int8_t auto_delete_days;    /* Auto-delete policy: -1=off, 0-30=days */
        uint16_t agc_target;        /* AGC target level */
        bool agc_enabled;           /* AGC enabled */
        bool dereverb_enabled;      /* Dereverberation enabled */
        uint8_t oled_contrast;      /* OLED contrast/brightness (0-255) */
    } config;

    /* Status */
    struct clip_status {
        uint8_t battery_percent;
        bool battery_charging;
        uint32_t recording_time;
        uint32_t free_space;
        uint16_t session_count;
    } status;

    /* Time sync */
    struct clip_time {
        uint16_t year;
        uint8_t month;
        uint8_t day;
        uint8_t hour;
        uint8_t min;
        uint8_t sec;
        int64_t base_uptime_ms;
        bool valid;
    } time;
};

/**
 * @brief Get the global clip context
 *
 * @return Pointer to the clip context
 */
struct clip_context *clip_get_context(void);

/**
 * @brief Initialize clip application
 *
 * @return 0 on success, negative error code on failure
 */
int clip_init(void);

/**
 * @brief Main application loop
 */
void clip_main_loop(void);

/**
 * @brief Get current synchronized time
 *
 * @param out_year Output year
 * @param out_month Output month
 * @param out_day Output day
 * @param out_hour Output hour
 * @param out_min Output minute
 * @param out_sec Output second
 * @return true if time is valid, false otherwise
 */
bool clip_get_current_time(uint16_t *out_year, uint8_t *out_month, uint8_t *out_day,
                           uint8_t *out_hour, uint8_t *out_min, uint8_t *out_sec);

/**
 * @brief Convert state to string
 *
 * @param state State to convert
 * @return String representation
 */
const char *clip_state_to_string(enum clip_state state);

#endif /* CLIP_H */
