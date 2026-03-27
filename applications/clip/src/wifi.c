/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdio.h>
#include <string.h>
#include <net/wifi_ready.h>
#include "wifi.h"
#include "wifi_udp.h"
#include "transport_udp.h"


#ifdef CONFIG_NRF70_SR_COEX
#include <coex.h>
#endif

LOG_MODULE_REGISTER(wifi, CONFIG_CLIP_LOG_LEVEL);

#define WIFI_AP_MGMT_EVENTS (NET_EVENT_WIFI_AP_ENABLE_RESULT |  \
							 NET_EVENT_WIFI_AP_DISABLE_RESULT | \
							 NET_EVENT_WIFI_AP_STA_CONNECTED |  \
							 NET_EVENT_WIFI_AP_STA_DISCONNECTED)

static char ap_ssid[32] = "ClipAP_Test";
static bool sta_connected;
static bool ap_running;
static bool wifi_ready;

static struct net_mgmt_event_callback wifi_mgmt_cb;
static K_SEM_DEFINE(wifi_ready_sem, 0, 1);
static K_SEM_DEFINE(ap_enabled_sem, 0, 1);

#ifdef CONFIG_NRF70_SR_COEX
static void wifi_coex_configure(void)
{
	bool sep = IS_ENABLED(CONFIG_COEX_SEP_ANTENNAS);
	bool ble = IS_ENABLED(CONFIG_SR_PROTOCOL_BLE);
	int ret;

	ret = nrf_wifi_coex_config_non_pta(sep, ble);
	if (ret)
	{
		LOG_WRN("Coex non-PTA config failed: %d", ret);
	}
	ret = nrf_wifi_coex_config_pta(NRF_WIFI_PTA_WLAN_OP_BAND_5_GHZ, sep, ble);
	if (ret)
	{
		LOG_WRN("Coex PTA config failed: %d", ret);
	}
	LOG_INF("Coex PTA configured (5GHz, sep=%d, ble=%d)", sep, ble);
}
#endif

static void generate_ap_ssid(void)
{
	uint8_t chip_id[16];
	ssize_t len = hwinfo_get_device_id(chip_id, sizeof(chip_id));

	if (len > 0)
	{
		uint32_t suffix = 0;
		int off = len > 4 ? len - 4 : 0;

		for (int i = 0; i < 4 && (off + i) < len; i++)
		{
			suffix = (suffix << 8) | chip_id[off + i];
		}
		snprintf(ap_ssid, sizeof(ap_ssid), "%s%04X",
				 WIFI_AP_SSID_PREFIX, (unsigned)(suffix & 0xFFFF));
	}
	LOG_INF("AP SSID: %s", ap_ssid);
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
									uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event)
	{
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
	{
		const struct wifi_status *status = (const struct wifi_status *)cb->info;
		if (status->status)
		{
			LOG_ERR("AP enable failed: %d", status->status);
		}
		else
		{
			LOG_INF("WiFi AP enabled");
			ap_running = true;
		}
		k_sem_give(&ap_enabled_sem);
		break;
	}
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		LOG_INF("WiFi AP disabled");
		ap_running = false;
		sta_connected = false;
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
	{
		const struct wifi_ap_sta_info *sta_info = (const struct wifi_ap_sta_info *)cb->info;
		char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];
		snprintf(mac_string_buf, sizeof(mac_string_buf),
				 "%02x:%02x:%02x:%02x:%02x:%02x",
				 sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
				 sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
		LOG_INF("Station connected: %s", mac_string_buf);
		sta_connected = true;
		transport_udp_update_active(false); /* Reset, waiting for new client */
		break;
	}
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
	{
		const struct wifi_ap_sta_info *sta_info = (const struct wifi_ap_sta_info *)cb->info;
		char mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];
		snprintf(mac_string_buf, sizeof(mac_string_buf),
				 "%02x:%02x:%02x:%02x:%02x:%02x",
				 sta_info->mac[0], sta_info->mac[1], sta_info->mac[2],
				 sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
		LOG_INF("Station disconnected: %s", mac_string_buf);
		sta_connected = false;
		transport_udp_update_active(false); /* Notify transport of disconnect */
		break;
	}
	default:
		break;
	}
}

static void wifi_ready_callback(bool ready)
{
	LOG_DBG("WiFi ready: %s", ready ? "yes" : "no");
	wifi_ready = ready;
	if (ready)
	{
		k_sem_give(&wifi_ready_sem);
	}
}

int wifi_init(void)
{
	struct net_if *iface;
	wifi_ready_callback_t cb;
	int ret;

	generate_ap_ssid();

	/* Register WiFi management events */
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
								 WIFI_AP_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	/* Register WiFi ready callback */
	iface = net_if_get_first_wifi();
	if (iface)
	{
		cb.wifi_ready_cb = wifi_ready_callback;
		ret = register_wifi_ready_callback(cb, iface);
		if (ret)
		{
			LOG_WRN("WiFi ready callback registration failed: %d", ret);
		}
	}

	LOG_INF("WiFi module initialized");

	
#ifdef CONFIG_NRF70_SR_COEX
	wifi_coex_configure();
#endif

	return 0;
}

static int wifi_set_reg_domain(struct net_if *iface)
{
	struct wifi_reg_domain regd = {0};
	int ret;

	regd.oper = WIFI_MGMT_SET;
	strncpy(regd.country_code, WIFI_AP_REG_DOMAIN, WIFI_COUNTRY_CODE_LEN + 1);

	ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &regd, sizeof(regd));
	if (ret)
	{
		LOG_WRN("Failed to set reg domain: %d", ret);
	}

	return ret;
}

static int wifi_enable_ap(struct net_if *iface)
{
	struct wifi_connect_req_params req;
	int ret;

	memset(&req, 0, sizeof(req));
	req.ssid = (const uint8_t *)ap_ssid;
	req.ssid_length = strlen(ap_ssid);
	req.psk = (const uint8_t *)WIFI_AP_PASSWORD;
	req.psk_length = strlen(WIFI_AP_PASSWORD);
	req.channel = WIFI_AP_CHANNEL;
	req.security = WIFI_SECURITY_TYPE_PSK;
	req.mfp = WIFI_MFP_OPTIONAL;
	req.band = WIFI_FREQ_BAND_5_GHZ;

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &req, sizeof(req));
	if (ret)
	{
		LOG_ERR("AP enable request failed: %d", ret);
		return ret;
	}

	/* Wait for AP_ENABLE_RESULT event */
	ret = k_sem_take(&ap_enabled_sem, K_SECONDS(5));
	if (ret)
	{
		LOG_ERR("Timeout waiting for AP enable result");
		return -ETIMEDOUT;
	}

	if (!ap_running)
	{
		LOG_ERR("AP enable failed");
		return -EIO;
	}

	return 0;
}

static int wifi_start_dhcp_server(struct net_if *iface)
{
	struct in_addr pool_start;
	int ret;

	net_addr_pton(AF_INET, WIFI_AP_DHCP_POOL_START, &pool_start);
	ret = net_dhcpv4_server_start(iface, &pool_start);
	if (ret && ret != -EALREADY)
	{
		LOG_WRN("DHCP server start failed: %d", ret);
	}
	else
	{
		LOG_INF("DHCP server started, pool: %s/1", WIFI_AP_DHCP_POOL_START);
	}

	return ret;
}

int wifi_on(void)
{
	struct net_if *iface;
	int ret;

	/* Check if already running */
	if (ap_running)
	{
		LOG_DBG("AP already running");
		return 0;
	}

	iface = net_if_get_first_wifi();
	if (!iface)
	{
		LOG_ERR("No WiFi interface");
		return -ENODEV;
	}

	/* Bring up interface if not already up (required when CONFIG_NRF_WIFI_IF_AUTO_START=n) */
	if (!net_if_is_admin_up(iface))
	{
		LOG_INF("Interface is down, bringing up");
		ret = net_if_up(iface);
		if (ret)
		{
			LOG_ERR("net_if_up failed: %d", ret);
			return ret;
		}
		LOG_INF("WiFi interface up");

		/* Wait for WPA supplicant to initialize */
		k_sleep(K_SECONDS(2));
	}

	/* Set regulatory domain */
	wifi_set_reg_domain(iface);

	/* Enable AP mode */
	ret = wifi_enable_ap(iface);
	if (ret)
	{
		return ret;
	}

	/* Start DHCP server */
	wifi_start_dhcp_server(iface);

	/* Start UDP server */
	ret = wifi_udp_start();
	if (ret)
	{
		LOG_WRN("UDP server start failed: %d", ret);
		/* Continue anyway - UDP is optional */
	}

	LOG_INF("WiFi AP started: SSID=%s ch=%d IP=%s port=%d",
			ap_ssid, WIFI_AP_CHANNEL, WIFI_AP_IP_ADDR, WIFI_AP_UDP_PORT);

	return 0;
}

int wifi_off(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	if (!iface)
	{
		return -ENODEV;
	}

	/* Stop UDP server */
	wifi_udp_stop();

	/* Stop DHCP server */
	net_dhcpv4_server_stop(iface);

	/* Disable AP mode */
	net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
	ap_running = false;
	sta_connected = false;

	/* Power down interface to save power (required when CONFIG_NRF_WIFI_IF_AUTO_START=n) */
	if (net_if_is_admin_up(iface))
	{
		ret = net_if_down(iface);
		if (ret)
		{
			LOG_WRN("net_if_down failed: %d", ret);
		}
		else
		{
			LOG_INF("WiFi interface down");
		}
	}

	LOG_INF("WiFi AP stopped");
	return 0;
}

bool wifi_is_interface_up(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface)
	{
		return false;
	}
	return net_if_is_up(iface);
}

bool wifi_ap_is_running(void)
{
	return ap_running;
}

const char *wifi_get_ssid(void)
{
	return ap_ssid;
}

const char *wifi_get_password(void)
{
	return WIFI_AP_PASSWORD;
}

const char *wifi_get_ip_address(void)
{
	return WIFI_AP_IP_ADDR;
}

bool wifi_is_sta_connected(void)
{
	return sta_connected;
}
