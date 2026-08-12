/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_HTTP_UPLOAD_H
#define CLIP_HTTP_UPLOAD_H

#include <stddef.h>

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

#endif /* CLIP_HTTP_UPLOAD_H */
