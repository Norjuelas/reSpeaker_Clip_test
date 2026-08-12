/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pushes recordings to an HTTP endpoint.
 *
 * Plain HTTP for now. The audio is conversation recordings, so shipping this to
 * devices in the field without TLS is not defensible — see Doc 12 for the fleet
 * CA and mTLS plan. The room for TLS already exists: the nRF70 firmware moving
 * out of the image left ~52KB free, and TLS measures ~45KB.
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

#include "http_upload.h"
#include "config.h"
#include "storage.h"
#include "wifi.h"

LOG_MODULE_REGISTER(http_upload, CONFIG_CLIP_LOG_LEVEL);

/* Enough for the response headers; the body we get back is a short status. */
#define RECV_BUF_SIZE   512
#define SEND_CHUNK      1024
#define HTTP_TIMEOUT_MS 15000
#define CONNECT_TIMEOUT_MS 5000

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

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket failed: %d", -errno);
		return -errno;
	}

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
		LOG_ERR("connect to %s:%u failed: %d", host, port, -errno);
		zsock_close(sock);
		return -errno;
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
