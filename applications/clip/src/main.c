/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

#include <nrfx_clock.h>

#include "clip.h"
#include "config.h"
#include "ble.h"
#include "transport.h"
#include "transport_ble.h"
#include "at_server.h"
#include "at_commands.h"
#include "audio.h"
#include "storage.h"
#include "transfer.h"
#include "button.h"
#include "display.h"
#include "wifi.h"
#include "wifi_udp.h"
#include "transport_udp.h"
#include "clip_event.h"
#include "battery.h"

LOG_MODULE_REGISTER(main, CONFIG_CLIP_LOG_LEVEL);

/* Global context */
static struct clip_context g_ctx;

struct clip_context *clip_get_context(void)
{
    return &g_ctx;
}

const char *clip_state_to_string(enum clip_state state)
{
    switch (state) {
    case CLIP_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case CLIP_STATE_IDLE:
        return "IDLE";
    case CLIP_STATE_RECORDING:
        return "RECORDING";
    case CLIP_STATE_TRANSMITTING:
        return "TRANSMITTING";
    case CLIP_STATE_WIFI_SYNC:
        return "WIFI_SYNC";
    case CLIP_STATE_PAUSED:
        return "PAUSED";
    case CLIP_STATE_ERROR:
        return "ERROR";
    case CLIP_STATE_OTA:
        return "OTA";
    default:
        return "UNKNOWN";
    }
}

bool clip_get_current_time(uint16_t *out_year, uint8_t *out_month, uint8_t *out_day,
                           uint8_t *out_hour, uint8_t *out_min, uint8_t *out_sec)
{
    uint32_t sec;

    if (!g_ctx.time.valid) {
        return false;
    }

    /* Calculate elapsed time since sync */
    int64_t current_uptime = k_uptime_get();
    int64_t elapsed_ms = current_uptime - g_ctx.time.base_uptime_ms;

    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }

    /* Convert to seconds */
    uint32_t elapsed_sec = (uint32_t)(elapsed_ms / 1000);

    /* Start from synced time */
    uint16_t year = g_ctx.time.year;
    uint8_t month = g_ctx.time.month;
    uint8_t day = g_ctx.time.day;
    uint8_t hour = g_ctx.time.hour;
    uint8_t min = g_ctx.time.min;
    sec = g_ctx.time.sec;

    /* Add elapsed seconds */
    sec += elapsed_sec;

    /* Handle overflow */
    while (sec >= 60) {
        sec -= 60;
        min++;
    }
    while (min >= 60) {
        min -= 60;
        hour++;
    }
    while (hour >= 24) {
        hour -= 24;
        day++;
    }

    /* Simple day overflow handling */
    uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    while (day > days_in_month[month - 1]) {
        day -= days_in_month[month - 1];
        month++;
        if (month > 12) {
            month = 1;
            year++;
        }
    }

    *out_year = year;
    *out_month = month;
    *out_day = day;
    *out_hour = hour;
    *out_min = min;
    *out_sec = (uint8_t)sec;

    return true;
}

int clip_init(void)
{
    int err;

    LOG_INF("Initializing Clip2...");

    /* Initialize context */

    /* Initialize configuration (will set defaults and load from storage) */
    err = config_init();
    if (err) {
        LOG_WRN("Config init failed: %d, using defaults", err);
        /* Continue anyway - config is optional */
    }

    /* Initialize display early (before slow BLE init) to light up screen */
    err = display_init();
    if (err) {
        LOG_WRN("Display init failed: %d", err);
        /* Continue anyway - display is optional */
    }

    /* Initialize BLE (bt_enable is slow, display is already showing) */
    err = ble_init();
    if (err) {
        LOG_ERR("BLE init failed: %d", err);
        return err;
    }

    /* Initialize battery monitoring (needs BLE for BAS service) */
    err = battery_init();
    if (err) {
        LOG_WRN("Battery init failed: %d", err);
    }

    /* Initialize transport layer */
    err = transport_init();
    if (err) {
        LOG_ERR("Transport init failed: %d", err);
        return err;
    }

    /* Initialize BLE transport (now that BLE is ready) */
    err = transport_ble_init();
    if (err) {
        LOG_ERR("BLE transport init failed: %d", err);
        return err;
    }

    /* Register BLE transport */
    err = transport_register(transport_ble_get());
    if (err) {
        LOG_ERR("BLE transport register failed: %d", err);
        return err;
    }

    /* Initialize UDP transport */
    err = transport_udp_init();
    if (err) {
        LOG_WRN("UDP transport init failed: %d", err);
        /* Continue anyway - UDP is optional */
    }

    /* Register UDP transport */
    err = transport_register(transport_udp_get());
    if (err) {
        LOG_WRN("UDP transport register failed: %d", err);
        /* Continue anyway - UDP is optional */
    }

    /* Initialize audio subsystem */
    err = audio_init();
    if (err) {
        LOG_ERR("Audio init failed: %d", err);
        return err;
    }

    /* Initialize storage subsystem */
    err = storage_init();
    if (err) {
        LOG_WRN("Storage init failed: %d", err);
        /* Continue anyway - storage is optional */
    }

    /* Initialize transfer subsystem */
    err = transfer_init();
    if (err) {
        LOG_WRN("Transfer init failed: %d", err);
        /* Continue anyway - transfer is optional */
    }

    /* Initialize WiFi subsystem */
    err = wifi_init();
    if (err) {
        LOG_WRN("WiFi init failed: %d", err);
        /* Continue anyway - WiFi is optional */
    }

    /* Initialize WiFi UDP server */
    err = wifi_udp_init();
    if (err) {
        LOG_WRN("WiFi UDP init failed: %d", err);
        /* Continue anyway - UDP is optional */
    }

    /* Register AT commands BEFORE starting server thread */
    err = at_commands_register();
    if (err) {
        LOG_ERR("AT commands register failed: %d", err);
        return err;
    }

    /* Initialize AT server (commands must be registered first) */
    err = at_server_init();
    if (err) {
        LOG_ERR("AT server init failed: %d", err);
        return err;
    }

    /* Initialize button handler */
    err = button_init();
    if (err) {
        LOG_WRN("Button init failed: %d", err);
        /* Continue anyway - button is optional */
    }

    /* Initialize event dispatcher (depends on audio, haptic, display, wifi) */
    err = clip_event_init();
    if (err) {
        LOG_ERR("Event dispatcher init failed: %d", err);
        return err;
    }

    /* Clear status (battery_percent/charging set by battery_init) */
    g_ctx.status.recording_time = 0;
    g_ctx.status.free_space = 0;
    g_ctx.status.session_count = 0;

    /* Transition to idle state */
    clip_event_get_state();  /* Already set by clip_event_init */

    LOG_INF("Clip initialized successfully");
    LOG_INF("Device ready, mode: normal=%u/%u enhanced=%u/%u",
           CONFIG_CLIP_NORMAL_BITRATE, CONFIG_CLIP_NORMAL_COMPLEXITY,
           CONFIG_CLIP_ENHANCED_BITRATE, CONFIG_CLIP_ENHANCED_COMPLEXITY);

    return 0;
}

void clip_main_loop(void)
{
    LOG_INF("Entering main loop");

    while (true) {
        /* Wait for events (timeout 1s for recording time update) */
        clip_event_wait(K_MSEC(1000));

        /* Process all pending events */
        clip_event_process();

        /* Update recording time if recording */
        if (clip_event_get_state() == CLIP_STATE_RECORDING) {
            g_ctx.status.recording_time++;
        }
    }
}

int main(void)
{
    /* Print version and build info at startup */
    LOG_INF("Clip2 Firmware v%s", APP_VERSION_STRING);
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    int err;

    /* Initialize application */
    err = clip_init();
    if (err) {
        LOG_ERR("Application init failed: %d", err);
        return err;
    }

    /* Enter main loop */
    clip_main_loop();

    return 0;
}
