/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * WiFi AP mode test module.
 * Reference: samples/wifi_ble_coex
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/zperf.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"

#ifdef CONFIG_NRF70_SR_COEX
#include <coex.h>
#endif

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_INF);

/* =========================================================================
 * WiFi AP Configuration
 * ========================================================================= */
#define WIFI_AP_SSID_PREFIX "ClipTest_"
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 36
#define WIFI_AP_REG_DOMAIN "US"

#define IPERF_PORT 5001
#define IPERF_PACKET_SIZE 1400
#define IPERF_DURATION_SEC 10

static bool ap_started;
static char ap_ssid[32] = "ClipTest_XXXX";
static struct net_mgmt_event_callback wifi_mgmt_cb;
static K_SEM_DEFINE(ap_enabled_sem, 0, 1);

/* Scan state */
static K_SEM_DEFINE(scan_done_sem, 0, 1);
static struct wifi_scan_result scan_results[10];
static int scan_result_count;

/* =========================================================================
 * AP SSID Generation (from chip ID)
 * ========================================================================= */
static void generate_ap_ssid(void)
{
	uint8_t chip_id[16];
	ssize_t len = hwinfo_get_device_id(chip_id, sizeof(chip_id));

	if (len > 0) {
		uint32_t suffix = 0;
		int off = len > 4 ? len - 4 : 0;

		for (int i = 0; i < 4 && (off + i) < len; i++) {
			suffix = (suffix << 8) | chip_id[off + i];
		}
		snprintf(ap_ssid, sizeof(ap_ssid), "%s%04X",
			 WIFI_AP_SSID_PREFIX, (unsigned)(suffix & 0xFFFF));
	}
}

/* =========================================================================
 * WiFi Coexistence
 * ========================================================================= */
#ifdef CONFIG_NRF70_SR_COEX
static void wifi_coex_configure(void)
{
	bool sep = IS_ENABLED(CONFIG_COEX_SEP_ANTENNAS);
	bool ble = IS_ENABLED(CONFIG_SR_PROTOCOL_BLE);
	int ret;

	ret = nrf_wifi_coex_config_non_pta(sep, ble);
	if (ret) {
		LOG_WRN("Coex non-PTA config failed: %d", ret);
	}
	ret = nrf_wifi_coex_config_pta(NRF_WIFI_PTA_WLAN_OP_BAND_5_GHZ, sep, ble);
	if (ret) {
		LOG_WRN("Coex PTA config failed: %d", ret);
	}
	LOG_INF("Coex PTA configured (5GHz, sep=%d, ble=%d)", sep, ble);
}
#endif

/* =========================================================================
 * WiFi Event Handler
 * ========================================================================= */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
	{
		const struct wifi_status *status =
			(const struct wifi_status *)cb->info;
		if (status && status->status == 0) {
			printk("[WiFi] AP enabled\n");
			ap_started = true;
		} else {
			printk("[WiFi] AP enable failed: %d\n",
			       status ? status->status : -999);
		}
		k_sem_give(&ap_enabled_sem);
		break;
	}
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		printk("[WiFi] AP disabled\n");
		ap_started = false;
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		printk("[WiFi] Station connected\n");
		break;
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
		printk("[WiFi] Station disconnected\n");
		break;
	default:
		break;
	}
}

static void scan_result_callback(struct net_mgmt_event_callback *cb,
				 uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_WIFI_SCAN_RESULT) {
		struct wifi_scan_result *result =
			(struct wifi_scan_result *)cb->info;

		if (scan_result_count < 10 && result) {
			memcpy(&scan_results[scan_result_count], result,
			       sizeof(struct wifi_scan_result));
			scan_result_count++;
			printk("Found: %s, RSSI: %d\n", result->ssid, result->rssi);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_SCAN_DONE) {
		printk("Scan done, found %d networks\n", scan_result_count);
		k_sem_give(&scan_done_sem);
	}
}

/* =========================================================================
 * AP Start / Stop
 * ========================================================================= */
static int do_wifi_ap_start(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params req;
	struct wifi_reg_domain regd = {0};
	int ret;

	if (!iface) {
		return -ENODEV;
	}
	if (ap_started) {
		printk("AP already running: %s\n", ap_ssid);
		return 0;
	}

	/* Bring up interface */
	ret = net_if_up(iface);
	if (ret) {
		printk("[WiFi] net_if_up failed: %d\n", ret);
		return ret;
	}

	k_sleep(K_SECONDS(2));

	/* Set regulatory domain */
	regd.oper = WIFI_MGMT_SET;
	strncpy(regd.country_code, WIFI_AP_REG_DOMAIN, WIFI_COUNTRY_CODE_LEN + 1);
	ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &regd, sizeof(regd));
	if (ret) {
		LOG_WRN("Reg domain failed: %d", ret);
	}

	/* Configure AP parameters */
	memset(&req, 0, sizeof(req));
	req.ssid = (const uint8_t *)ap_ssid;
	req.ssid_length = strlen(ap_ssid);
	req.psk = (const uint8_t *)WIFI_AP_PASSWORD;
	req.psk_length = strlen(WIFI_AP_PASSWORD);
	req.channel = WIFI_AP_CHANNEL;
	req.security = WIFI_SECURITY_TYPE_PSK;
	req.mfp = WIFI_MFP_OPTIONAL;
	req.band = WIFI_FREQ_BAND_5_GHZ;

	k_sem_reset(&ap_enabled_sem);
	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &req, sizeof(req));
	if (ret) {
		LOG_ERR("AP enable request failed: %d", ret);
		return ret;
	}

	/* Wait for AP_ENABLE_RESULT event */
	ret = k_sem_take(&ap_enabled_sem, K_SECONDS(5));
	if (ret) {
		printk("[WiFi] Timeout waiting for AP enable result\n");
		return -ETIMEDOUT;
	}
	if (!ap_started) {
		printk("[WiFi] AP enable failed\n");
		return -EIO;
	}

	/* Manually configure static IP (matching main application) */
	{
		struct in_addr addr, netmask, gw;
		struct net_if_addr *ifaddr;
		struct in_addr pool_start;

		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &addr);
		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_NETMASK, &netmask);
		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_GW, &gw);
		ifaddr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		if (!ifaddr && !net_if_ipv4_addr_lookup(&addr, &iface)) {
			LOG_WRN("Failed to set IP address");
		}
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
		net_if_ipv4_set_gw(iface, &gw);

		/* Start DHCP server (pool starts at AP IP + 1) */
		pool_start.s_addr = htonl(ntohl(addr.s_addr) + 1);
		ret = net_dhcpv4_server_start(iface, &pool_start);
		if (ret && ret != -EALREADY) {
			LOG_WRN("DHCP server start failed: %d", ret);
		} else {
			printk("[WiFi] DHCP server started\n");
		}
	}

#ifdef CONFIG_NRF70_SR_COEX
	wifi_coex_configure();
#endif

	printk("[WiFi] AP started: SSID=%s ch=%d IP=%s\n",
	       ap_ssid, WIFI_AP_CHANNEL, CONFIG_NET_CONFIG_MY_IPV4_ADDR);
	printk("[WiFi] Password: %s\n", WIFI_AP_PASSWORD);
	return 0;
}

static int do_wifi_ap_stop(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface || !ap_started) {
		printk("AP not running\n");
		return 0;
	}

	net_dhcpv4_server_stop(iface);
	net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
	ap_started = false;
	net_if_down(iface);
	printk("[WiFi] AP stopped\n");
	return 0;
}

/* =========================================================================
 * Shell Commands
 * ========================================================================= */
static int cmd_wifi_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return do_wifi_ap_start();
}

static int cmd_wifi_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return do_wifi_ap_stop();
}

static int cmd_wifi_status(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_iface_status status;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "WiFi AP Status:");

	if (ap_started) {
		shell_print(sh, "  SSID: %s", ap_ssid);
		shell_print(sh, "  Password: %s", WIFI_AP_PASSWORD);
		shell_print(sh, "  Channel: %d (5GHz)", WIFI_AP_CHANNEL);
		shell_print(sh, "  IP: 192.168.4.1");
	} else {
		shell_print(sh, "  State: Stopped");
		shell_print(sh, "  Use 'wifi on' to start AP");
	}

	ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
		       &status, sizeof(status));
	if (ret == 0) {
		shell_print(sh, "  Interface state: %d", status.state);
	}

	return 0;
}

static int cmd_wifi_scan(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_default();
	static struct net_mgmt_event_callback scan_cb;
	int ret, band = 0;

	if (argc >= 2) {
		band = atoi(argv[1]);
		if (band < 0 || band > 2) {
			shell_print(sh, "Invalid band (0=all, 1=2.4G, 2=5G)");
			return -EINVAL;
		}
	}

	scan_result_count = 0;
	memset(scan_results, 0, sizeof(scan_results));
	k_sem_reset(&scan_done_sem);

	net_mgmt_init_event_callback(&scan_cb, scan_result_callback,
				     NET_EVENT_WIFI_SCAN_RESULT |
				     NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&scan_cb);

	struct wifi_scan_params params = {
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.bands = (band == 0) ? ((1 << WIFI_FREQ_BAND_2_4_GHZ) | (1 << WIFI_FREQ_BAND_5_GHZ)) :
			(band == 1) ? (1 << WIFI_FREQ_BAND_2_4_GHZ) : (1 << WIFI_FREQ_BAND_5_GHZ),
		.max_bss_cnt = 10,
	};

	shell_print(sh, "Scanning for networks...");

	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
	if (ret) {
		shell_print(sh, "Scan failed: %d", ret);
		net_mgmt_del_event_callback(&scan_cb);
		return ret;
	}

	ret = k_sem_take(&scan_done_sem, K_SECONDS(15));
	if (ret != 0) {
		shell_print(sh, "Scan timeout");
		net_mgmt_del_event_callback(&scan_cb);
		return -ETIMEDOUT;
	}

	net_mgmt_del_event_callback(&scan_cb);

	shell_print(sh, "Found %d networks:", scan_result_count);
	shell_print(sh, "%-4s %-20s %-6s %-4s", "CH", "SSID", "RSSI", "BAND");

	for (int i = 0; i < scan_result_count; i++) {
		struct wifi_scan_result *r = &scan_results[i];
		const char *band_str = (r->band == WIFI_FREQ_BAND_2_4_GHZ) ? "2.4G" : "5G";

		char ssid[21];
		snprintf(ssid, sizeof(ssid), "%.20s", r->ssid);

		shell_print(sh, "%-4d %-20s %-6d %-4s",
			   r->channel, ssid, r->rssi, band_str);
	}

	return 0;
}

/* =========================================================================
 * zperf UDP throughput test (from wifi_ble_coex sample)
 * ========================================================================= */
static K_SEM_DEFINE(zperf_done_sem, 0, 1);
static atomic_t zperf_sessions = ATOMIC_INIT(0);
static volatile bool zperf_error;
static char iperf_server_ip[32] = "192.168.4.10";

static void zperf_cb(enum zperf_status status,
		     struct zperf_results *result,
		     void *user_data)
{
	const struct shell *sh = (const struct shell *)user_data;

	switch (status) {
	case ZPERF_SESSION_STARTED:
		atomic_inc(&zperf_sessions);
		break;
	case ZPERF_SESSION_FINISHED:
		if (result && !zperf_error) {
			uint64_t bytes = (uint64_t)result->nb_packets_sent *
					 result->packet_size;
			uint64_t time_ms = result->client_time_in_us / 1000;
			uint32_t kbps = 0;

			if (result->client_time_in_us > 0) {
				kbps = (uint32_t)(
					((uint64_t)result->nb_packets_sent *
					 result->packet_size * 8ULL * 1000000ULL) /
					(result->client_time_in_us * 1024ULL));
			}
			shell_print(sh, "[WiFi iperf] pkts=%u lost=%u bytes=%llu time=%llums",
				    result->nb_packets_sent, result->nb_packets_lost,
				    bytes, time_ms);
			shell_print(sh, "[WiFi iperf] Throughput: %u kbps (%u.%03u Mbps)",
				    kbps, kbps / 1000, kbps % 1000);
		}
		if (atomic_dec(&zperf_sessions) <= 1) {
			k_sem_give(&zperf_done_sem);
		}
		break;
	case ZPERF_SESSION_ERROR:
		zperf_error = true;
		if (atomic_dec(&zperf_sessions) <= 1) {
			shell_print(sh, "[WiFi iperf] Test failed");
			k_sem_give(&zperf_done_sem);
		}
		break;
	default:
		break;
	}
}

static int cmd_udp_test(const struct shell *sh, size_t argc, char **argv)
{
	struct sockaddr_in addr;
	struct zperf_upload_params params;
	int ret;
	int duration_sec = IPERF_DURATION_SEC;
	uint32_t rate_kbps = 100000;

	if (!ap_started) {
		shell_print(sh, "WiFi AP not started. Use 'wifi on' first.");
		return -ENETDOWN;
	}

	if (argc >= 2) {
		strncpy(iperf_server_ip, argv[1], sizeof(iperf_server_ip) - 1);
	}
	if (argc >= 3) {
		duration_sec = atoi(argv[2]);
		if (duration_sec <= 0 || duration_sec > 3600) {
			shell_print(sh, "Invalid duration (1-3600 seconds)");
			return -EINVAL;
		}
	}
	if (argc >= 4) {
		rate_kbps = atoi(argv[3]);
		if (rate_kbps < 100 || rate_kbps > 1000000) {
			shell_print(sh, "Invalid rate (100-1000000 kbps)");
			return -EINVAL;
		}
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(IPERF_PORT);
	ret = inet_pton(AF_INET, iperf_server_ip, &addr.sin_addr);
	if (ret != 1) {
		shell_print(sh, "Invalid IP address");
		return -EINVAL;
	}

	memset(&params, 0, sizeof(params));
	params.duration_ms = duration_sec * 1000;
	params.packet_size = IPERF_PACKET_SIZE;
	params.rate_kbps = rate_kbps;
	memcpy(&params.peer_addr, &addr, sizeof(addr));

	shell_print(sh, "");
	shell_print(sh, "UDP throughput test (iperf compatible)");
	shell_print(sh, "Target: %s:%d", iperf_server_ip, IPERF_PORT);
	shell_print(sh, "Packet size: %u bytes", IPERF_PACKET_SIZE);
	shell_print(sh, "Duration: %d seconds", duration_sec);
	shell_print(sh, "Rate limit: %u kbps (%u Mbps)", rate_kbps, rate_kbps / 1000);
	shell_print(sh, "");
	shell_print(sh, "Run iperf server on your PC:");
	shell_print(sh, "  iperf -s -i 1 -u");
	shell_print(sh, "");

	zperf_error = false;
	ret = zperf_udp_upload_async(&params, zperf_cb, (void *)sh);
	if (ret) {
		shell_print(sh, "Failed to start test: %d", ret);
		return ret;
	}

	ret = k_sem_take(&zperf_done_sem, K_SECONDS(duration_sec + 30));
	if (ret != 0) {
		shell_print(sh, "Test timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wifi,
	SHELL_CMD(on, NULL, "Start WiFi AP", cmd_wifi_on),
	SHELL_CMD(off, NULL, "Stop WiFi AP", cmd_wifi_off),
	SHELL_CMD(scan, NULL, "Scan for networks [band: 0=all, 1=2.4G, 2=5G]", cmd_wifi_scan),
	SHELL_CMD(status, NULL, "Show WiFi AP status", cmd_wifi_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wifi, &sub_wifi, "WiFi AP commands", NULL);

SHELL_CMD_REGISTER(iperf, NULL, "UDP iperf throughput test [server_ip] [duration_sec] [rate_kbps]",
		   cmd_udp_test);

/* =========================================================================
 * Public API
 * ========================================================================= */
int wifi_init_and_connect(void)
{
	/* Generate AP SSID from chip ID */
	generate_ap_ssid();

	/* Register WiFi event callbacks */
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_DISABLE_RESULT |
				     NET_EVENT_WIFI_AP_STA_CONNECTED |
				     NET_EVENT_WIFI_AP_STA_DISCONNECTED);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	printk("WiFi initialized (AP mode)\n");
	printk("  SSID will be: %s\n", ap_ssid);
	printk("  Use 'wifi on' to start AP\n");

	return 0;
}

int wifi_start_throughput_test(void)
{
	return 0;
}

int wifi_run_test(void)
{
	return wifi_init_and_connect();
}
