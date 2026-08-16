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
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/random/random.h>
#include <stdio.h>
#include <string.h>
#include <net/wifi_ready.h>
#include "wifi.h"
#include "wifi_udp.h"
#include "transport_udp.h"
#include "config.h"
#include "config.h"
#include "clip_event.h"
#include "ble.h"
#include "transfer.h"


#ifdef CONFIG_NRF70_SR_COEX
#include <coex.h>
#endif

LOG_MODULE_REGISTER(wifi, CONFIG_CLIP_LOG_LEVEL);

#define WIFI_AP_MGMT_EVENTS (NET_EVENT_WIFI_AP_ENABLE_RESULT |  \
							 NET_EVENT_WIFI_AP_DISABLE_RESULT | \
							 NET_EVENT_WIFI_AP_STA_CONNECTED |  \
							 NET_EVENT_WIFI_AP_STA_DISCONNECTED)

/* Station mode: joining someone else's network. Distinct from the AP events
 * above, which are about clients joining *our* network. */
/* NOTE: the station path logs at WRN, not INF, on purpose. prj.conf compiles
 * the app at CLIP_LOG_LEVEL_WRN to make room for the nRF70 firmware embedded in
 * the image, and LOG_INF calls are compiled out entirely — runtime filtering
 * cannot bring them back. Connection progress is the one thing worth keeping
 * visible while the WiFi work is in flight. Revert to INF once a J-Link lets us
 * relocate the firmware patch again and reclaim the ~87KB.
 */
#define WIFI_STA_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
							  NET_EVENT_WIFI_DISCONNECT_RESULT)

static char ap_ssid[32] = "ClipAP_Test";
static bool ap_client_connected;
static bool ap_running;
static bool wifi_ready;

/* Station-mode state. sta_associated means the link layer is up; sta_ip is only
 * populated once DHCP hands us a lease, which is what callers actually need. */
static bool sta_associated;
static char sta_ip[NET_IPV4_ADDR_LEN];
static int sta_fail_reason;
static char sta_fail_text[32];

/* Reconnection state. sta_want_connected separates "the link dropped" from
 * "the user asked us to disconnect" — without it, AT+STA=off would fight an
 * automatic retry forever. */
static bool sta_want_connected;
static uint32_t sta_backoff_ms;

/* Cuando empezo la racha sin red. 0 = hay red, o todavia no se ha intentado. */
static int64_t sta_offline_since;

/* wifi_sta_on() blocks for up to 35s waiting on association + DHCP. That must
 * not run on the system workqueue — a watchdog guards it, and the display and
 * transfer paths queue work there too. Hence a dedicated queue. */
/* 6KB, not the 2KB this started with. net_mgmt() from here descends into the
 * WPA supplicant, which the project gives 10KB in its own thread — 2KB was
 * wishful thinking, and with STACK_SENTINEL and THREAD_MONITOR both off a
 * blown stack corrupts memory silently instead of faulting. The device died
 * ~25s into every association attempt, right when this path runs deepest. */
static K_THREAD_STACK_DEFINE(sta_work_stack, 6144);
static struct k_work_q sta_work_q;
static bool sta_work_q_started;

/* Declared here because the disconnect event handler below schedules it. */
static void sta_reconnect_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sta_reconnect_work, sta_reconnect_work_handler);
static void sta_schedule_reconnect(void);

static struct net_mgmt_event_callback wifi_mgmt_cb;
static struct net_mgmt_event_callback wifi_sta_cb;
static struct net_mgmt_event_callback ipv4_cb;
static K_SEM_DEFINE(wifi_ready_sem, 0, 1);
static K_SEM_DEFINE(ap_enabled_sem, 0, 1);
static K_SEM_DEFINE(ap_disabled_sem, 0, 1);
static K_SEM_DEFINE(sta_connected_sem, 0, 1);
static K_SEM_DEFINE(sta_got_ip_sem, 0, 1);

/* MAC estable derivada del chip id, fijada antes de levantar la interfaz.
 *
 * El OTP del nRF7002 de estas unidades no tiene MAC grabada, y el respaldo
 * (WIFI_RANDOM_MAC_ADDRESS) genera una distinta en cada arranque: en el panel
 * el mismo device aparece con otra MAC tras cada reinicio, y una red de
 * tienda con lista blanca tendria que dar de alta una MAC nueva cada dia.
 *
 * El driver del nRF70 solo va a buscar la MAC (OTP/fija/aleatoria) si la
 * interfaz NO tiene ya una valida (nrf_wifi_if_start_zep), asi que basta con
 * fijarla aqui, con la interfaz aun abajo. Derivada del chip id es unica por
 * device y la misma en cada arranque. El primer octeto 0xB2 marca
 * locally-administered + unicast (bits U/L=1, I/G=0) — no es un OUI de nadie
 * y no puede chocar con hardware real.
 *
 * Ojo: esta MAC tiene prioridad sobre cualquier otra fuente mientras se fije
 * antes de levantar la interfaz. El dia que la MAC de fabrica se grabe en el
 * OTP (tests/otp), hay que RETIRAR esta funcion para que el driver vuelva a
 * leer el OTP. */
static void wifi_apply_stable_mac(struct net_if *iface)
{
	static uint8_t mac[6];
	uint8_t chip_id[8];
	ssize_t len;

	if (net_if_is_admin_up(iface)) {
		return; /* solo puede fijarse con la interfaz abajo */
	}

	len = hwinfo_get_device_id(chip_id, sizeof(chip_id));
	if (len < 6) {
		LOG_WRN("Sin chip id (%d): la MAC queda aleatoria por arranque",
			(int)len);
		return;
	}

	mac[0] = 0xB2; /* B de B-Pin; U/L=1 (local), I/G=0 (unicast) */
	memcpy(&mac[1], &chip_id[len - 5], 5);

	if (net_if_set_link_addr(iface, mac, sizeof(mac), NET_LINK_ETHERNET)) {
		LOG_WRN("No se pudo fijar la MAC estable");
		return;
	}
	LOG_INF("MAC estable %02X:%02X:%02X:%02X:%02X:%02X (del chip id)",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (ap_running && !ap_client_connected) {
		LOG_INF("WiFi timeout, auto-off");
		clip_post_event(CLIP_EVENT_WIFI_OFF);
	}
}

static K_WORK_DELAYABLE_DEFINE(wifi_timeout_work, wifi_timeout_handler);

/* Work for transfer cancel on STA disconnect (deferred to avoid stack issues) */
static void wifi_transfer_cancel_work_handler(struct k_work *work);
static K_WORK_DEFINE(wifi_transfer_cancel_work, wifi_transfer_cancel_work_handler);

static void wifi_transfer_cancel_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (transfer_is_active()) {
		transfer_cancel();
	}
}

static void schedule_wifi_timeout(void)
{
	if (CONFIG_CLIP_WIFI_TIMEOUT_MS > 0) {
		k_work_reschedule(&wifi_timeout_work, K_MSEC(CONFIG_CLIP_WIFI_TIMEOUT_MS));
	}
}

static void cancel_wifi_timeout(void)
{
	k_work_cancel_delayable(&wifi_timeout_work);
}

#ifdef CONFIG_NRF70_SR_COEX
static void wifi_coex_configure(bool is_5ghz)
{
	bool sep = IS_ENABLED(CONFIG_COEX_SEP_ANTENNAS);
	bool ble = IS_ENABLED(CONFIG_SR_PROTOCOL_BLE);
	enum nrf_wifi_pta_wlan_op_band band = is_5ghz ?
		NRF_WIFI_PTA_WLAN_OP_BAND_5_GHZ : NRF_WIFI_PTA_WLAN_OP_BAND_2_4_GHZ;
	int ret;

	ret = nrf_wifi_coex_config_non_pta(sep, ble);
	if (ret)
	{
		LOG_WRN("Coex non-PTA config failed: %d", ret);
	}
	ret = nrf_wifi_coex_config_pta(band, sep, ble);
	if (ret)
	{
		LOG_WRN("Coex PTA config failed: %d", ret);
	}
	LOG_INF("coex PTA (%s sep=%d ble=%d)", is_5ghz ? "5GHz" : "2.4GHz", sep, ble);
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
		ap_client_connected = false;
		k_sem_give(&ap_disabled_sem);
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
		ap_client_connected = true;
		cancel_wifi_timeout();
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
		ap_client_connected = false;
		schedule_wifi_timeout();
		transport_udp_update_active(false); /* Notify transport of disconnect */
		k_work_submit(&wifi_transfer_cancel_work);
		break;
	}
	default:
		break;
	}
}

static void wifi_sta_event_handler(struct net_mgmt_event_callback *cb,
								   uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	switch (mgmt_event)
	{
	case NET_EVENT_WIFI_CONNECT_RESULT:
	{
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status->status)
		{
			/* Keep the reason around: the caller reports it over BLE and
			 * AT+STA? serves it later. "failed" alone is unactionable —
			 * a wrong passphrase and an out-of-range AP look identical. */
			sta_fail_reason = status->status;
			snprintf(sta_fail_text, sizeof(sta_fail_text), "%s",
					 wifi_conn_status_txt(status->status));
			LOG_ERR("STA connect failed: %s (%d)", sta_fail_text, status->status);
			sta_associated = false;
		}
		else
		{
			LOG_WRN("STA associated to %s", config_get_sta_ssid());
			sta_associated = true;
			sta_fail_reason = 0;
			sta_fail_text[0] = '\0';
		}
		k_sem_give(&sta_connected_sem);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("STA disconnected");
		sta_associated = false;
		sta_ip[0] = '\0';
		ble_notify_event("sta", "off");
		/* An unexpected drop schedules a retry; a deliberate AT+STA=off does
		 * not, because wifi_sta_off() clears sta_want_connected first. Without
		 * this the device stayed silently off the network until someone power
		 * cycled it — observed over a 10h soak test. */
		sta_schedule_reconnect();
		/* Same treatment as an AP client vanishing: a transfer in flight has
		 * nowhere to go. */
		k_work_submit(&wifi_transfer_cancel_work);
		break;
	default:
		break;
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
							   uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	/* Only meaningful in station mode — in AP mode we assign the address
	 * ourselves and already know what it is. */
	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD || !sta_associated)
	{
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++)
	{
		struct net_if_addr *if_addr = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (!if_addr->is_used || if_addr->addr_type != NET_ADDR_DHCP)
		{
			continue;
		}

		net_addr_ntop(AF_INET, &if_addr->address.in_addr, sta_ip, sizeof(sta_ip));
		LOG_WRN("STA got IP %s", sta_ip);
		ble_notify_event("sta", sta_ip);
		k_sem_give(&sta_got_ip_sem);
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

	/* Station-mode association events, and the DHCP lease that follows */
	net_mgmt_init_event_callback(&wifi_sta_cb, wifi_sta_event_handler,
								 WIFI_STA_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_sta_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler,
								 NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	/* Queue for the blocking STA connect (see wifi_sta_connect_async) */
	k_work_queue_init(&sta_work_q);
	k_work_queue_start(&sta_work_q, sta_work_stack,
					   K_THREAD_STACK_SIZEOF(sta_work_stack),
					   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&sta_work_q.thread, "sta_connect");
	sta_work_q_started = true;

	/* Register WiFi ready callback */
	iface = net_if_get_first_wifi();
	if (iface)
	{
		cb.wifi_ready_cb = wifi_ready_callback;
		ret = register_wifi_ready_callback(cb, iface);
		if (ret)
		{
			LOG_WRN("wifi_ready cb reg failed %d", ret);
		}
	}

	LOG_INF("WiFi module initialized");


	return 0;
}

static int wifi_set_reg_domain(struct net_if *iface)
{
	struct wifi_reg_domain regd = {0};
	int ret;

	regd.oper = WIFI_MGMT_SET;
	strncpy(regd.country_code, config_get_wifi_reg_domain(), WIFI_COUNTRY_CODE_LEN);

	ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, iface, &regd, sizeof(regd));
	if (ret)
	{
		LOG_WRN("Failed to set reg domain: %d", ret);
	}

	return ret;
}

#define WIFI_AP_ENABLE_RETRIES 2

static int wifi_enable_ap(struct net_if *iface)
{
	struct wifi_connect_req_params req;
	int ret;

	memset(&req, 0, sizeof(req));
	req.ssid = (const uint8_t *)ap_ssid;
	req.ssid_length = strlen(ap_ssid);
	req.psk = (const uint8_t *)config_get_wifi_password();
	req.psk_length = strlen(config_get_wifi_password());
	uint8_t channel = config_get_wifi_channel();
	bool is_5ghz = (channel >= 36);

	req.channel = channel;
	req.security = WIFI_SECURITY_TYPE_PSK;
	req.mfp = WIFI_MFP_OPTIONAL;
	req.band = is_5ghz ? WIFI_FREQ_BAND_5_GHZ : WIFI_FREQ_BAND_2_4_GHZ;

	for (int attempt = 0; attempt <= WIFI_AP_ENABLE_RETRIES; attempt++) {
		k_sem_reset(&ap_enabled_sem);

		ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &req, sizeof(req));
		if (ret) {
			LOG_ERR("AP enable fail %d (try %d)", ret, attempt);
			continue;
		}

		ret = k_sem_take(&ap_enabled_sem, K_SECONDS(5));
		if (ret) {
			LOG_WRN("AP enable timeout (attempt %d)", attempt);
			continue;
		}

		if (!ap_running) {
			LOG_ERR("AP enable rejected (attempt %d)", attempt);
			continue;
		}

	return 0;
	}

	return -ETIMEDOUT;
}

static int wifi_start_dhcp_server(struct net_if *iface)
{
	struct in_addr pool_start;
	int ret;

	/* Derive pool start from server IP (192.168.4.1 -> 192.168.4.2) */
	net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &pool_start);
	pool_start.s_addr = htonl(ntohl(pool_start.s_addr) + 1);

	ret = net_dhcpv4_server_start(iface, &pool_start);
	if (ret && ret != -EALREADY)
	{
		LOG_WRN("DHCP server start failed: %d", ret);
	}
	else
	{
		LOG_INF("DHCP server started");
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

	/* Bring up interface if not already up */
	if (!net_if_is_admin_up(iface))
	{
		LOG_INF("Interface is down, bringing up");
		wifi_apply_stable_mac(iface);
		ret = net_if_up(iface);
		if (ret)
		{
			LOG_ERR("net_if_up failed: %d", ret);
			return ret;
		}

		/* Wait for WiFi driver ready */
		if (!wifi_ready) {
			ret = k_sem_take(&wifi_ready_sem, K_SECONDS(3));
			if (ret) {
				LOG_ERR("WiFi ready timeout");
				net_if_down(iface);
				return -ETIMEDOUT;
			}
		}
	}

	/* Set regulatory domain */
	wifi_set_reg_domain(iface);

	/* Enable AP mode */
	ret = wifi_enable_ap(iface);
	if (ret)
	{
		LOG_WRN("wifi_on: AP failed, cleaning up");
		net_if_down(iface);
		ap_running = false;
		return ret;
	}

#ifdef CONFIG_NRF70_SR_COEX
	/* Configure coexistence AFTER AP is enabled. Do NOT configure before
	 * AP enable — if nRF70 is in a bad state, coex config hangs forever.
	 * The band follows the channel (>=36 is 5GHz), same as wifi_enable_ap(). */
	wifi_coex_configure(config_get_wifi_channel() >= 36);
#endif

	/* Configure static IP from Kconfig macros */
	{
		struct in_addr addr, netmask, gw;
		struct net_if_addr *ifaddr;
		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &addr);
		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_NETMASK, &netmask);
		net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_GW, &gw);
		ifaddr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		if (!ifaddr && !net_if_ipv4_addr_lookup(&addr, &iface))
		{
			LOG_WRN("Failed to set IP address");
		}
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
		net_if_ipv4_set_gw(iface, &gw);
	}

	/* Start DHCP server */
	ret = wifi_start_dhcp_server(iface);
	if (ret) {
		LOG_WRN("DHCP server start failed: %d", ret);
	}

	/* Start UDP server */
	ret = wifi_udp_start();
	if (ret)
	{
		LOG_WRN("UDP server start failed: %d", ret);
		/* Continue anyway - UDP is optional */
	}

	/* Start auto-off timeout */
	schedule_wifi_timeout();

	LOG_INF("AP up: %s ch=%d %s:%d",
			ap_ssid, WIFI_AP_CHANNEL, CONFIG_NET_CONFIG_MY_IPV4_ADDR, WIFI_AP_UDP_PORT);

	ble_notify_event("wifi", "on");

	return 0;
}

int wifi_off(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	cancel_wifi_timeout();

	if (!ap_running) {
		return 0;
	}

	if (!iface)
	{
		return -ENODEV;
	}

	/* Stop UDP server */
	wifi_udp_stop();

	/* Stop DHCP server */
	net_dhcpv4_server_stop(iface);

	/* Disable AP mode — wait for the result before powering down,
	 * otherwise a fast re-enable (wifi_on) can race the still-pending
	 * disable and fail. */
	k_sem_reset(&ap_disabled_sem);
	net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);
	ret = k_sem_take(&ap_disabled_sem, K_SECONDS(3));
	if (ret) {
		LOG_WRN("WiFi AP disable timeout: %d", ret);
	}
	ap_running = false;
	ap_client_connected = false;

	/* Reset WiFi ready state so next wifi_on() waits properly */
	wifi_ready = false;
	k_sem_reset(&wifi_ready_sem);

#ifdef CONFIG_NRF70_SR_COEX
	/* Reset coexistence hardware */
	nrf_wifi_coex_hw_reset();
#endif

	/* Power down interface to save power (required when CONFIG_NRF_WIFI_IF_AUTO_START=n) */
	if (net_if_is_admin_up(iface))
	{
		LOG_INF("wifi_off: calling net_if_down");
		ret = net_if_down(iface);
		if (ret)
		{
			LOG_WRN("net_if_down failed: %d", ret);
		}
		else
		{
			LOG_INF("wifi_off: net_if_down done");
		}
	}

	ble_notify_event("wifi", "off");

	return 0;
}

/* Association can take a few seconds on a busy 2.4GHz band; DHCP is usually
 * fast but a loaded home router occasionally dawdles. */
#define STA_CONNECT_TIMEOUT_SEC 20
#define STA_DHCP_TIMEOUT_SEC    15

/* Reconnection backoff. Starts short so a blip recovers quickly, then backs off
 * so a genuinely absent AP is not hammered.
 *
 * The jitter matters more than it looks: when an AP reboots, every device in
 * the store loses the link at the same instant. Retrying on a fixed schedule
 * makes them stampede the AP together, over and over. Spreading each retry over
 * a random window desynchronises the fleet. */
#define STA_RECONNECT_MIN_MS   5000
#define STA_RECONNECT_MAX_MS   120000

int wifi_sta_on(void)
{
	struct net_if *iface;
	struct wifi_connect_req_params req;
	const char *ssid = config_get_sta_ssid();
	const char *psk = config_get_sta_psk();
	int ret;

	if (!config_has_sta_credentials())
	{
		LOG_ERR("No STA credentials — set them with AT+STACFG first");
		return -ENOENT;
	}

	if (sta_associated)
	{
		LOG_DBG("STA already connected");
		return 0;
	}

	/* From here on, an unexpected drop should bring the link back by itself. */
	sta_want_connected = true;

	/* AP and STA cannot coexist: the interface only holds one IPv4 address
	 * (NET_IF_UNICAST_IPV4_ADDR_COUNT=1), and the AP owns a static one. */
	if (ap_running)
	{
		LOG_INF("Stopping AP before joining a network");
		wifi_off();
	}

	iface = net_if_get_first_wifi();
	if (!iface)
	{
		LOG_ERR("No WiFi interface");
		return -ENODEV;
	}

	if (!net_if_is_admin_up(iface))
	{
		wifi_apply_stable_mac(iface);
		ret = net_if_up(iface);
		if (ret)
		{
			LOG_ERR("net_if_up failed: %d", ret);
			return ret;
		}

		if (!wifi_ready)
		{
			/* 10s y no 3: el PRIMER arranque del RPU tras un reinicio carga
			 * el parche del nRF70 (87KB) desde la flash externa en trozos de
			 * 8KB, y con 3s el primer intento moria aqui con ETIMEDOUT — el
			 * segundo intento siempre funcionaba porque la radio ya estaba
			 * arriba. Era la mitad del "autoconnect no se dispara": el unico
			 * intento del arranque caia justo en este timeout. */
			ret = k_sem_take(&wifi_ready_sem, K_SECONDS(10));
			if (ret)
			{
				LOG_ERR("WiFi ready timeout");
				net_if_down(iface);
				return -ETIMEDOUT;
			}
		}
	}

	wifi_set_reg_domain(iface);

	memset(&req, 0, sizeof(req));
	req.ssid = (const uint8_t *)ssid;
	req.ssid_length = strlen(ssid);
	req.channel = WIFI_CHANNEL_ANY;
	req.band = WIFI_FREQ_BAND_UNKNOWN; /* let the supplicant scan both bands */
	req.mfp = WIFI_MFP_OPTIONAL;

	if (psk[0] != '\0')
	{
		req.psk = (const uint8_t *)psk;
		req.psk_length = strlen(psk);
		/* WPA3/SAE is compiled out (see prj.conf), so PSK is all we can do */
		req.security = WIFI_SECURITY_TYPE_PSK;
	}
	else
	{
		req.security = WIFI_SECURITY_TYPE_NONE;
	}

	k_sem_reset(&sta_connected_sem);
	k_sem_reset(&sta_got_ip_sem);
	sta_fail_reason = 0;
	sta_fail_text[0] = '\0';

	LOG_WRN("STA connecting to '%s' (%s, reg=%s)", ssid,
			psk[0] ? "WPA2-PSK" : "open", config_get_wifi_reg_domain());

	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &req, sizeof(req));
	if (ret)
	{
		LOG_ERR("STA connect request failed: %d", ret);
		net_if_down(iface);
		return ret;
	}

	ret = k_sem_take(&sta_connected_sem, K_SECONDS(STA_CONNECT_TIMEOUT_SEC));
	if (ret || !sta_associated)
	{
		LOG_ERR("STA association to '%s' failed (reason: %s)", ssid,
				sta_fail_text[0] ? sta_fail_text : "timeout, no result event");

		/* Leave the interface UP. Tearing it down here — DISCONNECT
		 * immediately followed by net_if_down() while the supplicant is still
		 * mid-scan — took the whole device down every time, USB included, and
		 * with it the deferred log messages that would have explained why the
		 * association failed. A radio left powered is a far smaller problem
		 * than an unreachable device; AT+STA=off brings it down cleanly once
		 * the supplicant has settled. */
		net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);

		return ret ? -ETIMEDOUT : -ECONNREFUSED;
	}

#ifdef CONFIG_NRF70_SR_COEX
	/* Configure coex only after association, same ordering constraint as the
	 * AP path: doing it earlier can hang if the nRF70 is in a bad state. */
	wifi_coex_configure(false);
#endif

	/* Free the single IPv4 slot before DHCP asks for it. net_config assigns
	 * the AP's static CONFIG_NET_CONFIG_MY_IPV4_ADDR at boot, and
	 * NET_IF_UNICAST_IPV4_ADDR_COUNT is 1 — leave it in place and the lease
	 * has nowhere to land, which looks exactly like "no DHCP server". */
	{
		struct in_addr ap_addr;

		if (net_addr_pton(AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &ap_addr) == 0 &&
			net_if_ipv4_addr_rm(iface, &ap_addr))
		{
			LOG_WRN("Released the AP static address before DHCP");
		}
	}

	/* The DHCP client is started by the network stack on link-up; we only wait
	 * for the lease so callers get an interface that is actually usable. */
	net_dhcpv4_start(iface);

	ret = k_sem_take(&sta_got_ip_sem, K_SECONDS(STA_DHCP_TIMEOUT_SEC));
	if (ret)
	{
		LOG_WRN("No DHCP lease after %ds — link is up but unusable",
				STA_DHCP_TIMEOUT_SEC);
		return -ETIMEDOUT;
	}

	/* The UDP server is not just for the AP: transfers are bidirectional, and
	 * without it the FILE_ACK frames that drive retransmission never arrive. */
	ret = wifi_udp_start();
	if (ret)
	{
		LOG_WRN("UDP server start failed: %d", ret);
		/* The link is still usable — transfers are what suffer */
	}

	LOG_WRN("STA up: %s -> %s", ssid, sta_ip);

	return 0;
}

int wifi_sta_off(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	int ret;

	/* Deliberate disconnect: stop retrying before touching the link, or the
	 * work item would race us and reconnect what the user just turned off. */
	sta_want_connected = false;
	sta_backoff_ms = 0;
	sta_offline_since = 0;
	k_work_cancel_delayable(&sta_reconnect_work);

	if (!sta_associated)
	{
		return 0;
	}
	if (!iface)
	{
		return -ENODEV;
	}

	wifi_udp_stop();
	net_dhcpv4_stop(iface);

	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	if (ret)
	{
		LOG_WRN("STA disconnect failed: %d", ret);
	}

	sta_associated = false;
	sta_ip[0] = '\0';
	wifi_ready = false;
	k_sem_reset(&wifi_ready_sem);

	/* Give the supplicant a moment to unwind before pulling the interface
	 * down. Doing both back to back is what wedged the device on the failed
	 * association path. */
	k_msleep(500);

#ifdef CONFIG_NRF70_SR_COEX
	nrf_wifi_coex_hw_reset();
#endif

	if (net_if_is_admin_up(iface))
	{
		net_if_down(iface);
	}

	ble_notify_event("sta", "off");

	return 0;
}

static void sta_schedule_reconnect(void)
{
	uint32_t delay, jitter;

	if (!sta_want_connected || !sta_work_q_started)
	{
		return;
	}

	if (sta_backoff_ms == 0)
	{
		sta_backoff_ms = STA_RECONNECT_MIN_MS;
	}

	if (sta_offline_since == 0)
	{
		sta_offline_since = k_uptime_get();
	}

	/* Up to +50% of the current backoff, so a fleet that lost the same AP does
	 * not retry in lockstep. */
	jitter = sys_rand32_get() % (sta_backoff_ms / 2 + 1);
	delay = sta_backoff_ms + jitter;

	LOG_WRN("STA reconnect in %u ms", delay);
	k_work_schedule_for_queue(&sta_work_q, &sta_reconnect_work, K_MSEC(delay));

	sta_backoff_ms *= 2;
	if (sta_backoff_ms > STA_RECONNECT_MAX_MS)
	{
		sta_backoff_ms = STA_RECONNECT_MAX_MS;
	}
}

static void sta_reconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!sta_want_connected || sta_associated)
	{
		return;
	}

	if (wifi_sta_on() == 0)
	{
		sta_backoff_ms = STA_RECONNECT_MIN_MS; /* recovered — reset the ramp */
		return;
	}

	sta_schedule_reconnect();
}

static void sta_connect_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	/* Bringing the radio up allocates the nRF70 heaps (60KB between data and
	 * ctrl). With static RAM already at ~96%, doing that while the rest of the
	 * system is still initialising wedged the boot animation — hence the delay
	 * the autoconnect path schedules before landing here. */

	ret = wifi_sta_on();
	if (ret)
	{
		/* Al log antes que a BLE. Esto solo avisaba por ble_notify_event, de
		 * modo que un arranque que no consigue red no dejaba rastro en ninguna
		 * parte salvo que hubiera un telefono emparejado escuchando en ese
		 * instante. Con BLE retirado no lo veria nadie: el device queda mudo y
		 * en el panel parece apagado, sin explicacion.
		 *
		 * A WRN para que sobreviva al backend de fichero de la SD, que es lo
		 * unico que queda en una unidad en tienda. */
		LOG_WRN("Autoconnect a '%s' fallo: %s (%d)", config_get_sta_ssid(),
				sta_fail_text[0] ? sta_fail_text : "sin motivo del supplicant",
				ret);

		/* Y REINTENTAR, no rendirse: este handler era de un solo tiro y la
		 * maquinaria de reconexion solo se activaba tras una DESconexion —
		 * un primer intento fallido en el arranque dejaba el device fuera de
		 * la red para siempre, mudo, hasta un AT+STA=on por cable. Un device
		 * de tienda tiene que insistir solo; la rampa con jitter ya existe
		 * (sta_schedule_reconnect) y sta_want_connected quedo puesto por
		 * wifi_sta_on(). */
		sta_schedule_reconnect();

		/* Report the reason, not just the fact. -ETIMEDOUT after association
		 * means DHCP never answered, which is a different problem entirely
		 * from the AP refusing us. */
		if (sta_fail_text[0] != '\0')
		{
			ble_notify_event("sta", sta_fail_text);
		}
		else if (ret == -ETIMEDOUT && sta_associated)
		{
			ble_notify_event("sta", "no-dhcp");
		}
		else if (ret == -ETIMEDOUT)
		{
			ble_notify_event("sta", "no-response");
		}
		else
		{
			/* Carry the errno: an empty fail reason means the request never
			 * reached the supplicant, and the number is the only clue. */
			char buf[24];

			snprintf(buf, sizeof(buf), "failed:%d", ret);
			ble_notify_event("sta", buf);
		}
	}
}

static K_WORK_DELAYABLE_DEFINE(sta_connect_work, sta_connect_work_handler);

int wifi_sta_connect_async_delayed(uint32_t delay_ms)
{
	if (!config_has_sta_credentials())
	{
		return -ENOENT;
	}
	if (!sta_work_q_started)
	{
		return -EAGAIN;
	}
	if (sta_associated)
	{
		return -EALREADY;
	}

	return k_work_schedule_for_queue(&sta_work_q, &sta_connect_work,
									 K_MSEC(delay_ms)) < 0 ? -EBUSY : 0;
}

int wifi_sta_connect_async(void)
{
	return wifi_sta_connect_async_delayed(0);
}

int wifi_sta_get_rssi(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_iface_status st = {0};

	if (!iface || !sta_associated) {
		return 0;
	}

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st))) {
		return 0;
	}

	return st.rssi;
}

int wifi_get_mac(char *buf, size_t len)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct net_linkaddr *la;

	if (!buf || len < 18) {
		return -EINVAL;
	}
	if (!iface) {
		return -ENODEV;
	}

	la = net_if_get_link_addr(iface);
	if (!la || la->len != 6) {
		return -ENODEV;
	}

	snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
		 la->addr[0], la->addr[1], la->addr[2],
		 la->addr[3], la->addr[4], la->addr[5]);
	return 0;
}

uint32_t wifi_sta_offline_minutes(void)
{
	if (sta_offline_since == 0)
	{
		return 0;
	}

	return (uint32_t)((k_uptime_get() - sta_offline_since) / 60000);
}

bool wifi_sta_is_connected(void)
{
	return sta_associated && sta_ip[0] != '\0';
}

const char *wifi_sta_get_ip(void)
{
	return sta_ip;
}

const char *wifi_sta_get_fail_reason(void)
{
	return sta_fail_text;
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
	return config_get_wifi_password();
}

const char *wifi_get_ip_address(void)
{
	return CONFIG_NET_CONFIG_MY_IPV4_ADDR;
}

bool wifi_ap_has_client(void)
{
	return ap_client_connected;
}
