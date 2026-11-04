/*
 * gfx_capture.c -- captura de um quadro real para reproduzir fora do jogo (oot_replay).
 *
 * Arquivo "OOTCAP1": cabecalho + estado do RDP/RSP (GxCapState) + as paginas de 4 KB da memoria
 * que a display list leu (comandos, vertices, matrizes, texturas, paletas) e os buffers de cor e
 * profundidade, com o conteudo de ANTES do quadro. O reprodutor mapeia a memoria no mesmo
 * endereco (0x80000000), restaura tudo e chama gx_render: o resultado tem que ser identico.
 *
 * Formato (little-endian):
 *   char magic[8] = "OOTCAP1\0"
 *   u32 dlAddr, u32 ramEnd, u32 capStateSize, u32 npages
 *   GxCapState
 *   npages x { u32 addr; u8 data[4096]; }
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "gfx_priv.h"

#define CAP_BASE 0x80000000u
#define CAP_PAGES 4096u /* 16 MB */

int gx_capturing;
static uint8_t g_touched[CAP_PAGES];

void gx_touch_slow(const void *p, size_t n) {
    uint32_t a = (uint32_t)(uintptr_t)p, e, pg;
    if (n == 0 || a < CAP_BASE) {
        return;
    }
    e = a + (uint32_t)n - 1;
    if (e < a) {
        e = 0xFFFFFFFFu;
    }
    for (pg = (a - CAP_BASE) >> 12; pg <= ((e - CAP_BASE) >> 12) && pg < CAP_PAGES; pg++) {
        g_touched[pg] = 1;
    }
}

void gx_cap_save(GxCapState *c) {
    memset(c, 0, sizeof *c);
    c->omL = gx.omL;
    c->omH = gx.omH;
    c->combW0 = gx.combW0;
    c->combW1 = gx.combW1;
    memcpy(c->prim, gx.prim, 4);
    memcpy(c->env, gx.env, 4);
    memcpy(c->fogc, gx.fogc, 4);
    memcpy(c->blendc, gx.blendc, 4);
    c->primLodFrac = gx.primLodFrac;
    c->primLodMin = gx.primLodMin;
    c->fillColor = gx.fillColor;
    c->primZ = gx.primZ;
    c->primDZ = gx.primDZ;
    /* o que o jogo pediu (com a resolucao maior, o efetivo e outro) */
    c->scissorX0 = gx.scRaw[0] >> 2;
    c->scissorY0 = gx.scRaw[1] >> 2;
    c->scissorX1 = (gx.scRaw[2] + 3) >> 2;
    c->scissorY1 = (gx.scRaw[3] + 3) >> 2;
    memcpy(c->tile, gx.tile, sizeof c->tile);
    memcpy(c->tmem, gx.tmem, sizeof c->tmem);
    c->timgAddr = (uint32_t)(uintptr_t)gx.timg.addr;
    c->timgFmt = gx.timg.fmt;
    c->timgSiz = gx.timg.siz;
    c->timgWidth = gx.timg.width;
    c->cimgAddr = (uint32_t)(uintptr_t)gx.cimgRaw;
    c->cimgFmt = gx.cimg.fmt;
    c->cimgSiz = gx.cimg.siz;
    c->cimgWidth = gx.cimgRawW;
    c->zimgAddr = (uint32_t)(uintptr_t)gx.zimgRaw;
    c->texOn = gx.texOn;
    c->texTile = gx.texTile;
    c->texLevel = gx.texLevel;
    c->texScaleS = gx.texScaleS;
    c->texScaleT = gx.texScaleT;
    c->fogMul = gx.fogMul;
    c->fogOff = gx.fogOff;
    c->rdphalf1 = gx.rdphalf1;
    c->rdphalf2 = gx.rdphalf2;
    memcpy(c->light, gx.light, sizeof c->light);
    memcpy(c->lookat, gx.lookat, sizeof c->lookat);
}

void gx_cap_load(const GxCapState *c) {
    gx.omL = c->omL;
    gx.omH = c->omH;
    gx.combW0 = c->combW0;
    gx.combW1 = c->combW1;
    memcpy(gx.prim, c->prim, 4);
    memcpy(gx.env, c->env, 4);
    memcpy(gx.fogc, c->fogc, 4);
    memcpy(gx.blendc, c->blendc, 4);
    gx.primLodFrac = c->primLodFrac;
    gx.primLodMin = c->primLodMin;
    gx.fillColor = c->fillColor;
    gx.primZ = c->primZ;
    gx.primDZ = c->primDZ;
    gx.scRaw[0] = c->scissorX0 << 2;
    gx.scRaw[1] = c->scissorY0 << 2;
    gx.scRaw[2] = c->scissorX1 << 2;
    gx.scRaw[3] = c->scissorY1 << 2;
    memcpy(gx.tile, c->tile, sizeof c->tile);
    memcpy(gx.tmem, c->tmem, sizeof c->tmem);
    gx.timg.addr = (const uint8_t *)(uintptr_t)c->timgAddr;
    gx.timg.fmt = c->timgFmt;
    gx.timg.siz = c->timgSiz;
    gx.timg.width = c->timgWidth;
    gx.cimgRaw = (uint8_t *)(uintptr_t)c->cimgAddr;
    gx.cimg.fmt = c->cimgFmt;
    gx.cimg.siz = c->cimgSiz;
    gx.cimgRawW = c->cimgWidth;
    gx.zimgRaw = (uint8_t *)(uintptr_t)c->zimgAddr;
    gx_hr_retarget(); /* cimg/zimg efetivos, tesoura e viewport */
    gx.texOn = c->texOn;
    gx.texTile = c->texTile;
    gx.texLevel = c->texLevel;
    gx.texScaleS = c->texScaleS;
    gx.texScaleT = c->texScaleT;
    gx.fogMul = c->fogMul;
    gx.fogOff = c->fogOff;
    gx.rdphalf1 = c->rdphalf1;
    gx.rdphalf2 = c->rdphalf2;
    memcpy(gx.light, c->light, sizeof c->light);
    memcpy(gx.lookat, c->lookat, sizeof c->lookat);
    gx.lightsDirty = 1;
    gx.texDirty = 1;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    fwrite(b, 1, 4, f);
}

int gx_capture_render(uint32_t dlAddr, const char *path) {
    uint32_t end = gx_ram_end, size, pg, npages = 0;
    uint8_t *snap;
    GxCapState st;
    FILE *f;
    if (end <= CAP_BASE || end - CAP_BASE > CAP_PAGES * 4096u) {
        end = CAP_BASE + CAP_PAGES * 4096u;
    }
    size = end - CAP_BASE;
    snap = (uint8_t *)malloc(size);
    if (snap == NULL) {
        gx_render(dlAddr, NULL);
        return -1;
    }
    /* memoria e estado ANTES do quadro. Pagina a pagina: entre o codigo e os dados do executavel
     * o ligador do ARM deixa paginas sem mapear (copiar tudo de uma vez travava no aparelho) */
    for (pg = 0; pg < size / 4096u; pg++) {
        unsigned char vec;
        void *p = (void *)(uintptr_t)(CAP_BASE + pg * 4096u);
        if (mincore(p, 4096, &vec) == 0) {
            memcpy(snap + (size_t)pg * 4096u, p, 4096);
        } else {
            memset(snap + (size_t)pg * 4096u, 0, 4096);
        }
    }
    gx_cap_save(&st);
    /* renderiza normalmente marcando o que for lido */
    memset(g_touched, 0, sizeof g_touched);
    gx_capturing = 1;
    gx_hr_pass_begin(); /* capturando: sempre 320x240 (o que o jogo pediu) */
    if (gx.cimgRaw) {
        gx_touch(gx.cimgRaw, 320u * 240u * 2u);
    }
    if (gx.zimgRaw) {
        gx_touch(gx.zimgRaw, 320u * 240u * 2u);
    }
    gx_render(dlAddr, NULL);
    gx_capturing = 0;
    f = fopen(path, "wb");
    if (f == NULL) {
        free(snap);
        return -1;
    }
    for (pg = 0; pg < size / 4096u; pg++) {
        npages += g_touched[pg];
    }
    fwrite("OOTCAP1", 1, 8, f);
    put32(f, dlAddr);
    put32(f, end);
    put32(f, (uint32_t)sizeof st);
    put32(f, npages);
    fwrite(&st, 1, sizeof st, f);
    for (pg = 0; pg < size / 4096u; pg++) {
        if (g_touched[pg]) {
            put32(f, CAP_BASE + pg * 4096u);
            fwrite(snap + (size_t)pg * 4096u, 1, 4096, f);
        }
    }
    fclose(f);
    free(snap);
    fprintf(stderr, "[gfx] captura gravada: %s (dl=%08x, %u paginas de 4 KB)\n", path, dlAddr, npages);
    return 0;
}
