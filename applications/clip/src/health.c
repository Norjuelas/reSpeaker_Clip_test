/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the device says about itself, and the heartbeat that carries it.
 *
 * The device pushes; it does not serve. An HTTP server on the device would
 * cost FLASH this image does not have, and a Clip sitting behind a home router
 * is not reachable from outside anyway — the panel could never poll it. A POST
 * every few minutes works from any network that can reach the service, and it
 * reuses the upload client already built.
 *
 * The reply to that POST is where remote control will arrive: the service can
 * answer with pending commands, and the device acts on them the next time it
 * beats. That costs nothing extra — the connection is already open. Not wired
 * up yet; see Doc 14.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "health.h"
#include "clip.h"
#include "clip_event.h"
#include "config.h"
#include "storage.h"
#include "wifi.h"
#include "http_upload.h"

LOG_MODULE_REGISTER(health, CONFIG_CLIP_LOG_LEVEL);

#define HEARTBEAT_STACK_SIZE 4096
#define HEARTBEAT_PRIORITY   7

static K_THREAD_STACK_DEFINE(heartbeat_stack, HEARTBEAT_STACK_SIZE);
static struct k_work_q heartbeat_wq;
static struct k_work_delayable heartbeat_work;
static bool heartbeat_running;
static bool heartbeat_enabled;

/* Why the device last restarted. Read once at boot: hwinfo clears the cause
 * only when asked, and reading it repeatedly from different threads would race.
 * A panel that can see "watchdog" or "brownout" across a fleet is the whole
 * point of collecting it. */
static uint32_t boot_reset_cause;

static const char *reset_cause_txt(uint32_t cause)
{
	if (cause == 0) {
		return "unknown";
	}
	if (cause & RESET_WATCHDOG) {
		return "watchdog";
	}
	if (cause & RESET_SOFTWARE) {
		return "software";
	}
	if (cause & RESET_BROWNOUT) {
		return "brownout";
	}
	if (cause & RESET_POR) {
		return "power-on";
	}
	if (cause & RESET_PIN) {
		return "pin";
	}
	if (cause & RESET_DEBUG) {
		return "debug";
	}
	if (cause & RESET_LOW_POWER_WAKE) {
		return "wake";
	}
	return "other";
}

static const char *device_id_str(void)
{
	static char id[17];
	uint8_t chip_id[8];
	ssize_t len;

	if (id[0] != '\0') {
		return id;
	}

	len = hwinfo_get_device_id(chip_id, sizeof(chip_id));
	if (len <= 0) {
		strcpy(id, "unknown");
		return id;
	}

	for (ssize_t i = 0; i < len && i < 8; i++) {
		snprintf(id + i * 2, 3, "%02X", chip_id[i]);
	}

	return id;
}

int health_snapshot_json(char *buf, size_t len)
{
	struct clip_context *ctx = clip_get_context();
	struct http_upload_status up;
	struct storage_stats st = {0};
	uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);
	bool have_storage;
	int n;

	if (!buf || len == 0) {
		return -EINVAL;
	}

	have_storage = (storage_get_stats(&st) == 0);
	http_upload_get_status(&up);

	/* One flat object. Deliberately not nested: this gets parsed by a panel,
	 * a log line, and whatever comes next, and flat survives all three. */
	n = snprintf(buf, len,
		     "{\"device\":\"%s\",\"uptime_s\":%u,\"reset\":\"%s\","
		     "\"battery_pct\":%u,\"battery_mv\":%u,\"charging\":%s,"
		     "\"battery_temp_c\":%d,"
		     "\"sd_mounted\":%s,\"sd_free_mb\":%u,\"sd_total_mb\":%u,"
		     "\"sd_used_mb\":%u,"
		     "\"wifi\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
		     "\"state\":\"%s\","
		     "\"upload_state\":\"%s\",\"upload_done\":%u,"
		     "\"upload_total\":%u,\"upload_err\":%d}",
		     device_id_str(), uptime_s, reset_cause_txt(boot_reset_cause),
		     ctx->status.battery_percent, ctx->status.battery_mv,
		     ctx->status.battery_charging ? "true" : "false",
		     ctx->status.battery_temp,
		     have_storage && st.is_mounted ? "true" : "false",
		     have_storage ? st.free_space_mb : 0,
		     have_storage ? st.total_mb : 0,
		     /* Espacio ocupado, no el contador de trozos: st.total_chunks
		      * solo sube en storage_write_chunk(), a la que la grabacion
		      * no llama nunca — se vio en la primera prueba real, con el
		      * device grabando y el campo clavado en 0. Un numero muerto
		      * en un panel es peor que un campo ausente.
		      *
		      * Lo que un operador querria de verdad es cuantas sesiones
		      * quedan por subir; eso necesita el registro de subidas, que
		      * no existe. Mientras tanto el espacio ocupado si se mueve
		      * al grabar y sirve de senal. */
		     have_storage ? (st.total_mb - st.free_space_mb) : 0,
		     wifi_sta_is_connected() ? "true" : "false",
		     /* wifi_sta_get_ip(), no wifi_get_ip_address(): esa segunda
		      * devuelve la del punto de acceso (192.168.4.1), que es la
		      * misma en los 100 devices y no sirve para encontrar
		      * ninguno en la red. */
		     wifi_sta_get_ip(), ctx->config.sta_ssid,
		     clip_state_to_string(clip_event_get_state()),
		     up.state == HTTP_UPLOAD_RUNNING ? "running" :
		     up.state == HTTP_UPLOAD_DONE    ? "done" :
		     up.state == HTTP_UPLOAD_FAILED  ? "failed" : "idle",
		     (unsigned int)up.files_done, (unsigned int)up.files_total,
		     up.last_error);

	if (n < 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	return n;
}

static void heartbeat_work_fn(struct k_work *work)
{
	char json[512];
	int ret;

	ARG_UNUSED(work);

	if (!heartbeat_enabled) {
		return;
	}

	/* Nothing to say to nobody. Reschedule and wait for the network — a
	 * device that is out of range should keep trying quietly, not give up. */
	if (!wifi_sta_is_connected() ||
	    config_get_upload_host()[0] == '\0' || config_get_upload_port() == 0) {
		goto reschedule;
	}

	ret = health_snapshot_json(json, sizeof(json));
	if (ret < 0) {
		LOG_ERR("Could not build the health snapshot: %d", ret);
		goto reschedule;
	}

	ret = http_post_json("/health", json, (size_t)ret);
	if (ret) {
		/* At WRN, not ERR: a missed beat is normal when the device is
		 * moving between access points. */
		LOG_WRN("Heartbeat did not land: %d", ret);
	}

reschedule:
	if (heartbeat_enabled) {
		k_work_schedule_for_queue(&heartbeat_wq, &heartbeat_work,
					  K_SECONDS(CONFIG_CLIP_HEALTH_INTERVAL_S));
	}
}

int health_init(void)
{
	if (heartbeat_running) {
		return 0;
	}

	if (hwinfo_get_reset_cause(&boot_reset_cause) != 0) {
		boot_reset_cause = 0;
	}
	hwinfo_clear_reset_cause();

	k_work_queue_init(&heartbeat_wq);
	k_work_queue_start(&heartbeat_wq, heartbeat_stack,
			   K_THREAD_STACK_SIZEOF(heartbeat_stack),
			   HEARTBEAT_PRIORITY, NULL);
	k_work_init_delayable(&heartbeat_work, heartbeat_work_fn);
	heartbeat_running = true;
	heartbeat_enabled = IS_ENABLED(CONFIG_CLIP_HEALTH_AUTOSTART);

	LOG_WRN("Boot: last reset was %s", reset_cause_txt(boot_reset_cause));

	if (heartbeat_enabled) {
		/* Not immediately: at boot there is no network yet, and the
		 * first beat would only report a device that cannot talk. */
		k_work_schedule_for_queue(&heartbeat_wq, &heartbeat_work,
					  K_SECONDS(30));
	}

	return 0;
}

int health_beat_now(void)
{
	if (!heartbeat_running) {
		return -EAGAIN;
	}

	k_work_reschedule_for_queue(&heartbeat_wq, &heartbeat_work, K_NO_WAIT);
	return 0;
}

void health_set_enabled(bool on)
{
	if (!heartbeat_running) {
		return;
	}

	heartbeat_enabled = on;

	if (on) {
		k_work_reschedule_for_queue(&heartbeat_wq, &heartbeat_work,
					    K_NO_WAIT);
	} else {
		k_work_cancel_delayable(&heartbeat_work);
	}
}

bool health_is_enabled(void)
{
	return heartbeat_enabled;
}
