/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * WiFi AP + iperf Throughput Test Sample for ReSpeaker Clip
 *
 * This sample combines WiFi AP mode with zperf client for network
 * throughput testing. The device acts as a WiFi hotspot and can
 * run iperf tests against a PC server connected to the AP.
 *
 * Features:
 * - Device acts as a WiFi AP (hotspot)
 * - SSID: "ClipAP_XXXX" (last 4 hex of chip ID)
 * - Password: "12345678"
	 * - No DHCP server; client must use static IP 192.168.4.10/24
 * - zperf client for UDP throughput testing (device -> PC server)
 * - Shell commands for AP control and iperf testing
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/zperf.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <nrfx_clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(wifi_ap_iperf, LOG_LEVEL_INF);

/* AP Configuration */
#define WIFI_AP_SSID_PREFIX "ClipAP_"
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 36
#define WIFI_AP_MAX_CLIENTS 1
#define WIFI_AP_REG_DOMAIN "US"

/* DHCP pool: 1 address, so client always gets this fixed IP */
#define DHCPV4_POOL_START "192.168.4.10"
#define STA_STATIC_IP DHCPV4_POOL_START

/* zperf default configuration */
#define IPERF_PORT 5001
#define IPERF_PACKET_SIZE 1400
#define IPERF_DURATION_SEC 10

/* Global state */
static bool ap_started = false;
static bool sta_connected = false;  /* at least one station connected */
static char ap_ssid[32];
static char iperf_server_ip[32] = STA_STATIC_IP;
static int iperf_server_port = IPERF_PORT;
static int iperf_duration = IPERF_DURATION_SEC;

/* Fallback SSID if chip ID read fails */
#define DEFAULT_AP_SSID "ClipAP_Test"

/**
 * @brief Generate AP SSID from chip ID
 */
static void generate_ap_ssid(void)
{
	uint8_t chip_id[16];
	ssize_t length;

	length = hwinfo_get_device_id(chip_id, sizeof(chip_id));

	if (length > 0) {
		/* Use last 4 bytes of chip ID for SSID suffix */
		uint32_t id_suffix = 0;
		int offset = length > 4 ? length - 4 : 0;

		for (int i = 0; i < 4 && (offset + i) < length; i++) {
			id_suffix = (id_suffix << 8) | chip_id[offset + i];
		}

		snprintf(ap_ssid, sizeof(ap_ssid), "%s%04X",
			 WIFI_AP_SSID_PREFIX, (unsigned int)(id_suffix & 0xFFFF));
	} else {
		strncpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid) - 1);
		ap_ssid[sizeof(ap_ssid) - 1] = '\0';
	}

	LOG_INF("AP SSID: %s", ap_ssid);
	LOG_INF("AP Password: " WIFI_AP_PASSWORD);
}

/**
 * @brief WiFi management event handler
 */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event,
				    struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		LOG_INF("AP enabled result");
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
	{
		struct wifi_ap_sta_info *sta =
			(struct wifi_ap_sta_info *)cb->info;
		sta_connected = true;
		if (sta) {
			LOG_INF("Station connected");
			printk("\n========================================\n");
			printk("Station connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
			       sta->mac[0], sta->mac[1], sta->mac[2],
			       sta->mac[3], sta->mac[4], sta->mac[5]);
			printk("========================================\n");
			printk("You can now run iperf tests.\n");
			printk("========================================\n\n");
		}
		break;
	}
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED:
	{
		struct wifi_ap_sta_info *sta =
			(struct wifi_ap_sta_info *)cb->info;
		sta_connected = false;
		if (sta) {
			LOG_INF("Station disconnected");
			printk("\nStation disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
			       sta->mac[0], sta->mac[1], sta->mac[2],
			       sta->mac[3], sta->mac[4], sta->mac[5]);
		}
		break;
	}
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("Layer 4 connected");
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_INF("Layer 4 disconnected");
		break;
	default:
		break;
	}
}

/**
 * @brief Check if WiFi AP is actually running (state == COMPLETED)
 * Mirrors is_wifi_connected() from wifi.c, adapted for AP mode.
 */
static bool is_ap_ready(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_iface_status status;
	int ret;

	ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
		       &status, sizeof(status));
	if (ret == 0 && status.iface_mode == WIFI_MODE_AP &&
	    status.state == WIFI_STATE_COMPLETED) {
		return true;
	}
	return false;
}

/**
 * @brief Set WiFi regulatory domain
 */
static int wifi_set_reg_domain(void)
{
	struct net_if *iface;
	struct wifi_reg_domain regd = {0};
	int ret;

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("Failed to get network interface");
		return -ENODEV;
	}

	regd.oper = WIFI_MGMT_SET;
	strncpy(regd.country_code, WIFI_AP_REG_DOMAIN, WIFI_COUNTRY_CODE_LEN + 1);

	ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &regd, sizeof(regd));
	if (ret) {
		LOG_ERR("Failed to set regulatory domain: %d", ret);
		return ret;
	}

	LOG_INF("Regulatory domain set to %s", WIFI_AP_REG_DOMAIN);
	printk("Regulatory domain: %s\n", WIFI_AP_REG_DOMAIN);

	return 0;
}

/**
 * @brief Configure DHCP server for AP mode
 */
static int configure_dhcp_server(void)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr pool_start;
	int ret;

	if (!iface) {
		return -ENODEV;
	}
	net_addr_pton(AF_INET, DHCPV4_POOL_START, &pool_start.s_addr);
	ret = net_dhcpv4_server_start(iface, &pool_start);
	if (ret == -EALREADY) {
		return 0;
	}
	if (ret < 0) {
		LOG_ERR("DHCPv4 server start failed: %d", ret);
		return ret;
	}
	LOG_INF("DHCP server started, client IP: " DHCPV4_POOL_START);
	return 0;
}

static void stop_dhcp_server(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface) {
		net_dhcpv4_server_stop(iface);
	}
}

/**
 * @brief Start WiFi AP
 */
static int wifi_ap_start(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params req;
	int ret;

	if (!iface) {
		LOG_ERR("No network interface found");
		return -ENODEV;
	}

	/* Check if AP is already started */
	if (ap_started) {
		LOG_INF("AP already started");
		return 0;
	}

	/* Set regulatory domain first (required for 5GHz) */
	ret = wifi_set_reg_domain();
	if (ret) {
		printk("Warning: Failed to set regulatory domain: %d\n", ret);
	}

	/* Fill AP configuration */
	memset(&req, 0, sizeof(req));

	req.ssid = (const uint8_t *)ap_ssid;
	req.ssid_length = strlen(ap_ssid);
	req.psk = (const uint8_t *)WIFI_AP_PASSWORD;
	req.psk_length = strlen(WIFI_AP_PASSWORD);
	req.channel = WIFI_AP_CHANNEL;
	req.security = WIFI_SECURITY_TYPE_PSK;
	req.mfp = WIFI_MFP_OPTIONAL;
	req.band = WIFI_FREQ_BAND_5_GHZ;

	/* Enable AP mode */
	printk("\n");
	printk("==========================================\n");
	printk("Starting WiFi AP (5GHz)\n");
	printk("==========================================\n");
	printk("SSID: %s\n", ap_ssid);
	printk("Password: %s\n", WIFI_AP_PASSWORD);
	printk("Channel: %d\n", WIFI_AP_CHANNEL);
	printk("Client static IP: %s/24, GW: 192.168.4.1\n", STA_STATIC_IP);
	printk("==========================================\n\n");

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &req, sizeof(req));
	if (ret) {
		LOG_ERR("AP enable failed: %d", ret);
		printk("Failed to start AP: %d\n", ret);
		return ret;
	}

	/* Wait a bit for AP to start */
	k_sleep(K_SECONDS(2));

	configure_dhcp_server();

	ap_started = true;
	LOG_INF("AP started successfully");
	printk("\nAP started! Waiting for connections...\n\n");

	return 0;
}

/**
 * @brief Stop WiFi AP
 */
static int wifi_ap_stop(void)
{
	struct net_if *iface = net_if_get_default();
	int ret;

	if (!iface) {
		return -ENODEV;
	}

	if (!ap_started) {
		printk("AP not started\n");
		return 0;
	}

	stop_dhcp_server();

	ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
	if (ret) {
		printk("Failed to stop AP: %d\n", ret);
		return ret;
	}

	ap_started = false;
	printk("AP stopped\n");
	LOG_INF("AP stopped");

	return 0;
}

/* zperf UDP throughput test - iperf compatible */
static K_SEM_DEFINE(zperf_done_sem, 0, 1);

/*
 * Session counter: zperf_udp_upload_async() can internally spawn multiple
 * sessions (e.g. one sender + one result-receiver socket). We track every
 * STARTED event and only release the semaphore once ALL sessions have
 * completed or errored, preventing dangling callbacks from interfering with
 * the shell after the command returns.
 */
static atomic_t zperf_sessions_active = ATOMIC_INIT(0);
static volatile bool zperf_any_error;

static void udp_upload_results_cb(enum zperf_status status,
				   struct zperf_results *result,
				   void *user_data)
{
	const struct shell *sh = (const struct shell *)user_data;

	switch (status) {
	case ZPERF_SESSION_STARTED:
		atomic_inc(&zperf_sessions_active);
		printk("zperf: Session started\n");
		break;
	case ZPERF_SESSION_FINISHED:
		printk("zperf: Session finished\n");
		if (result && !zperf_any_error) {
			uint32_t throughput_kbps = 0;
			uint64_t bytes_sent = result->nb_packets_sent * result->packet_size;
			uint64_t time_ms = result->client_time_in_us / 1000;

			if (result->client_time_in_us != 0) {
				throughput_kbps = (uint32_t)(
					((uint64_t)result->nb_packets_sent *
					 (uint64_t)result->packet_size * 8 *
					 1000000) /
					(result->client_time_in_us * 1024)
				);
			}

			shell_print(sh, "");
			shell_print(sh, "Test completed!");
			shell_print(sh, "  Packets sent:     %u", result->nb_packets_sent);
			shell_print(sh, "  Packets lost:     %u", result->nb_packets_lost);
			shell_print(sh, "  Packets received: %u", result->nb_packets_rcvd);
			shell_print(sh, "  Bytes sent:       %llu", bytes_sent);
			shell_print(sh, "  Time:             %llu ms", time_ms);
			shell_print(sh, "  Throughput:       %u kbps (%u.%03u Mbps)",
				   throughput_kbps,
				   throughput_kbps / 1000,
				   throughput_kbps % 1000);
		}
		/* Release semaphore only when the last active session completes */
		if (atomic_dec(&zperf_sessions_active) <= 1) {
			k_sem_give(&zperf_done_sem);
		}
		break;
	case ZPERF_SESSION_ERROR:
		printk("zperf: Session error\n");
		if (result) {
			printk("  Packet errors: %u\n", result->nb_packets_errors);
		}
		zperf_any_error = true;
		/* Release semaphore only when the last active session completes */
		if (atomic_dec(&zperf_sessions_active) <= 1) {
			shell_print(sh, "Test failed");
			k_sem_give(&zperf_done_sem);
		}
		break;
	case ZPERF_SESSION_PERIODIC_RESULT:
		break;
	default:
		printk("zperf: Unknown status: %d\n", status);
		break;
	}
}

/**
 * @brief Run UDP upload test (device -> server)
 *
 * Usage: iperf [server_ip] [duration_sec] [rate_kbps]
 */
static int cmd_udp_test(const struct shell *sh, size_t argc, char **argv)
{
	struct sockaddr_in addr;
	struct zperf_upload_params params;
	int ret;
	int duration_sec = iperf_duration;
	uint32_t packet_size = IPERF_PACKET_SIZE;
	uint32_t rate_kbps = 100000; /* Default 100 Mbps */

	if (!ap_started || !is_ap_ready()) {
		shell_print(sh, "Error: AP not started. Use 'wifi_ap start' first.");
		return -ENETDOWN;
	}

	// if (!sta_connected) {
	// 	shell_print(sh, "Error: No station connected to AP.");
	// 	shell_print(sh, "Connect a device to SSID: %s first.", ap_ssid);
	// 	return -ENOTCONN;
	// }

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
	addr.sin_port = htons(iperf_server_port);
	ret = inet_pton(AF_INET, iperf_server_ip, &addr.sin_addr);
	if (ret != 1) {
		shell_print(sh, "Invalid IP address");
		return -EINVAL;
	}

	memset(&params, 0, sizeof(params));
	params.duration_ms = duration_sec * 1000;
	params.packet_size = packet_size;
	params.rate_kbps = rate_kbps;
	memcpy(&params.peer_addr, &addr, sizeof(addr));

	shell_print(sh, "");
	shell_print(sh, "UDP throughput test (iperf compatible)");
	shell_print(sh, "Target: %s:%d", iperf_server_ip, iperf_server_port);
	shell_print(sh, "Packet size: %u bytes", packet_size);
	shell_print(sh, "Duration: %d seconds", duration_sec);
	shell_print(sh, "Rate limit: %u kbps (%.1f Mbps)", rate_kbps, rate_kbps / 1000.0);
	shell_print(sh, "");
	shell_print(sh, "Run iperf server on your PC:");
	shell_print(sh, "  iperf -s -i 1 -u");
	shell_print(sh, "");

	/* Reset state before starting test */
	zperf_any_error = false;
	atomic_set(&zperf_sessions_active, 0);
	k_sem_reset(&zperf_done_sem);

	ret = zperf_udp_upload_async(&params, udp_upload_results_cb, (void *)sh);
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

/**
 * @brief Shell command to start AP
 */
static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ret = wifi_ap_start();
	if (ret) {
		shell_print(sh, "Failed to start AP: %d", ret);
		return ret;
	}

	shell_print(sh, "AP started successfully");
	shell_print(sh, "SSID: %s", ap_ssid);
	shell_print(sh, "Password: %s", WIFI_AP_PASSWORD);
	shell_print(sh, "IP: 192.168.4.1");

	return 0;
}

/**
 * @brief Shell command to stop AP
 */
static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ret = wifi_ap_stop();
	if (ret) {
		shell_print(sh, "Failed to stop AP: %d", ret);
		return ret;
	}

	shell_print(sh, "AP stopped");
	return 0;
}

/**
 * @brief Shell command to get AP status
 */
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_iface_status status;
	int ret;

	ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
		       &status, sizeof(status));
	if (ret) {
		shell_print(sh, "Failed to get status: %d", ret);
		return ret;
	}

	shell_print(sh, "WiFi AP Status:");
	shell_print(sh, "  Mode: %s",
		   status.iface_mode == WIFI_MODE_AP ? "Access Point" :
		   status.iface_mode == WIFI_MODE_INFRA ? "Station" :
		   status.iface_mode == WIFI_MODE_IBSS ? "Ad-hoc" :
		   "Unknown");
	shell_print(sh, "  State: %s",
		   status.state == WIFI_STATE_COMPLETED ? "Running" :
		   status.state == WIFI_STATE_SCANNING ? "Scanning" :
		   "Idle");
	shell_print(sh, "  SSID: %s", status.ssid);
	shell_print(sh, "  Channel: %d", status.channel);
	shell_print(sh, "  Security: %s",
		   status.security == WIFI_SECURITY_TYPE_PSK ? "WPA2-PSK" :
		   status.security == WIFI_SECURITY_TYPE_NONE ? "Open" :
		   "Other");

	/* Show AP IP address */
	if (iface->config.ip.ipv4) {
		struct net_if_addr_ipv4 *ifaddr =
			&iface->config.ip.ipv4->unicast[0];
		char addr_str[NET_IPV4_ADDR_LEN];

		if (ifaddr->ipv4.addr_state == NET_ADDR_PREFERRED ||
		    ifaddr->ipv4.addr_state == NET_ADDR_TENTATIVE) {
			shell_print(sh, "  IP: %s",
				net_addr_ntop(AF_INET, &ifaddr->ipv4.address.in_addr,
					     addr_str, sizeof(addr_str)));
		}
	}

	if (!ap_started) {
		shell_print(sh, "\nAP not started. Use 'wifi_ap start' to start.");
	}

	return 0;
}

/* Shell commands for WiFi AP */
SHELL_STATIC_SUBCMD_SET_CREATE(wifi_ap_cmds,
	SHELL_CMD(start, NULL, "Start WiFi AP", cmd_start),
	SHELL_CMD(stop, NULL, "Stop WiFi AP", cmd_stop),
	SHELL_CMD(status, NULL, "Show AP status", cmd_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(wifi_ap, &wifi_ap_cmds, "WiFi AP commands", NULL);

SHELL_CMD_REGISTER(iperf, NULL, "UDP iperf throughput test [server_ip] [duration_sec] [rate_kbps]", cmd_udp_test);

/**
 * @brief Main application
 */
int main(void)
{
	int ret;

		/* Configure HF clock divider for optimal WiFi/BLE performance */
#ifdef CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
#endif

	LOG_INF("ReSpeaker Clip WiFi AP + iperf Sample");
	LOG_INF("======================================");

	printk("\n");
	printk("================================================\n");
	printk("   ReSpeaker Clip WiFi AP + iperf Sample\n");
	printk("================================================\n\n");

	/* Generate AP SSID */
	generate_ap_ssid();

	/* Set up management event callback */
	static struct net_mgmt_event_callback mgmt_cb;
	net_mgmt_init_event_callback(&mgmt_cb, wifi_mgmt_event_handler,
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_STA_CONNECTED |
				     NET_EVENT_WIFI_AP_STA_DISCONNECTED |
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&mgmt_cb);

	/* Wait for network interface to be ready */
	k_sleep(K_SECONDS(2));

	/* Auto-start AP */
	ret = wifi_ap_start();
	if (ret) {
		printk("Failed to start AP: %d\n", ret);
		printk("You can start AP manually using: wifi_ap start\n");
	} else {
		printk("\nWiFi AP is running!\n");
		printk("Connect your PC to SSID: %s\n", ap_ssid);
		printk("Password: %s\n", WIFI_AP_PASSWORD);
		printk("Your PC will get IP: 192.168.4.x\n\n");
	}

	printk("Shell commands:\n");
	printk("  wifi_ap start                    - Start WiFi AP\n");
	printk("  wifi_ap stop                     - Stop WiFi AP\n");
	printk("  wifi_ap status                   - Show AP status\n");
	printk("  iperf [server_ip] [duration] [rate] - UDP iperf test\n\n");

	printk("Test procedure:\n");
	printk("1. Connect PC to WiFi AP: %s\n", ap_ssid);
	printk("2. Check PC IP address (should be 192.168.4.x)\n");
	printk("3. Start iperf server on PC:\n");
	printk("   iperf -s -i 1 -u\n");
	printk("4. Run iperf from device:\n");
	printk("   iperf <PC_IP> [duration_sec] [rate_kbps]\n\n");

	return 0;
}
