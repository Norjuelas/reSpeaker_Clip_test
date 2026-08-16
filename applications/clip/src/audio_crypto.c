/*
 * Cifrado del audio en reposo — AES-128-GCM por trozos sobre PSA.
 * Ver audio_crypto.h para el formato y el porqué del troceado.
 *
 * La criptografía no añade FLASH: PSA con AES-GCM ya está en la imagen
 * porque el supplicant WPA2 y TLS la traen (Doc 09 §2).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include <string.h>

#include "audio_crypto.h"
#include "config.h"
#include "clip.h"

LOG_MODULE_REGISTER(audio_crypto, CONFIG_CLIP_LOG_LEVEL);

#define BPE2_MAGIC   "BPE2"
#define BPE2_VERSION 0x02
#define BPE2_KEYID   0x00 /* rotación futura; hoy una sola clave por device */
#define GCM_TAG_LEN  16
#define GCM_NONCE_LEN 12

/* Un fichero de audio a la vez (current_file_ptr en storage.c es único), así
 * que un solo buffer de salida estático basta. +16 por la etiqueta GCM. */
static uint8_t crypt_buffer[CONFIG_CLIP_STORAGE_CHUNK_SIZE + GCM_TAG_LEN];

/* La clave PSA se importa una vez y se cachea: importarla por fichero
 * gastaría un slot volátil por grabación hasta agotar los 32. */
static psa_key_id_t g_key;
static bool g_key_loaded;

static bool key_is_set(const uint8_t *key)
{
	uint8_t acc = 0;

	for (int i = 0; i < 16; i++) {
		acc |= key[i];
	}
	return acc != 0; /* todo-ceros = sin provisionar (AT+KEYCFG lo rechaza) */
}

bool audio_crypto_enabled(void)
{
	return IS_ENABLED(CONFIG_CLIP_AUDIO_ENCRYPT) &&
	       key_is_set(clip_get_context()->config.audio_key);
}

void audio_crypto_key_changed(void)
{
	if (g_key_loaded) {
		psa_destroy_key(g_key);
		g_key_loaded = false;
	}
}

static int load_key(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t st;

	if (g_key_loaded) {
		return 0;
	}

	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init: %d", (int)st);
		return -EIO;
	}

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_GCM);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);

	st = psa_import_key(&attr, clip_get_context()->config.audio_key, 16,
			    &g_key);
	if (st != PSA_SUCCESS) {
		LOG_ERR("import de la clave de audio: %d", (int)st);
		return -EIO;
	}

	g_key_loaded = true;
	return 0;
}

int audio_crypto_begin(struct audio_crypto_ctx *ctx, struct fs_file_t *f)
{
	uint8_t header[4 + 1 + 1 + GCM_NONCE_LEN];
	psa_status_t st;
	ssize_t written;
	int ret;

	ctx->active = false;
	ctx->counter = 0;

	if (!audio_crypto_enabled()) {
		/* Sin clave el audio queda en claro. Se avisa una vez, no por
		 * fichero: es la distincion cifra-o-no-cifra del arranque, no
		 * ruido de cada grabacion. */
		static bool warned;

		if (!warned) {
			LOG_WRN("Audio SIN cifrar en la tarjeta: no hay clave (AT+KEYCFG)");
			warned = true;
		}
		return 0;
	}

	ret = load_key();
	if (ret) {
		return ret;
	}

	st = psa_generate_random(ctx->nonce_base, GCM_NONCE_LEN);
	if (st != PSA_SUCCESS) {
		/* Sin aleatoriedad no hay nonce seguro, y un nonce repetido
		 * rompe GCM por completo. Fallar cerrado. */
		LOG_ERR("nonce: %d", (int)st);
		return -EIO;
	}

	memcpy(header, BPE2_MAGIC, 4);
	header[4] = BPE2_VERSION;
	header[5] = BPE2_KEYID;
	memcpy(&header[6], ctx->nonce_base, GCM_NONCE_LEN);

	written = fs_write(f, header, sizeof(header));
	if (written != (ssize_t)sizeof(header)) {
		LOG_ERR("cabecera BPE2: %d", (int)written);
		return -EIO;
	}

	ctx->active = true;
	return 0;
}

int audio_crypto_write(struct audio_crypto_ctx *ctx, struct fs_file_t *f,
		       const uint8_t *data, size_t len)
{
	uint8_t nonce[GCM_NONCE_LEN];
	uint8_t len_hdr[2];
	size_t out_len = 0;
	psa_status_t st;
	ssize_t written;

	if (!ctx->active) {
		return -EINVAL;
	}
	if (len == 0) {
		return 0;
	}
	if (len > CONFIG_CLIP_STORAGE_CHUNK_SIZE) {
		return -EMSGSIZE;
	}

	/* Nonce del trozo: base con el contador XOR en los últimos 4 bytes.
	 * Base aleatoria por fichero + contador distinto por trozo = nunca se
	 * repite (nonce, clave), que es lo único que GCM no perdona. */
	memcpy(nonce, ctx->nonce_base, GCM_NONCE_LEN);
	nonce[8] ^= (uint8_t)(ctx->counter >> 24);
	nonce[9] ^= (uint8_t)(ctx->counter >> 16);
	nonce[10] ^= (uint8_t)(ctx->counter >> 8);
	nonce[11] ^= (uint8_t)(ctx->counter);

	st = psa_aead_encrypt(g_key, PSA_ALG_GCM, nonce, GCM_NONCE_LEN,
			      NULL, 0, data, len,
			      crypt_buffer, sizeof(crypt_buffer), &out_len);
	if (st != PSA_SUCCESS) {
		LOG_ERR("aead_encrypt trozo %u: %d", ctx->counter, (int)st);
		return -EIO;
	}

	len_hdr[0] = (uint8_t)(out_len & 0xff);
	len_hdr[1] = (uint8_t)(out_len >> 8);

	written = fs_write(f, len_hdr, 2);
	if (written != 2) {
		return -EIO;
	}
	written = fs_write(f, crypt_buffer, out_len);
	if (written != (ssize_t)out_len) {
		return -EIO;
	}

	ctx->counter++;
	return 0;
}
