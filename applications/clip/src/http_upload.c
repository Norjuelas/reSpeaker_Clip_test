/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pushes recordings to an HTTP endpoint.
 *
 * Plain HTTP for now. The audio is conversation recordings, so shipping this to
 * devices in the field without TLS is not defensible — see Doc 13 for the fleet
 * CA and mTLS plan. Room is tight: the nRF70 firmware moving out of the image
 * freed ~52KB, but the HTTP client and TCP took most of it back. 27KB are left
 * against the ~45KB a default TLS build costs, so it has to be trimmed.
 *
 * Streams straight from the SD card. A recording is a few hundred KB and there
 * is no RAM to buffer one whole.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef CONFIG_CLIP_UPLOAD_TLS
#include <zephyr/net/tls_credentials.h>
#endif

#include "http_upload.h"
#include "config.h"
#include "storage.h"
#include "wifi.h"
#include "upload_registry.h"

LOG_MODULE_REGISTER(http_upload, CONFIG_CLIP_LOG_LEVEL);

/* Enough for the response headers; the body we get back is a short status. */
#define RECV_BUF_SIZE   512
#define SEND_CHUNK      1024
#define HTTP_TIMEOUT_MS 15000
#ifdef CONFIG_CLIP_UPLOAD_TLS
/* 20s con TLS. Los 5s que bastan para un connect TCP cortan el handshake por
 * la mitad: el saludo son varios viajes de ida y vuelta mas la verificacion de
 * la cadena, y en este chip la parte de curva eliptica no es instantanea. El
 * sintoma era ETIMEDOUT, que se lee como problema de red y no lo es. */
#define CONNECT_TIMEOUT_MS 20000
#else
#define CONNECT_TIMEOUT_MS 5000
#endif

#ifdef CONFIG_CLIP_UPLOAD_TLS
/* La CA se lee de la tarjeta, no se compila dentro. Asi una misma imagen sirve
 * para toda la flota y cambiar de CA no obliga a reconstruir 100 firmwares —
 * encaja con que la configuracion de entrega ya se hace por cable.
 *
 * El contrapunto: la tarjeta es escribible por USB, de modo que quien tenga el
 * device puede cambiar la CA en la que confia. Eso lo cierra el certificado
 * por device del Doc 13, no esto. */
#define CA_PATH        "/SD:/ca.pem"
#define CA_MAX_LEN     2048
#define CA_SEC_TAG     42

static bool ca_loaded;

/* Propiedad permanente del proceso: tls_credential_add() se queda con el
 * puntero, asi que este buffer tiene que sobrevivir a la funcion que lo pide. */
static uint8_t *ca_buf;

static int load_ca(void)
{
	struct fs_file_t f;
	uint8_t *pem;
	ssize_t n;
	bool is_pem;
	int ret;

	if (ca_loaded) {
		return 0;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, CA_PATH, FS_O_READ);
	if (ret) {
		LOG_WRN("Sin CA en %s (%d): se hablara TLS sin validar al servidor",
			CA_PATH, ret);
		return ret;
	}

	pem = k_malloc(CA_MAX_LEN);
	if (!pem) {
		fs_close(&f);
		return -ENOMEM;
	}

	n = fs_read(&f, pem, CA_MAX_LEN - 1);
	fs_close(&f);

	if (n <= 0) {
		k_free(pem);
		return -EIO;
	}

	/* Los dos formatos, y la longitud NO es la misma para cada uno.
	 *
	 * PEM es texto y mbedTLS exige que el tamano incluya el NUL final. DER es
	 * binario y el tamano tiene que ser exacto: pasarle uno de mas corrompe la
	 * estructura ASN.1 y el parseo falla.
	 *
	 * Se acepta PEM porque es lo que sale de openssl y lo que cualquiera tiene
	 * a mano, y DER porque no necesita el decodificador (unos 2KB de FLASH que
	 * en esta imagen se notan). */
	pem[n] = '\0';
	is_pem = (n > 10 && memcmp(pem, "-----BEGIN", 10) == 0);

	ret = tls_credential_add(CA_SEC_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				 pem, is_pem ? (size_t)n + 1 : (size_t)n);
	if (ret) {
		LOG_ERR("La CA de %s no se pudo cargar: %d", CA_PATH, ret);
		k_free(pem);
		return ret;
	}

	/* El buffer NO se libera, y no es un descuido.
	 *
	 * tls_credential_add() guarda el puntero — `credential->buf = cred` — sin
	 * copiar nada. Liberarlo deja la credencial apuntando a memoria libre.
	 *
	 * El sintoma que produjo es instructivo: la primera conexion funcionaba,
	 * porque el heap recien liberado todavia conservaba el contenido, y a
	 * partir de la segunda fallaba con TLS_SEC_TAG_LIST: -22. Y ese EINVAL
	 * salia como EHOSTUNREACH en connect(), acusando a la red de un puntero
	 * colgante. Un fallo intermitente que empeora con el uso es de los peores
	 * de diagnosticar.
	 *
	 * Vive lo que viva el device: la CA se carga una vez y se usa siempre. */
	ca_buf = pem;

	ca_loaded = true;
	LOG_WRN("CA cargada de %s: %u bytes en %s", CA_PATH, (unsigned int)n,
		is_pem ? "PEM" : "DER");
	return 0;
}
#endif /* CONFIG_CLIP_UPLOAD_TLS */

struct upload_ctx {
	struct fs_file_t *file;
	size_t remaining;
	uint8_t *chunk;
	int status;
	bool done;
};

static char device_id[17];

static const char *get_device_id(void)
{
	uint8_t chip_id[8];
	ssize_t len;

	if (device_id[0] != '\0') {
		return device_id;
	}

	len = hwinfo_get_device_id(chip_id, sizeof(chip_id));
	if (len <= 0) {
		strcpy(device_id, "unknown");
		return device_id;
	}

	for (ssize_t i = 0; i < len && i < 8; i++) {
		snprintf(device_id + i * 2, 3, "%02X", chip_id[i]);
	}

	return device_id;
}

/* Feeds the request body from the SD card, one chunk per call. Returning 0
 * ends the body — the HTTP client keeps calling until then. */
static int payload_cb(int sock, struct http_request *req, void *user_data)
{
	struct upload_ctx *ctx = user_data;
	size_t total = 0;

	while (ctx->remaining > 0) {
		size_t want = MIN(ctx->remaining, (size_t)SEND_CHUNK);
		ssize_t got = fs_read(ctx->file, ctx->chunk, want);

		if (got <= 0) {
			LOG_ERR("Read failed with %u bytes left: %d",
				(unsigned int)ctx->remaining, (int)got);
			return -EIO;
		}

		/* send() is free to take less than it was offered, and a chunk that
		 * goes out half-sent is not an error the server can see: it just
		 * receives fewer bytes than Content-Length promised and waits for
		 * the rest until the request times out. Push until the chunk is
		 * gone. */
		size_t off = 0;

		while (off < (size_t)got) {
			int sent = zsock_send(sock, ctx->chunk + off,
					      (size_t)got - off, 0);

			if (sent <= 0) {
				LOG_ERR("send stalled at %u/%u of the chunk: %d",
					(unsigned int)off, (unsigned int)got,
					sent < 0 ? -errno : 0);
				return sent < 0 ? -errno : -EIO;
			}

			off += (size_t)sent;
			total += (size_t)sent;
		}

		ctx->remaining -= (size_t)got;
	}

	return (int)total;
}

static int response_cb(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	struct upload_ctx *ctx = user_data;

	if (final == HTTP_DATA_FINAL) {
		ctx->status = rsp->http_status_code;
		ctx->done = true;
	}

	return 0;
}

static int connect_to_endpoint(const char *host, uint16_t port)
{
	struct sockaddr_in addr = {0};
	int sock;
	int ret;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (zsock_inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		LOG_ERR("'%s' is not a dotted-quad address", host);
		return -EINVAL;
	}

#ifdef CONFIG_CLIP_UPLOAD_TLS
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
#else
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
	if (sock < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return -errno;
	}

#ifdef CONFIG_CLIP_UPLOAD_TLS
	{
		sec_tag_t tags[] = { CA_SEC_TAG };
		int verify;

		load_ca();

		/* Sin CA no se puede verificar nada, y fingir que si seria peor que
		 * no cifrar: daria una sensacion de seguridad que no existe. Se cifra
		 * igualmente — protege de quien escucha la red — pero queda dicho en
		 * el log que no se comprueba con quien se habla. */
		verify = ca_loaded ? TLS_PEER_VERIFY_REQUIRED : TLS_PEER_VERIFY_NONE;
		if (!ca_loaded) {
			LOG_WRN("TLS sin verificar al servidor: cifra, pero no autentica");
		}

		if (ca_loaded &&
		    zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST, tags,
				     sizeof(tags)) < 0) {
			LOG_ERR("TLS_SEC_TAG_LIST: %d", -errno);
		}
		zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY, &verify,
				 sizeof(verify));
		/* TLS_HOSTNAME no es (solo) SNI: es el nombre contra el que mbedTLS
		 * compara el certificado del servidor. Si no se fija, Zephyr pone ""
		 * a proposito para forzar la verificacion, y "" no coincide con
		 * ningun certificado: el handshake muere en BADCERT_CN_MISMATCH y
		 * arriba se ve como connect() = -113 (ECONNABORTED), sin mencion a
		 * certificados por ninguna parte. El certificado del servicio lleva
		 * la IP en el subjectAltName, asi que la IP en texto casa bien.
		 * (Con MBEDTLS_SSL_SERVER_NAME_INDICATION apagado no se emite la
		 * extension SNI; esto solo alimenta la comparacion X.509.) */
		if (ca_loaded &&
		    zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME, host,
				     strlen(host)) < 0) {
			LOG_ERR("TLS_HOSTNAME: %d", -errno);
		}
	}
#endif

	/* Without this, a wrong address or a service that is not listening makes
	 * connect() block for the stack's full retry sequence, and the caller —
	 * the AT thread — goes with it. */
	{
		struct zsock_timeval tv = {
			.tv_sec = CONNECT_TIMEOUT_MS / 1000,
			.tv_usec = 0,
		};

		zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		/* Dos convenciones distintas en la misma llamada, y confundirlas
		 * cuesta caro.
		 *
		 * Un socket normal devuelve -1 y deja el motivo en errno. Pero la capa
		 * TLS (ztls_connect_ctx) devuelve el codigo negativo directamente y no
		 * toca errno. Leer errno en ese caso da un valor viejo de cualquier
		 * operacion anterior: aqui salia EHOSTUNREACH, y se perdio una tarde
		 * buscando un problema de red que no existia mientras el fallo real
		 * era del handshake.
		 *
		 * Si ret no es -1, ret ES el error. */
		int err = (ret == -1) ? -errno : ret;

		LOG_ERR("connect a %s:%u fallo: %d", host, port, err);
		zsock_close(sock);
		return err;
	}

	return sock;
}

int http_upload_file(const char *session_id, const char *filename,
		     const char *path, size_t size)
{
	const char *host = config_get_upload_host();
	uint16_t port = config_get_upload_port();
	struct http_request req = {0};
	struct upload_ctx ctx = {0};
	struct fs_file_t file;
	char url[128];
	char len_hdr[32];
	char dev_hdr[64];
	uint8_t *recv_buf = NULL;
	int sock = -1;
	int ret;

	if (host[0] == '\0' || port == 0) {
		return -ENOENT;
	}
	if (!wifi_sta_is_connected()) {
		return -ENETDOWN;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret) {
		LOG_ERR("Cannot open %s: %d", path, ret);
		return ret;
	}

	/* Both buffers come from the heap: static RAM sits above 90% and these
	 * are only live for the duration of one upload. */
	ctx.chunk = k_malloc(SEND_CHUNK);
	recv_buf = k_malloc(RECV_BUF_SIZE);
	if (!ctx.chunk || !recv_buf) {
		LOG_ERR("No heap for the upload buffers");
		ret = -ENOMEM;
		goto out;
	}

	sock = connect_to_endpoint(host, port);
	if (sock < 0) {
		ret = sock;
		goto out;
	}

	ctx.file = &file;
	ctx.remaining = size;

	snprintf(url, sizeof(url), "/upload/%s/%s", session_id, filename);
	snprintf(len_hdr, sizeof(len_hdr), "Content-Length: %u\r\n",
		 (unsigned int)size);
	snprintf(dev_hdr, sizeof(dev_hdr), "X-Device-Id: %s\r\n", get_device_id());

	const char *headers[] = { len_hdr, dev_hdr, NULL };

	req.method = HTTP_POST;
	req.url = url;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.header_fields = headers;
	req.content_type_value = "application/octet-stream";
	req.payload_cb = payload_cb;
	req.payload_len = size;
	req.response = response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = RECV_BUF_SIZE;

	ret = http_client_req(sock, &req, HTTP_TIMEOUT_MS, &ctx);
	if (ret < 0) {
		LOG_ERR("http_client_req failed: %d", ret);
		goto out;
	}

	if (!ctx.done || ctx.status < 200 || ctx.status > 299) {
		LOG_ERR("%s rejected: HTTP %d", filename, ctx.status);
		ret = -EIO;
		goto out;
	}

	LOG_WRN("uploaded %s (%u bytes) -> HTTP %d", filename,
		(unsigned int)size, ctx.status);
	ret = 0;

out:
	if (sock >= 0) {
		zsock_close(sock);
	}
	k_free(ctx.chunk);
	k_free(recv_buf);
	fs_close(&file);

	return ret;
}

/* A POST whose body is already in memory. The audio path streams from the SD
 * card because a recording does not fit in RAM; a health snapshot is 400 bytes
 * and does not need any of that machinery. */
int http_post_json(const char *url, const char *body, size_t body_len)
{
	const char *host = config_get_upload_host();
	uint16_t port = config_get_upload_port();
	struct http_request req = {0};
	struct upload_ctx ctx = {0};
	uint8_t *recv_buf;
	char len_hdr[32];
	char dev_hdr[64];
	int sock;
	int ret;

	if (!url || !body || body_len == 0) {
		return -EINVAL;
	}
	if (host[0] == '\0' || port == 0) {
		return -ENOENT;
	}
	if (!wifi_sta_is_connected()) {
		return -ENETDOWN;
	}

	recv_buf = k_malloc(RECV_BUF_SIZE);
	if (!recv_buf) {
		return -ENOMEM;
	}

	sock = connect_to_endpoint(host, port);
	if (sock < 0) {
		k_free(recv_buf);
		return sock;
	}

	snprintf(len_hdr, sizeof(len_hdr), "Content-Length: %u\r\n",
		 (unsigned int)body_len);
	snprintf(dev_hdr, sizeof(dev_hdr), "X-Device-Id: %s\r\n", get_device_id());

	const char *headers[] = { len_hdr, dev_hdr, NULL };

	req.method = HTTP_POST;
	req.url = url;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.header_fields = headers;
	req.content_type_value = "application/json";
	req.payload = body;
	req.payload_len = body_len;
	req.response = response_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = RECV_BUF_SIZE;

	ret = http_client_req(sock, &req, HTTP_TIMEOUT_MS, &ctx);
	if (ret >= 0 && (!ctx.done || ctx.status < 200 || ctx.status > 299)) {
		ret = -EIO;
	} else if (ret >= 0) {
		ret = 0;
	}

	zsock_close(sock);
	k_free(recv_buf);

	return ret;
}

/* ------------------------------------------------------------------------
 * The upload thread
 *
 * A session upload used to run straight on the AT server thread, which meant
 * the command channel was held for the whole transfer. Two seconds with the
 * test files, but a real session is minutes — and the channel is also how you
 * ask the device what is going on when it misbehaves. TLS will make this worse
 * (a handshake is seconds on its own), so the work moves here first.
 * ------------------------------------------------------------------------ */

/* 10KB, and the number is measured rather than argued.
 *
 * It was briefly cut to 6KB on the reasoning that this thread carries no
 * command parser — the device went down on the first upload. Put back to the
 * 8KB the AT thread had been doing the same work on, CONFIG_INIT_STACKS then
 * reported 1164 bytes left over: peak use is ~7KB, so 6KB never had a chance
 * and 8KB was running at 86%. The AT thread had been that close all along
 * without anyone knowing.
 *
 * 10KB puts the margin near 30%, which matters because TLS goes on top of this
 * same path and a handshake is not free. Re-read stack_free in AT+HTTPUP?
 * after that lands rather than assuming this still holds. */
#ifdef CONFIG_CLIP_UPLOAD_TLS
/* 14KB con TLS. El handshake corre por este mismo hilo y pide varios KB por
 * encima de lo que ya costaba la subida. Medido sin TLS: una grabacion de
 * 1,17MB dejo 3212 bytes libres de 10240, y antes 1164 de 8192 — el 86%
 * consumido. Este firmware ya se ha caido dos veces por una pila mal
 * dimensionada; no se estrena TLS con la tercera. */
#define UPLOAD_STACK_SIZE 14336
#else
/* 10KB sin TLS. Es lo medido, no lo estimado: 7028 bytes de uso maximo
 * observado. Sigue justo, y por eso el hilo informa de lo que le sobra en
 * AT+HTTPUP? tras cada trabajo — el dato vale mas que el criterio. */
#define UPLOAD_STACK_SIZE 10240
#endif
#define UPLOAD_WQ_PRIORITY 6

static K_THREAD_STACK_DEFINE(upload_stack, UPLOAD_STACK_SIZE);
static struct k_work_q upload_wq;
static struct k_work upload_work;
static bool upload_wq_started;

/* Guards everything below it. The AT thread reads this while the upload thread
 * writes it. */
static K_MUTEX_DEFINE(status_lock);
static struct http_upload_status status;
static char pending_session[STORAGE_SESSION_ID_LEN];

static void status_set_state(enum http_upload_state st, int err)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	status.state = st;
	status.last_error = err;
	k_mutex_unlock(&status_lock);
}

static void record_stack_headroom(void)
{
#ifdef CONFIG_INIT_STACKS
	size_t unused = 0;

	if (k_thread_stack_space_get(&upload_wq.thread, &unused) != 0) {
		return;
	}

	k_mutex_lock(&status_lock, K_FOREVER);
	status.stack_free = unused;
	k_mutex_unlock(&status_lock);

	/* At WRN so it survives into the SD log on a production build. If this
	 * ever reads low, it is the warning the last two overflows never gave. */
	LOG_WRN("upload thread: %u of %u stack bytes still free",
		(unsigned int)unused, (unsigned int)UPLOAD_STACK_SIZE);
#endif /* CONFIG_INIT_STACKS */
}

static uint32_t count_session_files(const char *session_id)
{
	struct storage_session_info *sessions;
	uint32_t count = 0;
	int found;

	/* From the heap, never the stack: MAX_SESSIONS is 100 and each entry is
	 * ~56 bytes. On the stack this was a 5.6KB local that took the device
	 * down. */
	sessions = k_malloc(sizeof(*sessions) * CONFIG_CLIP_STORAGE_MAX_SESSIONS);
	if (!sessions) {
		return 0;
	}

	found = storage_list_sessions(sessions, CONFIG_CLIP_STORAGE_MAX_SESSIONS);
	for (int i = 0; i < found; i++) {
		if (strcmp(sessions[i].session_id, session_id) == 0) {
			count = sessions[i].file_count;
			break;
		}
	}

	k_free(sessions);
	return count;
}

static void upload_work_fn(struct k_work *work)
{
	char session_id[STORAGE_SESSION_ID_LEN];
	uint32_t file_count;
	int err = 0;

	ARG_UNUSED(work);

	k_mutex_lock(&status_lock, K_FOREVER);
	strncpy(session_id, pending_session, sizeof(session_id) - 1);
	session_id[sizeof(session_id) - 1] = '\0';
	strncpy(status.session_id, session_id, sizeof(status.session_id) - 1);
	status.files_done = 0;
	status.bytes_sent = 0;
	status.files_total = 0;
	status.state = HTTP_UPLOAD_RUNNING;
	status.last_error = 0;
	k_mutex_unlock(&status_lock);

	file_count = count_session_files(session_id);
	if (file_count == 0) {
		LOG_ERR("%s: unknown session, or it has no files", session_id);
		status_set_state(HTTP_UPLOAD_FAILED, -ENOENT);
		record_stack_headroom();
		return;
	}

	k_mutex_lock(&status_lock, K_FOREVER);
	status.files_total = file_count;
	k_mutex_unlock(&status_lock);

	for (uint32_t idx = 1; idx <= file_count; idx++) {
		char path[128];
		char name[32];
		struct fs_dirent st;

		if (storage_build_chunk_path(session_id, idx, path, sizeof(path)) != 0 ||
		    fs_stat(path, &st) != 0) {
			continue;
		}

		if (upload_registry_has(session_id, idx)) {
			/* Ya esta en S3. Volver a mandarlo duplicaria audio, y con
			 * subidas periodicas cada pocos minutos eso pasaria en cada
			 * pasada. */
			k_mutex_lock(&status_lock, K_FOREVER);
			status.files_done++;
			k_mutex_unlock(&status_lock);
			continue;
		}

		snprintf(name, sizeof(name), "%04u.opus", (unsigned int)idx);

		int64_t t0 = k_uptime_get();

		err = http_upload_file(session_id, name, path, (size_t)st.size);
		if (err) {
			LOG_ERR("%s: file %u of %u failed: %d", session_id,
				(unsigned int)idx, (unsigned int)file_count, err);
			break;
		}

		/* Anotar solo tras un 2xx confirmado: si el endpoint no lo acepto,
		 * el fichero tiene que volver a intentarse. */
		upload_registry_mark(session_id, idx);

		{
			int64_t ms = k_uptime_get() - t0;

			k_mutex_lock(&status_lock, K_FOREVER);
			status.files_done++;
			status.bytes_sent += (uint32_t)st.size;
			status.ok_count++;
			if (ms > 0) {
				status.last_kbps = (uint32_t)(((uint64_t)st.size * 1000U) /
							      ((uint64_t)ms * 1024U));
			}
			k_mutex_unlock(&status_lock);
		}
	}

	if (err) {
		k_mutex_lock(&status_lock, K_FOREVER);
		status.fail_count++;
		k_mutex_unlock(&status_lock);
	}

	status_set_state(err ? HTTP_UPLOAD_FAILED : HTTP_UPLOAD_DONE, err);
	record_stack_headroom();

	LOG_WRN("%s: %u of %u files sent%s", session_id,
		(unsigned int)status.files_done, (unsigned int)file_count,
		err ? " (stopped on error)" : "");
}

/* ------------------------------------------------------------------------
 * La subida periodica
 *
 * El firmware ya corta la grabacion en trozos de 5 minutos por su cuenta, asi
 * que no hay que trocear nada: basta con pasar cada cierto tiempo y mandar lo
 * que haya cerrado y no este anotado en el registro.
 *
 * Se eligieron 15 minutos y no 5 por la radio. Subir en cuanto se cierra un
 * trozo significa despertar el WiFi cada 5 minutos durante toda la jornada; a
 * 15 se agrupan tres ficheros por conexion, y en una celda de 170 mAh la
 * diferencia importa. El precio es que se pueden perder hasta 15 minutos de
 * audio si el device muere sin subir — contra 5. Se asume: el audio sigue en
 * la tarjeta y se sube en cuanto vuelva.
 * ------------------------------------------------------------------------ */

static struct k_work_delayable periodic_work;

static void periodic_work_fn(struct k_work *work)
{
	struct storage_session_info *sessions;
	int found;

	ARG_UNUSED(work);

	if (!wifi_sta_is_connected() ||
	    config_get_upload_host()[0] == '\0' || config_get_upload_port() == 0) {
		goto reschedule;
	}

	/* Nunca por encima de una subida en curso ni de una grabacion: el hilo es
	 * uno solo y la tarjeta esta ocupada. */
	k_mutex_lock(&status_lock, K_FOREVER);
	if (status.state == HTTP_UPLOAD_RUNNING) {
		k_mutex_unlock(&status_lock);
		goto reschedule;
	}
	k_mutex_unlock(&status_lock);

	sessions = k_malloc(sizeof(*sessions) * CONFIG_CLIP_STORAGE_MAX_SESSIONS);
	if (!sessions) {
		goto reschedule;
	}

	found = storage_list_sessions(sessions, CONFIG_CLIP_STORAGE_MAX_SESSIONS);

	/* De la mas antigua a la mas nueva: si la ventana no da para todas, lo que
	 * lleva mas tiempo esperando sale primero. */
	/* Contar TODO lo pendiente antes de subir nada: es el numero que dice si
	 * el intervalo aguanta el ritmo de grabacion o se va acumulando trabajo. */
	{
		uint32_t backlog = 0;

		for (int i = 0; i < found; i++) {
			for (uint32_t idx = 1; idx <= sessions[i].file_count; idx++) {
				if (!upload_registry_has(sessions[i].session_id, idx)) {
					backlog++;
				}
			}
		}

		k_mutex_lock(&status_lock, K_FOREVER);
		status.pending_files = backlog;
		k_mutex_unlock(&status_lock);
	}

	for (int i = found - 1; i >= 0; i--) {
		bool pending = false;

		for (uint32_t idx = 1; idx <= sessions[i].file_count; idx++) {
			if (!upload_registry_has(sessions[i].session_id, idx)) {
				pending = true;
				break;
			}
		}

		if (pending) {
			LOG_WRN("Subida periodica: %s tiene ficheros sin enviar",
				sessions[i].session_id);
			http_upload_session_async(sessions[i].session_id);
			break;
		}
	}

	k_free(sessions);

reschedule:
	k_work_schedule_for_queue(&upload_wq, &periodic_work,
				  K_MINUTES(CONFIG_CLIP_UPLOAD_INTERVAL_MIN));
}

int http_upload_init(void)
{
	if (upload_wq_started) {
		return 0;
	}

	k_work_queue_init(&upload_wq);
	k_work_queue_start(&upload_wq, upload_stack,
			   K_THREAD_STACK_SIZEOF(upload_stack),
			   UPLOAD_WQ_PRIORITY, NULL);
	k_thread_name_set(&upload_wq.thread, "http_upload");
	k_work_init(&upload_work, upload_work_fn);
	upload_wq_started = true;

#if CONFIG_CLIP_UPLOAD_INTERVAL_MIN > 0
	k_work_init_delayable(&periodic_work, periodic_work_fn);
	/* La primera pasada no es inmediata: al arrancar todavia no hay red, y
	 * ademas conviene que una grabacion recien empezada tenga tiempo de cerrar
	 * su primer trozo. */
	k_work_schedule_for_queue(&upload_wq, &periodic_work, K_MINUTES(2));
#endif

	return 0;
}

int http_upload_session_async(const char *session_id)
{
	if (!session_id || session_id[0] == '\0') {
		return -EINVAL;
	}
	if (!upload_wq_started) {
		return -ENODEV;
	}
	if (config_get_upload_host()[0] == '\0' || config_get_upload_port() == 0) {
		return -ENOENT;
	}
	if (!wifi_sta_is_connected()) {
		return -ENETDOWN;
	}

	k_mutex_lock(&status_lock, K_FOREVER);
	if (status.state == HTTP_UPLOAD_RUNNING) {
		k_mutex_unlock(&status_lock);
		return -EBUSY;
	}
	strncpy(pending_session, session_id, sizeof(pending_session) - 1);
	pending_session[sizeof(pending_session) - 1] = '\0';
	/* Claimed here, not in the work item: otherwise two commands arriving
	 * close together both see IDLE and both queue. */
	status.state = HTTP_UPLOAD_RUNNING;
	k_mutex_unlock(&status_lock);

	k_work_submit_to_queue(&upload_wq, &upload_work);
	return 0;
}

void http_upload_get_status(struct http_upload_status *out)
{
	if (!out) {
		return;
	}

	k_mutex_lock(&status_lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&status_lock);
}
