/*
 * savedir.h -- a pasta dos saves: a do perfil do OnionOS, ou ./saves (ver savedir.c).
 */
#ifndef SAVEDIR_H
#define SAVEDIR_H

#include <stddef.h>

#define PORT_PATH_MAX 256

/* A pasta (sem a barra no fim). Resolvida, e migrada de ./saves, na primeira chamada: feita pela thread principal ao
 * abrir, antes das outras threads; depois disso ela nao muda. */
const char* port_save_dir(void);

/* out = <pasta>/<name>. Devolve 0 se nao coube. */
int port_save_path(char* out, size_t size, const char* name);

#endif
