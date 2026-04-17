/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_BATTERY_H
#define CLIP_BATTERY_H

/**
 * @brief Initialize battery monitoring
 *
 * Sets up NPM1300 interrupt callbacks for charging events (VBUS, charge complete)
 * and starts periodic battery level polling (60s interval).
 * Updates BLE Battery Service with level and charging status.
 *
 * @return 0 on success, negative error code on failure
 */
int battery_init(void);

/**
 * @brief Poll battery status immediately
 *
 * Reads sensors, updates fuel gauge SoC, and refreshes display/BLE.
 * Call this when the user triggers a status bar display to get fresh readings.
 */
void battery_poll(void);

#endif /* CLIP_BATTERY_H */
