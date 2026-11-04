/*
 * port_stubs.c -- pecas pequenas que o jogo espera encontrar no executavel e que,
 * no N64, vinham do ligador, de assembly MIPS ou do 64DD.
 *
 * Compilado SEM os cabecalhos do jogo (so tipos basicos).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- identificacao da compilacao (src/boot/build.c usa macros do Makefile) ---- */
const char gBuildCreator[] = "miyoo-port";
const char gBuildDate[] = "2026-10-11 00:00:00";
const char gBuildMakeOption[] = "";

/* ---- libultra/gu/libm_vals.s ---- */
static const union {
    uint32_t u;
    float f;
} k_qnan = { 0x7F810000u };
float __libm_qnan_f = 0.0f; /* preenchido em port_stubs_init */

void port_stubs_init(void) {
    __libm_qnan_f = k_qnan.f;
}

/* ---- code/kanji_tex.s: agora em C dentro de z_kanfont.c (tools/oot-patch.py) ---- */

/* ---- code/z_moji.c (fonte de depuracao): os dados so existem em builds de debug ---- */
unsigned long long gMojiFontTLUTs[4][4];
unsigned long long gMojiFontTex[128];

/* ---- Overlays: no port estao todos ligados ao executavel (libu64/loadfragment2_n64.c) ---- */
int gOverlayLogSeverity = 2;

void Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd, void* allocatedRamAddr) {
    (void)vromStart; (void)vromEnd; (void)vramStart; (void)vramEnd; (void)allocatedRamAddr;
}

void* Overlay_AllocateAndLoad(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd) {
    (void)vromStart; (void)vromEnd; (void)vramStart; (void)vramEnd;
    return NULL;
}

void Overlay_Relocate(void* allocatedRamAddr, void* ovlRelocs, void* vramStart) {
    (void)allocatedRamAddr; (void)ovlRelocs; (void)vramStart;
}

/* ---- 64DD: nao existe. Tudo responde "sem drive". ---- */
unsigned int func_801C6E80(void) { return 0; }
void func_801C6EA0(void* gfxP) { (void)gfxP; }
int func_801C70FC(void) { return 0; }
void func_801C7268(void) {}
int func_801C7658(void) { return 0; }
int func_801C7818(void) { return 0; }
void func_801C7C1C(void* dest, int offset, int size) { (void)dest; (void)offset; (void)size; }
void func_801C7E78(void) {}
void n64dd_SetDiskVersion(int arg0) { (void)arg0; }
