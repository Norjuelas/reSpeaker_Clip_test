/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Que se ha subido ya, para no subirlo dos veces.
 *
 * Sin esto, cualquier reintento —y va a haber reintentos, porque la red de una
 * tienda se cae— vuelve a mandar ficheros que ya estan en S3. Duplicar audio
 * cuesta almacenamiento, ensucia la transcripcion y hace imposible saber cuanto
 * se grabo de verdad.
 *
 * El registro es un fichero de texto en la tarjeta, una linea por fichero
 * subido. Se eligio texto sobre un formato binario compacto por una razon
 * concreta: cuando algo vaya mal en campo, alguien va a mirar esto con un
 * editor, y un indice binario obliga a escribir una herramienta antes de poder
 * diagnosticar nada.
 *
 * El tamaño no preocupa: una jornada de 8 horas son 96 ficheros, unos 2 KB de
 * registro al dia contra una tarjeta de 1,8 GB.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "upload_registry.h"
#include "storage.h"

LOG_MODULE_REGISTER(upload_reg, CONFIG_CLIP_LOG_LEVEL);

#define REGISTRY_PATH "/SD:/UPLOADED.TXT"

/* Una linea es "20260811122300:0001\n" — 20 bytes. El buffer de lectura toma
 * varias de golpe para no hacer una llamada al sistema de ficheros por linea. */
#define LINE_MAX   24
#define SCAN_CHUNK 512

static K_MUTEX_DEFINE(registry_lock);

static void make_key(char *out, size_t len, const char *session, uint32_t idx)
{
	snprintf(out, len, "%s:%04u", session, (unsigned int)idx);
}

bool upload_registry_has(const char *session, uint32_t idx)
{
	struct fs_file_t f;
	char key[LINE_MAX];
	char *buf;
	bool found = false;
	ssize_t n;
	size_t carry = 0;

	if (!session) {
		return false;
	}

	make_key(key, sizeof(key), session, idx);

	k_mutex_lock(&registry_lock, K_FOREVER);

	fs_file_t_init(&f);
	if (fs_open(&f, REGISTRY_PATH, FS_O_READ) != 0) {
		/* Sin registro no hay nada subido. Es el estado de un device nuevo,
		 * no un error. */
		k_mutex_unlock(&registry_lock);
		return false;
	}

	buf = k_malloc(SCAN_CHUNK + LINE_MAX + 1);
	if (!buf) {
		fs_close(&f);
		k_mutex_unlock(&registry_lock);
		LOG_ERR("Sin memoria para leer el registro");
		return false;
	}

	/* Se arrastra la cola del bloque anterior porque una entrada puede quedar
	 * partida entre dos lecturas, y buscarla solo dentro de cada bloque la
	 * daria por ausente — se resubiria el fichero. */
	while ((n = fs_read(&f, buf + carry, SCAN_CHUNK)) > 0) {
		size_t total = carry + (size_t)n;

		buf[total] = '\0';
		if (strstr(buf, key)) {
			found = true;
			break;
		}

		carry = MIN(total, (size_t)LINE_MAX);
		memmove(buf, buf + total - carry, carry);
	}

	k_free(buf);
	fs_close(&f);
	k_mutex_unlock(&registry_lock);

	return found;
}

int upload_registry_mark(const char *session, uint32_t idx)
{
	struct fs_file_t f;
	char line[LINE_MAX + 2];
	int ret;
	int len;

	if (!session) {
		return -EINVAL;
	}

	len = snprintf(line, sizeof(line), "%s:%04u\n", session, (unsigned int)idx);
	if (len < 0 || len >= (int)sizeof(line)) {
		return -EINVAL;
	}

	k_mutex_lock(&registry_lock, K_FOREVER);

	fs_file_t_init(&f);
	ret = fs_open(&f, REGISTRY_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (ret) {
		k_mutex_unlock(&registry_lock);
		LOG_ERR("No se pudo abrir el registro: %d", ret);
		return ret;
	}

	ret = fs_write(&f, line, (size_t)len);

	/* Sincronizar antes de cerrar, y no confiar en el cierre: si el device se
	 * queda sin bateria justo despues de subir, la anotacion tiene que estar
	 * en la tarjeta. Perder la marca de algo ya subido significa subirlo otra
	 * vez, que es exactamente lo que este fichero existe para evitar. */
	fs_sync(&f);
	fs_close(&f);

	k_mutex_unlock(&registry_lock);

	if (ret < 0) {
		LOG_ERR("No se pudo anotar %s:%04u: %d", session,
			(unsigned int)idx, ret);
		return ret;
	}

	return 0;
}

int upload_registry_reset(void)
{
	int ret;

	k_mutex_lock(&registry_lock, K_FOREVER);
	ret = fs_unlink(REGISTRY_PATH);
	k_mutex_unlock(&registry_lock);

	if (ret == -ENOENT) {
		return 0;
	}

	LOG_WRN("Registro de subidas borrado: todo se volvera a subir");
	return ret;
}
