/*
 * audio_hle.h -- o microcodigo de audio do Ocarina of Time (ABI "nead") rodando em C.
 *
 * O jogo monta uma lista de comandos de audio (src/audio/internal/synthesis.c) e manda
 * para o RSP. Aqui cada comando e executado direto na CPU, sobre uma DMEM simulada de
 * 4 KB. Mesmo comportamento do HLE do mupen64plus (rsp-hle, alist_nead.c / alist.c),
 * so que com a memoria em ordem natural: as amostras de 16 bits ficam em endian nativo
 * na RAM e na DMEM (o jogo, as soundfonts convertidas e a saida para o AI concordam).
 */
#ifndef AUDIO_HLE_H
#define AUDIO_HLE_H

#include <stdint.h>

/* Onde comeca a RAM do N64 neste processo: um endereco de comando "a" vira
 * audio_hle_ram + (a & 0xFFFFFF). No jogo e 0x80000000 (a RAM do N64 e o executavel
 * ficam ali); nos testes e um bloco de memoria qualquer. */
extern uint8_t* audio_hle_ram;

/* Executa a lista de comandos (nbytes = tamanho em bytes, 8 por comando). */
void audio_hle_run(const uint32_t* alist, uint32_t nbytes);

/* Comandos desconhecidos vistos ate agora (diagnostico). */
extern unsigned audio_hle_unknown;

/* Resumo desde o ultimo relatorio (diagnostico do som mudo): comandos e picos de cada etapa. */
void audio_hle_report(char* buf, int size);

#endif
