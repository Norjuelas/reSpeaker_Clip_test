/* Credenciales de cliente del device (mTLS) — Doc 23 Fase 2.
 *
 * Con esto el servicio deja de creerse la cabecera X-Device-Id, que cualquiera
 * puede escribir, y pasa a saber QUE device habla porque presenta un
 * certificado firmado por la CA de flota.
 */
#ifndef CLIP_MTLS_H_
#define CLIP_MTLS_H_

#include <stdbool.h>

/** ¿Hay certificado y clave de cliente instalados en la flash interna? */
bool mtls_present(void);

/**
 * @brief Registra el certificado y la clave de cliente en la pila TLS.
 *
 * Idempotente: solo carga una vez. Los buffers se quedan vivos a proposito —
 * tls_credential_add() guarda el puntero sin copiar.
 *
 * @return 0 si quedaron registrados, -ENOENT si no hay credenciales.
 */
int mtls_load_credentials(void);

/**
 * @brief Instala el certificado y la clave desde la microSD.
 *
 * Lee /SD:/device.crt y /SD:/device.key, los copia a la flash interna y BORRA
 * las copias de la tarjeta: una clave privada en un medio extraible deja de
 * ser privada en cuanto alguien saca la tarjeta.
 *
 * Es la via de provision porque un PEM son ~700 bytes y la linea AT admite
 * 256 — por el canal de comandos no cabe.
 *
 * @return 0, o negativo si falta alguno de los dos ficheros.
 */
int mtls_provision_from_sd(void);

/** Destruye las credenciales de cliente. Lo usa config_wipe_secrets(). */
int mtls_wipe(void);

#endif /* CLIP_MTLS_H_ */
