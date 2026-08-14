/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_WIFI_H
#define CLIP_WIFI_H

#include <stdbool.h>

/**
 * @brief WiFi AP configuration
 */
#define WIFI_AP_SSID_PREFIX "ClipAP_"
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 36          /* 5GHz channel */
#define WIFI_AP_MAX_CLIENTS 1
#define WIFI_AP_REG_DOMAIN "US"

/**
 * @brief UDP file transfer server configuration
 */
#define WIFI_AP_UDP_PORT 8089

/**
 * @brief Initialize WiFi module
 *
 * Initializes the WiFi subsystem and generates AP SSID.
 * Must be called before any other WiFi functions.
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_init(void);

/**
 * @brief Power on WiFi interface and start AP
 *
 * Brings up the WiFi network interface and starts AP mode.
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_on(void);

/**
 * @brief Power off WiFi interface
 *
 * Stops AP mode and powers off the radio.
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_off(void);

/**
 * @brief Check if WiFi interface is up
 *
 * @return true if interface is up (radio powered on), false otherwise
 */
bool wifi_is_interface_up(void);

/**
 * @brief Check if WiFi AP is running
 *
 * @return true if AP is running, false otherwise
 */
bool wifi_ap_is_running(void);

/**
 * @brief Get AP SSID
 *
 * @return Pointer to SSID string (static buffer)
 */
const char *wifi_get_ssid(void);

/**
 * @brief Get AP password
 *
 * @return Pointer to password string
 */
const char *wifi_get_password(void);

/**
 * @brief Get AP IP address
 *
 * @return Pointer to IP address string
 */
const char *wifi_get_ip_address(void);

/**
 * @brief Check if a station is connected to the AP
 *
 * @return true if a station is connected, false otherwise
 */
bool wifi_ap_has_client(void);

/**
 * @brief Join the configured network in station mode
 *
 * Credentials come from config (AT+STACFG). Stops the AP first if it is
 * running — the two modes cannot coexist, the interface holds a single IPv4
 * address. Blocks until a DHCP lease arrives or the attempt times out.
 *
 * @return 0 once the interface has an IP address
 * @retval -ENOENT       no credentials configured
 * @retval -ECONNREFUSED the AP rejected us (wrong passphrase, most likely)
 * @retval -ETIMEDOUT    no answer from the AP, or no DHCP lease
 */
int wifi_sta_on(void);

/**
 * @brief Start joining the configured network without blocking
 *
 * Runs wifi_sta_on() on a dedicated work queue and returns immediately.
 * Progress is reported over BLE as the "sta" event: the IP address on
 * success, "failed" otherwise.
 *
 * @return 0 if the attempt was queued
 * @retval -ENOENT   no credentials configured
 * @retval -EALREADY already connected
 * @retval -EAGAIN   wifi_init() has not run yet
 */
int wifi_sta_connect_async(void);

/**
 * @brief Same as wifi_sta_connect_async(), but after a delay
 *
 * Powering the nRF70 allocates ~60KB of heap. Doing that while the system is
 * still starting up hangs the boot, so the boot-time join is scheduled rather
 * than fired immediately.
 *
 * @param delay_ms How long to wait before bringing the radio up
 * @return 0 if the attempt was scheduled, negative error code otherwise
 */
int wifi_sta_connect_async_delayed(uint32_t delay_ms);

/**
 * @brief Leave the network and power the interface down
 *
 * @return 0 on success, negative error code on failure
 */
int wifi_sta_off(void);

/**
 * @brief Whether we are associated *and* hold a DHCP lease
 *
 * @return true if the station link is usable
 */
bool wifi_sta_is_connected(void);

/**
 * @brief Minutos que lleva sin conseguir conectarse a ninguna red
 *
 * 0 si esta conectado, o si todavia no lo ha intentado. Se usa para decidir
 * cuando apagarse: un device que no encuentra su red gasta la bateria
 * despertando la radio para reintentar, y con 170 mAh eso la vacia en unas
 * horas sin haber hecho nada util.
 */
uint32_t wifi_sta_offline_minutes(void);

/**
 * @brief Get the IP address obtained by DHCP in station mode
 *
 * @return Dotted-quad string, empty if not connected
 */
const char *wifi_sta_get_ip(void);

/**
 * @brief Why the last station-mode attempt failed
 *
 * Text from wifi_conn_status_txt(), e.g. "Authentication failure" or
 * "Network not found". Empty if the last attempt succeeded or none was made.
 *
 * @return Reason string, never NULL
 */
const char *wifi_sta_get_fail_reason(void);

#endif /* CLIP_WIFI_H */
