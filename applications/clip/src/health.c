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
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <zephyr/sys/reboot.h>
#include <stdlib.h>

#include "health.h"
#include "clip.h"
#include "config.h"
#include "http_upload.h"
#if defined(CONFIG_CLIP_MTLS)
#include "mtls.h"
#endif
#include "clip_event.h"
#include "storage.h"
#include "wifi.h"
#include "audio.h"

LOG_MODULE_REGISTER(health, CONFIG_CLIP_LOG_LEVEL);

/* 12K y no 4K: http_post_json corre el handshake TLS EN ESTE HILO, y el hilo
 * de subida —que hace exactamente lo mismo— tiene medidos ~7KB de pico sobre
 * sus 14KB. Con 4KB el primer latido tras conectar desbordaba la pila y
 * tumbaba el device entero. Paso desapercibido durante dias porque el bug del
 * buffer del snapshot (-ENOMEM con 832 bytes) abortaba el latido ANTES de
 * llegar al TLS: al arreglar aquel, aflora este. Dos bugs anidados, el de
 * arriba tapando al de abajo. */
#define HEARTBEAT_STACK_SIZE 12288
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
	char mac[18] = "";
	struct sys_memory_stats hs = {0};
	extern struct k_heap _system_heap;
	uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);
	bool have_storage;
	int n;

	if (!buf || len == 0) {
		return -EINVAL;
	}

	have_storage = (storage_get_stats(&st) == 0);
	(void)wifi_get_mac(mac, sizeof(mac));
	/* Memoria libre del heap: en una jornada de 8 horas es donde se veria una
	 * fuga, y una fuga lenta no se nota hasta que el device deja de subir. */
	(void)sys_heap_runtime_stats_get(&_system_heap.heap, &hs);
	http_upload_get_status(&up);

	/* One flat object. Deliberately not nested: this gets parsed by a panel,
	 * a log line, and whatever comes next, and flat survives all three. */
	n = snprintf(buf, len,
		     "{\"device\":\"%s\",\"uptime_s\":%u,\"reset\":\"%s\","
		     "\"battery_pct\":%u,\"battery_mv\":%u,\"charging\":%s,"
		     "\"battery_temp_c\":%d,\"battery_ua\":%d,"
		     "\"sd_mounted\":%s,\"sd_free_mb\":%u,\"sd_total_mb\":%u,"
		     "\"sd_used_mb\":%u,"
		     "\"wifi\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
		     "\"mac\":\"%s\",\"upload_every_min\":%u,"
		     "\"wifi_err\":\"%s\","
		     "\"state\":\"%s\","
		     "\"upload_state\":\"%s\",\"upload_done\":%u,"
		     "\"upload_total\":%u,\"upload_err\":%d,"
		     "\"rssi\":%d,\"heap_free\":%u,\"up_stack_free\":%u,"
		     "\"up_kbps\":%u,\"up_ok\":%u,\"up_fail\":%u,"
		     "\"pending_files\":%u,\"recording\":%s}",
		     device_id_str(), uptime_s, reset_cause_txt(boot_reset_cause),
		     ctx->status.battery_percent, ctx->status.battery_mv,
		     ctx->status.battery_charging ? "true" : "false",
		     ctx->status.battery_temp,
		     /* IBAT en crudo y en microamperios, negativo = descargando.
		      * No es el consumo total: el nRF7002 cuelga de VBAT por
		      * delante del PMIC y su corriente no pasa por el sensor.
		      * Ver clip.h. */
		     ctx->status.battery_ua,
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
		     /* La MAC en cada latido, no solo por AT+DEVICE. Las redes de
		      * tienda filtran por lista blanca, y dar de alta 100 devices
		      * conectandolos uno a uno por cable es media jornada; con esto
		      * el panel las tiene todas.
		      *
		      * Ojo: hoy la MAC es aleatoria en cada arranque
		      * (CONFIG_WIFI_RANDOM_MAC_ADDRESS), asi que este campo cambia
		      * solo. Sirve para verlo, no todavia para dar de alta nada. */
		     mac,
		     (unsigned int)CONFIG_CLIP_UPLOAD_INTERVAL_MIN,
		     /* Por que no hay red, no solo que no la hay. Con 100 devices en
		      * tiendas, "sin red" a secas es una llamada de soporte; "clave
		      * incorrecta" o "no-dhcp" se resuelve sin desplazarse. Vacio
		      * cuando esta conectado. */
		     wifi_sta_is_connected() ? "" : wifi_sta_get_fail_reason(),
		     clip_state_to_string(clip_event_get_state()),
		     up.state == HTTP_UPLOAD_RUNNING ? "running" :
		     up.state == HTTP_UPLOAD_DONE    ? "done" :
		     up.state == HTTP_UPLOAD_FAILED  ? "failed" : "idle",
		     (unsigned int)up.files_done, (unsigned int)up.files_total,
		     up.last_error,
		     /* Todo lo que hace falta para decidir sin ir a mirar el device:
		      * la senal explica reintentos y subidas lentas; el heap delata
		      * fugas en jornadas largas; la pila del hilo de subida es donde
		      * este firmware ya se cayo dos veces; la velocidad y los
		      * contadores dicen si el intervalo de 15 min aguanta; y
		      * pending_files es el numero que lo decide — si crece dia tras
		      * dia, se graba mas rapido de lo que se sube. */
		     wifi_sta_get_rssi(),
		     (unsigned int)hs.free_bytes,
		     (unsigned int)up.stack_free,
		     (unsigned int)up.last_kbps,
		     (unsigned int)up.ok_count,
		     (unsigned int)up.fail_count,
		     (unsigned int)up.pending_files,
		     audio_is_recording() ? "true" : "false");

	if (n < 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	return n;
}


/* ── Ordenes que llegan en la respuesta del latido ────────────────────────
 *
 * El device pregunta; nadie le habla sin que pregunte (Doc 23 §4). No hay
 * puerto escuchando, asi que no hay puerto que atacar, y el canal ya esta
 * autenticado y cifrado por el mismo TLS que lleva el latido.
 *
 * Formato:  {"commands":[{"id":7,"cmd":"stop_recording"}, ...]}
 *
 * Se parsea a mano y no con el JSON de Zephyr a proposito: son dos campos y
 * el parser generico cuesta ~3KB de FLASH en una imagen que va al 97%. Lo que
 * se paga a cambio es rigor: cualquier cosa que no encaje se ignora en vez de
 * adivinarse.
 */

/* Lista blanca. Una orden que no este aqui se registra y se descarta: la
 * respuesta del servicio es entrada no confiable como cualquier otra, y un
 * despachador que ejecuta cadenas arbitrarias es una puerta trasera. */
static void run_command(const char *cmd, int id)
{
	struct clip_event_result_info info = {0};

	LOG_WRN("orden #%d del servicio: %s", id, cmd);

	if (strcmp(cmd, "stop_recording") == 0) {
		/* Por el sistema de eventos y no llamando a audio_*: es el mismo
		 * camino que usan el boton y AT+STOP, asi que la maquina de estados,
		 * la pantalla y el haptico se enteran. Llamar al driver por debajo
		 * dejaria el device grabando segun su propio estado. */
		(void)clip_post_event_sync(CLIP_EVENT_STOP, &info);
	} else if (strcmp(cmd, "start_recording") == 0) {
		(void)clip_post_event_sync(CLIP_EVENT_START, &info);
	} else if (strcmp(cmd, "upload_now") == 0) {
		/* Adelanta la pasada, que es la que conoce el backlog completo.
		 * http_upload_session_async() exige un session_id y devuelve
		 * -EINVAL con NULL — se comprobo. */
		(void)http_upload_sweep_now();
	} else if (strcmp(cmd, "wipe") == 0) {
		/* La orden de un device perdido. Destruye la clave, no los
		 * ficheros: instantaneo aunque la tarjeta este llena, e
		 * irreversible — el audio queda como ciphertext sin llave.
		 *
		 * No se reinicia despues: el device sigue latiendo, y que siga
		 * apareciendo en el panel es util para localizarlo. */
		(void)config_wipe_secrets();
#if defined(CONFIG_CLIP_MTLS)
		(void)mtls_wipe();
#endif
		LOG_WRN("device inutilizado por orden remota");
	} else if (strcmp(cmd, "reboot") == 0) {
		sys_reboot(SYS_REBOOT_COLD);
	} else if (strcmp(cmd, "health_now") == 0) {
		/* Ya estamos dentro del latido: no hay nada que hacer. */
	} else {
		LOG_WRN("orden desconocida, se ignora: '%s'", cmd);
	}
}

static void handle_commands(const char *body)
{
	const char *p = body;

	if (!body || !strstr(body, "\"commands\"")) {
		return;
	}

	/* Recorre los objetos {"id":N,"cmd":"X"} sin construir un arbol. */
	while ((p = strstr(p, "\"cmd\"")) != NULL) {
		char cmd[32];
		const char *ini, *fin;
		int id = 0;
		const char *idp;
		size_t n;

		ini = strchr(p + 5, '"');
		if (!ini) {
			return;
		}
		ini++;
		fin = strchr(ini, '"');
		if (!fin) {
			return;
		}
		n = (size_t)(fin - ini);
		if (n >= sizeof(cmd)) {
			/* Mas larga que cualquier orden valida: no se trunca para
			 * compararla, se descarta. Truncar podria convertir una
			 * cadena inventada en una orden real. */
			LOG_WRN("orden demasiado larga (%u), se ignora",
				(unsigned int)n);
			p = fin;
			continue;
		}
		memcpy(cmd, ini, n);
		cmd[n] = '\0';

		/* El id es informativo: sirve para el log y para el acuse cuando
		 * el servicio lo pida. */
		idp = strstr(p > body + 20 ? p - 20 : body, "\"id\"");
		if (idp && idp < p) {
			id = atoi(idp + 5);
		}

		run_command(cmd, id);
		p = fin;
	}
}

static void heartbeat_work_fn(struct k_work *work)
{
	/* 960: cada campo nuevo acerca el truncado, y snprintf trunca en
	 * silencio — el latido saldria como JSON invalido y el panel lo
	 * descartaria sin decir por que. health_snapshot_json() devuelve
	 * -ENOMEM si no cabe, y eso si se registra. El snapshot ya rebaso
	 * los 512 del buffer de AT+HEALTH? (fallaba con "Could not build
	 * the snapshot"); se suben los dos a 960, alineados, para que
	 * quepan o fallen juntos. */
	char json[960];
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

	{
		/* 256 dan para varias ordenes; el servicio entrega como mucho 8 y
		 * cada una son ~35 bytes. Si no cabe, se trunca y el parseo
		 * descarta lo cortado — es preferible a reservar en una pila que
		 * ya tuvo un desbordamiento aqui mismo. */
		char rsp[256];

		ret = http_post_json_rsp("/health", json, (size_t)ret,
					 rsp, sizeof(rsp));
		if (ret) {
			/* At WRN, not ERR: a missed beat is normal when the device
			 * is moving between access points. */
			LOG_WRN("Heartbeat did not land: %d", ret);
		} else {
			handle_commands(rsp);
		}
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
