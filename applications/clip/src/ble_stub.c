/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The BLE surface with nothing behind it, built instead of ble.c and
 * transport_ble.c when CONFIG_BT=n.
 *
 * Why the Bluetooth stack is gone, and it is not the FLASH: the device is meant
 * to be reachable only over the USB cable or over WiFi by an approved path. An
 * open, unauthenticated radio channel is attack surface across 100 deployed
 * devices, and that applies to the AP mode too — its SSID and fixed password
 * are the same on every unit. Doc 14 §2.
 *
 * That removing BLE also frees ~15KB, which is what let TLS fit, is a happy
 * coincidence rather than the reason. If space stopped being tight tomorrow,
 * BLE would still be out.
 *
 * What it costs, plainly: the mobile SDKs under mobile/ talk to the device over
 * BLE and stop working, and BLE OTA goes with it — leaving the cable as the only
 * way to update, which is why an HTTP OTA is now required rather than nice to
 * have. Reversible all the same: set CONFIG_BT=y and ble.c comes back with every
 * caller untouched.
 *
 * Stubs rather than #ifdefs at the call sites: fourteen BLE functions are
 * called from six files that have nothing to do with Bluetooth (audio, wifi,
 * storage events). Guarding each one would spread the decision across the
 * codebase and make putting BLE back a merge instead of a Kconfig flip.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stddef.h>
#include <errno.h>

#include "ble.h"
#include "transport.h"
#include "transport_ble.h"

LOG_MODULE_REGISTER(ble_stub, CONFIG_CLIP_LOG_LEVEL);

int ble_init(void)
{
	LOG_WRN("Built without Bluetooth: USB and WiFi are the only channels");
	return 0;
}

int ble_register_cmd_callback(ble_cmd_callback_t callback)
{
	ARG_UNUSED(callback);
	return 0;
}

int ble_send(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

int ble_send_file_data(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

int ble_send_response_buffer(size_t len)
{
	ARG_UNUSED(len);
	return -ENOTCONN;
}

int ble_send_audio_vis(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

bool ble_is_connected(void)
{
	return false;
}

bool ble_is_notify_enabled(void)
{
	return false;
}

bool ble_is_file_data_notify_enabled(void)
{
	return false;
}

/* False, not true: callers use this to decide whether a phone is paired, and
 * claiming a bond that cannot exist would send them down the BLE path. */
bool ble_is_bonded(void)
{
	return false;
}

bool ble_is_audio_vis_subscribed(void)
{
	return false;
}

int ble_get_bond_addr(char *addr_buf, size_t len)
{
	if (addr_buf && len) {
		addr_buf[0] = '\0';
	}
	return -ENOENT;
}

int ble_clear_bonds(void)
{
	return 0;
}

struct bt_conn *ble_get_connection(void)
{
	return NULL;
}

const char *ble_get_device_name(void)
{
	return "clip";
}

void ble_adv_restart_fast(void)
{
}

void ble_activity_refresh(void)
{
}

/* The notify_* calls are how the rest of the firmware announces what it is
 * doing. With no link to announce it over they simply succeed: the callers
 * treat a failure as something worth logging, and a device with no Bluetooth
 * would log on every state change forever. */
int ble_notify_state_change(const char *state, const char *session_id, int duration)
{
	ARG_UNUSED(state);
	ARG_UNUSED(session_id);
	ARG_UNUSED(duration);
	return 0;
}

int ble_notify_mark(const char *session_id, int mark_count)
{
	ARG_UNUSED(session_id);
	ARG_UNUSED(mark_count);
	return 0;
}

int ble_notify_event(const char *name, const char *status)
{
	ARG_UNUSED(name);
	ARG_UNUSED(status);
	return 0;
}

/* ---- transport_ble ---- */

int transport_ble_init(void)
{
	return 0;
}

int transport_ble_register_callback(transport_event_cb_t callback)
{
	ARG_UNUSED(callback);
	return 0;
}

void transport_ble_update_connection(void *conn, bool ready)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ready);
}

int transport_ble_send(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

bool transport_ble_is_connected(void)
{
	return false;
}

void *transport_ble_get_conn(void)
{
	return NULL;
}

/* NULL, so main.c never registers a transport that cannot carry anything.
 * transport_register() is expected to reject it. */
struct transport *transport_ble_get(void)
{
	return NULL;
}
