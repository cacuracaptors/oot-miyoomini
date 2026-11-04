/* inflate.h -- descompressor DEFLATE "cru" (RFC 1951), pequeno, so o que o port precisa.
 * Baseado no algoritmo do puff.c de Mark Adler (dominio publico), reescrito. */
#ifndef PORT_INFLATE_H
#define PORT_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Descomprime src[0..srclen) para dst[0..dstlen). Devolve 0 se deu certo e o fluxo
 * produziu exatamente dstlen bytes; != 0 em erro. */
int port_inflate(const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstlen);

#endif
