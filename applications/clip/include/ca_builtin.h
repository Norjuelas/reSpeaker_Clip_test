/* CA de flota compilada dentro de la imagen firmada. Ver src/ca_builtin.c. */
#ifndef CLIP_CA_BUILTIN_H_
#define CLIP_CA_BUILTIN_H_

#include <stdbool.h>
#include <stddef.h>

extern const char clip_ca_builtin_pem[];
extern const size_t clip_ca_builtin_len;

/** ¿Hay una CA de verdad compilada, o el fichero sigue con el hueco vacio? */
bool clip_ca_builtin_present(void);

#endif /* CLIP_CA_BUILTIN_H_ */
