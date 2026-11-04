/*
 * jpeg_hle.c -- o microcodigo de JPEG do Ocarina of Time (njpgdspMain) rodando em C.
 *
 * Os fundos pre-renderizados (casas de Kokiri, a casa do Link, a praca do mercado, lojas...)
 * sao JPEGs. A CPU do jogo le os cabecalhos e decodifica o Huffman (z_jpeg.c, jpegdecoder.c):
 * o resultado sao os coeficientes de cada bloco 8x8, 4 macroblocos de cada vez. O RSP faz o
 * resto: tira a quantizacao, desfaz o zigue-zague, aplica a IDCT e converte YUV -> RGBA5551,
 * escrevendo o bloco 16x16 de pixels por cima dos proprios coeficientes.
 *
 * Mesma conta do HLE do mupen64plus (rsp-hle, jpeg.c, jpeg_decode_PS), com a memoria em
 * ordem natural: coeficientes e tabelas sao u16 nativos (escritos pela CPU do jogo), e os
 * pixels de saida sao gravados em big-endian, como textura do N64 (o renderizador le assim).
 */
#include <stdint.h>
#include <string.h>

#include "os_hw.h"

#define SB 64

static const unsigned ZIGZAG[SB] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
    41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
    46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

static const float IDCT_C3 = 1.175875602f;
static const float IDCT_C6 = 0.541196100f;
static const float IDCT_K[10] = {
    0.765366865f,  -1.847759065f, -0.390180644f, -1.961570561f, 1.501321110f,
    2.053119869f,  3.072711027f,  0.298631336f,  -0.899976223f, -2.562915448f,
};

static inline int16_t clamp_s16(int32_t v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

static void idct1d(const float* x, float* dst, unsigned stride) {
    float e[4], f[4];
    float x15 = IDCT_K[2] * (x[1] + x[5]);
    float x37 = IDCT_K[3] * (x[3] + x[7]);
    float x17 = IDCT_K[8] * (x[1] + x[7]);
    float x35 = IDCT_K[9] * (x[3] + x[5]);
    float x1357 = IDCT_C3 * (x[1] + x[3] + x[5] + x[7]);
    float x26 = IDCT_C6 * (x[2] + x[6]);
    f[0] = x[0] + x[4];
    f[1] = x[0] - x[4];
    f[2] = x26 + IDCT_K[0] * x[2];
    f[3] = x26 + IDCT_K[1] * x[6];
    e[0] = x1357 + x15 + IDCT_K[4] * x[1] + x17;
    e[1] = x1357 + x37 + IDCT_K[6] * x[3] + x35;
    e[2] = x1357 + x15 + IDCT_K[5] * x[5] + x35;
    e[3] = x1357 + x37 + IDCT_K[7] * x[7] + x17;
    dst[0] = f[0] + f[2] + e[0];
    dst[stride] = f[1] + f[3] + e[1];
    dst[2 * stride] = f[1] - f[3] + e[2];
    dst[3 * stride] = f[0] - f[2] + e[3];
    dst[4 * stride] = f[0] - f[2] - e[3];
    dst[5 * stride] = f[1] - f[3] - e[2];
    dst[6 * stride] = f[1] + f[3] - e[1];
    dst[7 * stride] = f[0] + f[2] - e[0];
}

static void idct_block(int16_t* dst, const int16_t* src) {
    float x[8], block[SB];
    unsigned i, j;
    for (i = 0; i < 8; ++i) {
        for (j = 0; j < 8; ++j) {
            x[j] = (float)src[i * 8 + j];
        }
        idct1d(x, &block[i], 8);
    }
    for (i = 0; i < 8; ++i) {
        idct1d(&block[i * 8], x, 1);
        for (j = 0; j < 8; ++j) {
            dst[i + j * 8] = (int16_t)((int16_t)x[j] >> 3);
        }
    }
}

static inline uint16_t clamp_comp(int16_t x) {
    if (x > 0xff0) {
        x = 0xff0;
    } else if (x < 0) {
        x = 0;
    }
    return (uint16_t)(x & 0xf80);
}

/* pixel RGBA5551 ja com os bytes na ordem do N64 (big-endian na memoria) */
static inline uint16_t rgba_be(int16_t y, int16_t u, int16_t v) {
    const float fY = (float)y + 2048.0f, fU = (float)u, fV = (float)v;
    uint16_t r = clamp_comp((int16_t)(fY + 1.4025 * fV));
    uint16_t g = clamp_comp((int16_t)(fY - 0.3443 * fU - 0.7144 * fV));
    uint16_t b = clamp_comp((int16_t)(fY + 1.7729 * fU));
    uint16_t p = (uint16_t)((r << 4) | (g >> 1) | (b >> 6) | 1);
    return (uint16_t)((p >> 8) | (p << 8));
}

static void emit_line(const int16_t* y, const int16_t* u, uint16_t* out) {
    const int16_t* v = u + SB;
    const int16_t* y2 = y + SB;
    int k;
    for (k = 0; k < 4; k++) {
        out[2 * k] = rgba_be(y[2 * k], u[k], v[k]);
        out[2 * k + 1] = rgba_be(y[2 * k + 1], u[k], v[k]);
        out[8 + 2 * k] = rgba_be(y2[2 * k], u[4 + k], v[4 + k]);
        out[8 + 2 * k + 1] = rgba_be(y2[2 * k + 1], u[4 + k], v[4 + k]);
    }
}

#ifdef JPEG_HLE_TEST
#define phys(a) ((void*)(uintptr_t)(a)) /* teste: enderecos comuns */
#else
static inline void* phys(uint32_t a) { /* endereco fisico do N64 -> RAM mapeada em 0x80000000 */
    return (void*)(uintptr_t)(0x80000000u | (a & 0x7fffffffu));
}
#endif

int jpeg_hle_run(const uint32_t* taskData) {
    uint32_t address = taskData[0], count = taskData[1], mode = taskData[2];
    int16_t q[3][SB];
    int16_t mb[6 * SB];
    unsigned nsb, sb, i, m;
    if (mode != 0 && mode != 2) {
        return 0;
    }
    memcpy(q[0], phys(taskData[3]), sizeof q[0]);
    memcpy(q[1], phys(taskData[4]), sizeof q[1]);
    memcpy(q[2], phys(taskData[5]), sizeof q[2]);
    nsb = mode + 4;
    for (m = 0; m < count; m++) {
        uint8_t* mem = (uint8_t*)phys(address);
        uint16_t line[16];
        unsigned qi = 0, yo, uo;
        memcpy(mb, mem, nsb * SB * 2);
        for (sb = 0; sb < nsb; sb++) {
            int16_t* blk = mb + sb * SB;
            int16_t tmp[SB];
            if (nsb - sb <= 2) {
                ++qi; /* croma: tabelas U e V */
            }
            for (i = 0; i < SB; i++) {
                blk[i] = (int16_t)(clamp_s16((int32_t)blk[i] * q[qi][i]) << 4);
            }
            for (i = 0; i < SB; i++) {
                tmp[i] = blk[ZIGZAG[i]];
            }
            idct_block(blk, tmp);
        }
        if (mode == 0) { /* 16x8 */
            yo = 0;
            uo = 2 * SB;
            for (i = 0; i < 8; i++) {
                emit_line(&mb[yo], &mb[uo], line);
                memcpy(mem + i * 32, line, 32);
                yo += 8;
                uo += 8;
            }
        } else { /* 16x16 */
            yo = 0;
            uo = 4 * SB;
            for (i = 0; i < 8; i++) {
                emit_line(&mb[yo], &mb[uo], line);
                memcpy(mem + i * 64, line, 32);
                emit_line(&mb[yo + 8], &mb[uo], line);
                memcpy(mem + i * 64 + 32, line, 32);
                yo += (i == 3) ? SB + 16 : 16;
                uo += 8;
            }
        }
        address += 2 * nsb * SB;
    }
    return 1;
}

#ifndef JPEG_HLE_TEST
#include <stdio.h>
static int hook_jpeg_task(OSTask* t) {
    const uint32_t* d = (const uint32_t*)t->t.data_ptr;
    if (d == NULL || t->t.data_size < 24) {
        static int warned;
        if (!warned++) {
            fprintf(stderr, "[jpeg] tarefa sem dados (data_ptr=%p, tamanho %u): ignorada\n", (void*)d,
                    (unsigned)t->t.data_size);
        }
        return 1;
    }
    jpeg_hle_run(d);
    return 1;
}

void jpeg_hle_install(void) {
    port_hooks.jpeg_task = hook_jpeg_task;
}
#endif
