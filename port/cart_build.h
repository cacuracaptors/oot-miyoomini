/*
 * cart_build.h -- cria o "cartucho virtual" a partir da ROM do usuario.
 *
 * Roda UMA vez (primeira execucao). Formato do cartucho virtual:
 *   [0 .. romSize)         a ROM original (z64), na MESMA posicao: os arquivos nao
 *                          comprimidos (audio etc.) continuam onde o jogo espera
 *   [romSize .. fim)       os arquivos que eram comprimidos (Yaz0), ja descomprimidos
 *   dmadata                reescrita em formato nativo (little-endian), apontando
 *                          tudo como "nao comprimido"
 *   dados de assets        convertidos para little-endian com o mapa oot_swapmap.bin
 *                          (texturas/sons/textos continuam em bytes originais)
 *   ultimos 32 bytes       carimbo (CART_MAGIC, versao@16, tamanho@20, id do mapa@24)
 */
#ifndef CART_BUILD_H
#define CART_BUILD_H

#include <stddef.h>
#include <stdint.h>

#define CART_FORMAT_VERSION 2u
#define CART_MAGIC "OOTMIYOO-CART"
#define CART_TRAILER_SIZE 32

/* percent: 0..100 da etapa atual. Pode ser NULL. */
typedef void (*CartProgress)(void* ud, const char* stage, int percent);

/* Devolve 0 se deu certo. Em caso de erro devolve != 0 e escreve uma mensagem
 * (em portugues, pronta para mostrar na tela) em err. */
int cart_build(const char* rom_path, const char* out_path, const char* swapmap_path, CartProgress cb, void* ud,
               char* err, size_t errsz);

/* 1 se o arquivo existe, tem o carimbo desta versao e foi feito com este mapa. */
int cart_is_valid(const char* path, const char* swapmap_path);

/* Descompressor Yaz0 (exposto para testes). Devolve 0 se ok. */
int yaz0_decode(const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstlen);

#endif
