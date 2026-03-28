/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_CONFIG_H
#define CLIP_CONFIG_H

#include <stdint.h>
#include <stddef.h>

/* Configuration keys */
#define CONFIG_KEY_MODE          0x03
#define CONFIG_KEY_NOISE         0x04
#define CONFIG_KEY_AUTODEL       0x06
#define CONFIG_KEY_DEREVERB      0x09
#define CONFIG_KEY_BRIGHTNESS    0x0A

/* Default values */
#define CONFIG_DEFAULT_MODE          0  /* MODE_NORMAL */
#define CONFIG_DEFAULT_NOISE         15
#define CONFIG_DEFAULT_AUTODEL       -1
#define CONFIG_DEFAULT_DEREVERB      false
#define CONFIG_DEFAULT_BRIGHTNESS    128

/**
 * @brief Initialize configuration system
 *
 * @return 0 on success, negative error code on failure
 */
int config_init(void);

/**
 * @brief Load configuration from storage
 *
 * @return 0 on success, negative error code on failure
 */
int config_load(void);

/**
 * @brief Save configuration to storage
 *
 * @return 0 on success, negative error code on failure
 */
int config_save(void);

/**
 * @brief Reset configuration to factory defaults
 *
 * @return 0 on success, negative error code on failure
 */
int config_factory_reset(void);

/**
 * @brief Set configuration value
 *
 * @param key   Configuration key
 * @param value Pointer to value
 * @param len   Length of value
 * @return 0 on success, negative error code on failure
 */
int config_set(uint16_t key, const void *value, size_t len);

/**
 * @brief Get configuration value
 *
 * @param key    Configuration key
 * @param value  Output buffer
 * @param len    Length of buffer
 * @return 0 on success, negative error code on failure
 */
int config_get(uint16_t key, void *value, size_t len);

/**
 * @brief Save Unix timestamp to storage
 *
 * @param unix_time Unix timestamp
 * @return 0 on success, negative error code on failure
 */
int config_set_time(int64_t unix_time);

/**
 * @brief Get Unix timestamp from storage
 *
 * @param unix_time Output buffer for Unix timestamp
 * @return 0 on success, negative error code if time not set
 */
int config_get_time(int64_t *unix_time);

/**
 * @brief Set time values (YMDHMS)
 *
 * @param year Year
 * @param month Month (1-12)
 * @param day Day (1-31)
 * @param hour Hour (0-23)
 * @param min Minute (0-59)
 * @param sec Second (0-59)
 * @return 0 on success, negative error code on failure
 */
int config_set_time_ymd(uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t min, uint8_t sec);

/**
 * @brief Set recording mode
 *
 * @param mode Recording mode
 * @return 0 on success, negative error code on failure
 */
int config_set_mode(enum recording_mode mode);

/**
 * @brief Set noise suppression level
 *
 * @param noise Noise suppression in dB
 * @return 0 on success, negative error code on failure
 */
int config_set_noise_suppress(uint8_t noise);

/**
 * @brief Set auto-delete days
 *
 * @param days Days (0-30) or -1 for off
 * @return 0 on success, negative error code on failure
 */
int config_set_auto_delete_days(int8_t days);

/**
 * @brief Set dereverb enabled
 *
 * @param enabled true to enable, false to disable
 * @return 0 on success, negative error code on failure
 */
int config_set_dereverb_enabled(bool enabled);

/**
 * @brief Set OLED brightness
 *
 * @param brightness Brightness (0-255)
 * @return 0 on success, negative error code on failure
 */
int config_set_oled_brightness(uint8_t brightness);

#endif /* CLIP_CONFIG_H */
