/*
 * Copyright (c) 2026 B·Pin
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_UPLOAD_REGISTRY_H
#define CLIP_UPLOAD_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Si ese fichero de esa sesion ya se subio
 *
 * Falso tambien cuando no se puede leer el registro: ante la duda se resube.
 * Duplicar audio es molesto; perderlo, no.
 */
bool upload_registry_has(const char *session, uint32_t idx);

/**
 * @brief Anotar un fichero como subido
 *
 * Sincroniza a la tarjeta antes de volver: si el device muere justo despues,
 * la marca tiene que haber sobrevivido.
 */
int upload_registry_mark(const char *session, uint32_t idx);

/**
 * @brief Vaciar el registro — todo se volvera a subir
 */
int upload_registry_reset(void);

#endif /* CLIP_UPLOAD_REGISTRY_H */
