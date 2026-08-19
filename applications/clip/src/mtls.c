/* Credenciales de cliente del device para mTLS (Doc 23 Fase 2).
 *
 * El problema que cierra: hoy el servicio identifica al device por la cabecera
 * X-Device-Id, que es texto que cualquiera puede escribir. Quien alcance el
 * endpoint puede subir audio haciendose pasar por un Clip, o mandar latidos
 * falsos. Con certificado de cliente, la identidad la da la criptografia.
 *
 * DONDE VIVEN LAS CREDENCIALES, y por que ahi:
 *
 *   /lfs/mtls/cert.pem  y  /lfs/mtls/key.pem   — flash INTERNA
 *
 * No en la microSD: la tarjeta se saca con la mano y se lee en cualquier
 * portatil. No en `settings`: el backend de fichero tiene un limite de lineas
 * y un PEM son cientos de bytes.
 *
 * LO QUE ESTO TODAVIA NO ES: la clave privada se genera fuera y entra por la
 * tarjeta, asi que existe fuera del device durante la provision. El paso
 * siguiente (Doc 23 §5) es generarla en el CryptoCell y que no salga nunca del
 * chip; entonces esta ruta se queda solo para el certificado.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/tls_credentials.h>
#include <string.h>
#include <errno.h>

#include "mtls.h"
#include "storage.h"

LOG_MODULE_REGISTER(mtls, CONFIG_CLIP_LOG_LEVEL);

#define DIR_INT      "/lfs/mtls"
#define CERT_INT     DIR_INT "/cert.pem"
#define KEY_INT      DIR_INT "/key.pem"
#define CERT_SD      "/SD:/device.crt"
#define KEY_SD       "/SD:/device.key"

#define CERT_MAX     2048
#define KEY_MAX      1024

/* Mismo motivo que con la CA: tls_credential_add() se queda con el puntero sin
 * copiar. Liberar estos buffers deja la credencial apuntando a memoria libre,
 * y el sintoma es un fallo intermitente que empeora con el uso. */
static uint8_t *cert_buf;
static uint8_t *key_buf;
static bool loaded;

static int read_whole(const char *path, uint8_t **out, size_t *out_len,
		      size_t max)
{
	struct fs_file_t f;
	uint8_t *buf;
	ssize_t n;
	int ret;

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret) {
		return ret;
	}

	buf = k_malloc(max);
	if (!buf) {
		fs_close(&f);
		return -ENOMEM;
	}

	n = fs_read(&f, buf, max - 1);
	fs_close(&f);

	if (n <= 0) {
		k_free(buf);
		return -EIO;
	}

	/* El NUL cuenta: mbedTLS exige que la longitud de un PEM lo incluya.
	 * Sin el, el parseo falla con un error que no menciona el formato. */
	buf[n] = '\0';
	*out = buf;
	*out_len = (size_t)n + 1;
	return 0;
}

static int write_whole(const char *path, const uint8_t *data, size_t len)
{
	struct fs_file_t f;
	ssize_t n;
	int ret;

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret) {
		return ret;
	}
	n = fs_write(&f, data, len);
	fs_close(&f);
	return (n == (ssize_t)len) ? 0 : -EIO;
}

bool mtls_present(void)
{
	struct fs_dirent e;

	return fs_stat(CERT_INT, &e) == 0 && fs_stat(KEY_INT, &e) == 0;
}

int mtls_load_credentials(void)
{
	size_t cert_len, key_len;
	int ret;

	if (loaded) {
		return 0;
	}
	if (!mtls_present()) {
		return -ENOENT;
	}

	ret = read_whole(CERT_INT, &cert_buf, &cert_len, CERT_MAX);
	if (ret) {
		LOG_ERR("no se pudo leer el certificado de cliente: %d", ret);
		return ret;
	}
	ret = read_whole(KEY_INT, &key_buf, &key_len, KEY_MAX);
	if (ret) {
		LOG_ERR("no se pudo leer la clave de cliente: %d", ret);
		k_free(cert_buf);
		cert_buf = NULL;
		return ret;
	}

	/* En Zephyr, el certificado que presenta un cliente se registra como
	 * SERVER_CERTIFICATE: el tipo nombra el rol dentro del handshake, no
	 * quien lo usa. Confunde la primera vez que se lee. */
	ret = tls_credential_add(CONFIG_CLIP_MTLS_SEC_TAG,
				 TLS_CREDENTIAL_SERVER_CERTIFICATE,
				 cert_buf, cert_len);
	if (ret && ret != -EEXIST) {
		LOG_ERR("registro del certificado de cliente: %d", ret);
		goto fallo;
	}

	ret = tls_credential_add(CONFIG_CLIP_MTLS_SEC_TAG,
				 TLS_CREDENTIAL_PRIVATE_KEY,
				 key_buf, key_len);
	if (ret && ret != -EEXIST) {
		LOG_ERR("registro de la clave de cliente: %d", ret);
		goto fallo;
	}

	loaded = true;
	LOG_INF("mTLS: credenciales de cliente registradas (cert %u B, clave %u B)",
		(unsigned int)cert_len, (unsigned int)key_len);
	return 0;

fallo:
	k_free(cert_buf);
	k_free(key_buf);
	cert_buf = NULL;
	key_buf = NULL;
	return ret;
}

int mtls_provision_from_sd(void)
{
	uint8_t *cert = NULL, *key = NULL;
	size_t cert_len, key_len;
	int ret;

	if (storage_ensure_mounted() != 0) {
		return -ENODEV;
	}

	ret = read_whole(CERT_SD, &cert, &cert_len, CERT_MAX);
	if (ret) {
		LOG_ERR("falta %s (%d)", CERT_SD, ret);
		return ret;
	}
	ret = read_whole(KEY_SD, &key, &key_len, KEY_MAX);
	if (ret) {
		LOG_ERR("falta %s (%d)", KEY_SD, ret);
		k_free(cert);
		return ret;
	}

	/* Comprobacion minima de formato antes de guardar nada: instalar basura
	 * dejaria el device sin subir y con un error de TLS que no menciona el
	 * fichero. */
	if (memcmp(cert, "-----BEGIN", 10) != 0 ||
	    memcmp(key, "-----BEGIN", 10) != 0) {
		LOG_ERR("cert o clave no parecen PEM");
		ret = -EINVAL;
		goto salir;
	}

	(void)fs_mkdir(DIR_INT);

	/* La clave PRIMERO y el certificado despues: si se corta la luz en medio,
	 * lo que queda es una clave sin certificado, que mtls_present() rechaza.
	 * Al reves quedaria un certificado sin clave, que TLS acepta registrar y
	 * falla en el handshake con un error mucho mas oscuro. */
	ret = write_whole(KEY_INT, key, key_len - 1);
	if (ret) {
		goto salir;
	}
	ret = write_whole(CERT_INT, cert, cert_len - 1);
	if (ret) {
		fs_unlink(KEY_INT);
		goto salir;
	}

	/* Y fuera de la tarjeta. Una clave privada en un medio extraible deja de
	 * ser privada en cuanto alguien saca la tarjeta — que es el escenario
	 * para el que existe todo esto. */
	if (fs_unlink(KEY_SD) != 0) {
		LOG_ERR("LA CLAVE SIGUE EN LA TARJETA (%s): borrala a mano", KEY_SD);
	}
	(void)fs_unlink(CERT_SD);

	LOG_WRN("mTLS: credenciales instaladas en la flash interna y borradas "
		"de la tarjeta");

salir:
	k_free(cert);
	k_free(key);
	return ret;
}

int mtls_wipe(void)
{
	int a = fs_unlink(CERT_INT);
	int b = fs_unlink(KEY_INT);

	/* Los buffers ya registrados en la pila TLS no se pueden retirar sin
	 * reiniciar; lo que se destruye es la copia persistente, que es la que
	 * sobrevive al apagado. */
	loaded = false;
	LOG_WRN("mTLS: credenciales de cliente destruidas");

	if (a && a != -ENOENT) {
		return a;
	}
	if (b && b != -ENOENT) {
		return b;
	}
	return 0;
}
