/*
 * Cifrado del audio en reposo (Doc 09) — AES-128-GCM por trozos.
 *
 * Formato BPE2 en disco:
 *   cabecera (18 B): "BPE2" | version 0x02 | keyid | nonce_base (12 B)
 *   trozo:           u16-LE len | ciphertext+tag (len bytes, tag = últimos 16)
 *
 * Cada flush del buffer de escritura es un trozo cifrado y autenticado con
 * nonce propio (nonce_base con el contador XOR en los últimos 4 bytes). Por
 * trozos y no de una pieza a propósito: si la batería muere a mitad de
 * fichero, con una sola etiqueta al final se pierde el fichero entero; así
 * solo se pierde el trozo incompleto. Y el GCM de una pieza (one-shot) está
 * garantizado en el acelerador; el multiparte no.
 */

#ifndef CLIP_AUDIO_CRYPTO_H
#define CLIP_AUDIO_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <zephyr/fs/fs.h>

struct audio_crypto_ctx {
	bool active;
	uint32_t counter;
	uint8_t nonce_base[12];
};

/** true si hay clave provisionada (AT+KEYCFG) y el cifrado va a aplicarse. */
bool audio_crypto_enabled(void);

/** Invalida la clave PSA cacheada. Llamar cuando AT+KEYCFG cambie la clave. */
void audio_crypto_key_changed(void);

/** Escribe la cabecera BPE2 y prepara el contexto. Falla cerrado: si no se
 *  puede cifrar habiendo clave, el fichero no se crea en claro. */
int audio_crypto_begin(struct audio_crypto_ctx *ctx, struct fs_file_t *f);

/** Cifra `len` bytes como UN trozo y lo escribe (prefijo u16 + ct + tag). */
int audio_crypto_write(struct audio_crypto_ctx *ctx, struct fs_file_t *f,
		       const uint8_t *data, size_t len);

#endif /* CLIP_AUDIO_CRYPTO_H */
