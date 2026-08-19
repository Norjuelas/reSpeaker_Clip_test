/* CA de flota compilada dentro de la imagen firmada.
 *
 * Por que aqui y no en /SD:/ca.pem: la tarjeta es escribible por USB con el
 * device en la mano. Quien la sustituya redirige el device a un servicio
 * impostor que si valida. Compilada, cambiarla exige firmar firmware nuevo,
 * que exige la clave de firma, que esta fuera de linea.
 *
 * COMO SE RELLENA (no se edita a mano):
 *
 *     curl -sk https://<ip-del-servicio>/v1/ca/root.pem \
 *       | python3 applications/clip/tools/gen_ca_builtin.py
 *
 * Deja raiz + intermedia. Las dos a proposito: si un dia el servidor no manda
 * la intermedia en el handshake — un fallo de despliegue habitual — el device
 * sigue validando en vez de fallar con un error de TLS que cuesta un dia
 * diagnosticar.
 *
 * Mientras este vacio, el device cae a la CA de la tarjeta si
 * CLIP_SECURITY_CA_FROM_SD sigue activo, y si no, no sube.
 */

#include <stdbool.h>
#include <stddef.h>

/* @@BPIN_CA_BEGIN@@ — todo lo que hay entre las marcas lo reescribe el script */
const char clip_ca_builtin_pem[] = "";
/* @@BPIN_CA_END@@ */

const size_t clip_ca_builtin_len = sizeof(clip_ca_builtin_pem);

bool clip_ca_builtin_present(void)
{
	/* sizeof cuenta el NUL: una cadena vacia son 1 byte, no 0. Y un PEM de
	 * verdad empieza por la cabecera — comprobarlo evita que un fichero a
	 * medio generar pase por CA valida. */
	return clip_ca_builtin_len > 32 &&
	       clip_ca_builtin_pem[0] == '-' && clip_ca_builtin_pem[1] == '-';
}
