/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * La superficie del transporte UDP sin nada detras, construida en lugar de
 * transport_udp.c y wifi_udp.c cuando CLIP_UDP_TRANSPORT=n.
 *
 * Se retira por dos motivos que apuntan al mismo sitio.
 *
 * El primero es de seguridad, y es el que manda: el servidor AT por UDP
 * responde en modo estacion —no solo en modo punto de acceso— sin credencial de
 * ninguna clase, y acepta AT+FACTORY, AT+FORMAT y AT+POWEROFF. Cualquiera en la
 * red de la tienda puede borrar un device. Se comprobo en hardware.
 *
 * El segundo es espacio: la imagen con TLS estaba al 99,81% de FLASH, con 1748
 * bytes libres. Con ese margen cualquier cosa que se anada no cabe, y el
 * diagnostico de cualquier fallo empieza por sospechar del enlazado.
 *
 * Lo que se pierde: la descarga de audio por UDP (udp_sync.py) y el canal AT
 * por red. Ambos son redundantes desde que la subida por HTTP funciona y esta
 * verificada byte a byte contra ese mismo camino UDP. Para reparar un device
 * queda el cable, que es lo que se decidio que fuera el canal de reparacion.
 *
 * Stubs y no #ifdefs repartidos: diecinueve funciones se llaman desde cinco
 * ficheros que no tienen nada que ver con UDP. Volver a activarlo es
 * CLIP_UDP_TRANSPORT=y sin tocar ningun sitio de llamada.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <stddef.h>
#include <errno.h>

#include "transport.h"
#include "transport_udp.h"
#include "wifi_udp.h"

LOG_MODULE_REGISTER(udp_stub, CONFIG_CLIP_LOG_LEVEL);

int transport_udp_init(void)
{
	return 0;
}

int transport_udp_send(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

int transport_udp_send_file_data(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

int transport_udp_send_file_start(const char *filename, uint32_t file_size)
{
	ARG_UNUSED(filename);
	ARG_UNUSED(file_size);
	return -ENOTCONN;
}

int transport_udp_send_file_end(void)
{
	return -ENOTCONN;
}

int transport_udp_send_transfer_done(const char *session_id, uint32_t file_count)
{
	ARG_UNUSED(session_id);
	ARG_UNUSED(file_count);
	return -ENOTCONN;
}

int transport_udp_send_response(const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTCONN;
}

bool transport_udp_is_active(void)
{
	return false;
}

void transport_udp_update_active(bool active)
{
	ARG_UNUSED(active);
}

void transport_udp_update_client_addr(const struct sockaddr *addr, socklen_t len)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(len);
}

int transport_udp_set_peer(const char *ip, uint16_t port)
{
	ARG_UNUSED(ip);
	ARG_UNUSED(port);
	return -ENOTSUP;
}

void transport_udp_notify_file_ack(uint8_t result, const uint8_t *bitmap,
				   uint16_t bitmap_len, uint16_t total_seqs)
{
	ARG_UNUSED(result);
	ARG_UNUSED(bitmap);
	ARG_UNUSED(bitmap_len);
	ARG_UNUSED(total_seqs);
}

/* NULL para que main.c no registre un transporte que no puede llevar nada;
 * transport_register() lo rechaza. */
struct transport *transport_udp_get(void)
{
	return NULL;
}

void transport_udp_reset_file_state(void)
{
}

/* ---- wifi_udp ---- */

int wifi_udp_init(void)
{
	LOG_WRN("Construido sin transporte UDP: el canal AT por red esta cerrado");
	return 0;
}

int wifi_udp_start(void)
{
	return -ENOTSUP;
}

void wifi_udp_stop(void)
{
}

bool wifi_udp_is_running(void)
{
	return false;
}

bool wifi_udp_is_active(void)
{
	return false;
}
