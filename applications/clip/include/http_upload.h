/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_HTTP_UPLOAD_H
#define CLIP_HTTP_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "storage.h"

enum http_upload_state {
	HTTP_UPLOAD_IDLE = 0,
	HTTP_UPLOAD_RUNNING,
	HTTP_UPLOAD_DONE,
	HTTP_UPLOAD_FAILED,
};

struct http_upload_status {
	enum http_upload_state state;
	char session_id[STORAGE_SESSION_ID_LEN];
	uint32_t files_total;
	uint32_t files_done;
	uint32_t bytes_sent;
	int last_error;
	/* Bytes still unused on the upload thread's stack at the end of the last
	 * job. Exposed on purpose: two stack overflows in this firmware came from
	 * sizes picked by eye, so the number is worth being able to read. 0 means
	 * it has not been measured (CONFIG_INIT_STACKS off). */
	size_t stack_free;

	/* Rendimiento, para poder ajustar el intervalo con datos en vez de con
	 * criterio: velocidad de la ultima subida, cuantas salieron bien y mal
	 * desde el arranque, y cuantos ficheros quedan por enviar.
	 *
	 * pending_files es el que decide si el intervalo aguanta: si crece
	 * jornada tras jornada, se sube mas despacio de lo que se graba. */
	uint32_t last_kbps;
	uint32_t ok_count;
	uint32_t fail_count;
	uint32_t pending_files;
};

/**
 * @brief Start the upload work queue
 *
 * Call once at boot. Uploads run on their own thread from here on.
 */
int http_upload_init(void);

/**
 * @brief Queue a whole session for upload and return immediately
 *
 * The work happens on the upload thread, so the caller — normally the AT
 * server — keeps its channel free. Poll http_upload_get_status() for progress.
 *
 * @retval 0         queued
 * @retval -EBUSY    an upload is already running
 * @retval -ENOENT   no endpoint configured (AT+UPCFG)
 * @retval -ENETDOWN not joined to a network (AT+STA=on)
 */
int http_upload_session_async(const char *session_id);

/**
 * @brief Adelanta la pasada de subida en vez de esperar al intervalo.
 *
 * Lo usa la orden remota `upload_now` que llega en la respuesta del latido.
 * A diferencia de http_upload_session_async(), no necesita saber que sesion
 * subir: la pasada periodica ya calcula el backlog y lo drena entero.
 *
 * @retval 0 o mayor  reprogramada
 * @retval -ENODEV    la cola de subida todavia no arranco
 */
int http_upload_sweep_now(void);

/**
 * @brief Read the state of the current or last upload
 */
void http_upload_get_status(struct http_upload_status *out);

/**
 * @brief POST one recording to the configured HTTP endpoint
 *
 * Streams the file straight from the SD card — a recording is a few hundred KB
 * and there is no RAM to hold one. Posts to /upload/<session>/<filename> with
 * the device id in X-Device-Id.
 *
 * Plain HTTP. Fine for the concept test, not for devices in the field carrying
 * conversation audio; see Doc 13 for the fleet CA and mTLS plan.
 *
 * Blocks until the endpoint answers. Call it from the upload thread, not from
 * the AT server.
 *
 * @param session_id Session the file belongs to
 * @param filename   Name to store it under
 * @param path       Full path on the SD card
 * @param size       File size in bytes
 * @return 0 if the endpoint answered 2xx
 * @retval -ENOENT    no endpoint configured (AT+UPCFG)
 * @retval -ENETDOWN  not joined to a network (AT+STA=on)
 * @retval -ENOMEM    no heap for the transfer buffers
 * @retval -EIO       the endpoint rejected it, or the read failed
 */
int http_upload_file(const char *session_id, const char *filename,
		     const char *path, size_t size);

/**
 * @brief POST a body that is already in memory
 *
 * For small payloads — the health heartbeat — where streaming from the SD card
 * would be machinery for nothing. Blocks until the endpoint answers.
 *
 * @retval 0 if the endpoint answered 2xx
 */
int http_post_json(const char *url, const char *body, size_t body_len);

/**
 * @brief Como http_post_json(), pero devolviendo el cuerpo de la respuesta.
 *
 * Lo necesita el latido: la cola de ordenes para el device viaja en la
 * respuesta del POST /health. El device pregunta y se lleva lo que haya —
 * nada escucha en el device, asi que no hay nada que atacar.
 *
 * @param rsp     buffer donde dejar el cuerpo (puede ser NULL).
 * @param rsp_max su tamano; el cuerpo se trunca si no cabe.
 */
int http_post_json_rsp(const char *url, const char *body, size_t body_len,
		       char *rsp, size_t rsp_max);

#endif /* CLIP_HTTP_UPLOAD_H */
