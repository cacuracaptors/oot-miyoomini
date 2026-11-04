/*
 * swapmap.h -- aplica o MAPA DE CONVERSAO (gerado por tools/oot-assets.py) ao cartucho virtual.
 *
 * Os dados da ROM sao big-endian. O jogo compilado para ARM le structs, u16, u32 e f32 em
 * little-endian. O mapa diz, para cada meia-palavra (2 bytes) de cada arquivo de assets,
 * se ela fica como esta (0), se troca 2 bytes (1) ou se faz parte de uma palavra de 4 bytes
 * a inverter (2 = 1a metade, 3 = 2a metade). Texturas, textos, JPEG, sequencias e amostras
 * de audio ficam como bytes originais (codigo 0): o renderizador e o audio os leem como o N64.
 *
 * Formato do arquivo (oot_swapmap.bin):
 *   "OOTSWP1\0"  u32 tamanho_descomprimido  u32 crc32_do_descomprimido  <DEFLATE cru>
 * descomprimido: u32 nfiles, depois por arquivo:
 *   u32 vromStart  u32 nbytes  u32 nhalfwords  bytes[(nhalfwords+3)/4]  (2 bits por meia-palavra)
 */
#ifndef SWAPMAP_H
#define SWAPMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t vrom_start, vrom_end; /* arquivo na ROM (espaco VROM) */
    uint32_t cart_off;             /* onde ele esta no cartucho */
} SwapFile;

/* Le e descomprime o mapa. *raw e alocado (free()). *id = crc32 (identifica a versao do mapa). */
int swapmap_load(const char* path, uint8_t** raw, size_t* rawlen, uint32_t* id, char* err, size_t errsz);

/* So le o id (cabecalho) sem descomprimir. Devolve 0 se ok. */
int swapmap_read_id(const char* path, uint32_t* id);

/* Aplica o mapa em cart (na memoria). files = os arquivos do dmadata. Devolve 0 se ok. */
int swapmap_apply(const uint8_t* raw, size_t rawlen, uint8_t* cart, size_t cartsz, const SwapFile* files,
                  size_t nfiles, char* err, size_t errsz);

#endif
