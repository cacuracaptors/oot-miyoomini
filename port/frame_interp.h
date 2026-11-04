/* frame_interp.h -- interpolacao de quadros (ver frame_interp.c) */
#ifndef FRAME_INTERP_H
#define FRAME_INTERP_H

#include <stdint.h>

/* chamadas pelo jogo (ajustes do oot-patch.py) */
void Port_FiStartRecord(void);
void Port_FiStopRecord(const void* tag);
void Port_FiOpenChild(const void* a, int b);
void Port_FiCloseChild(void);
void Port_FiActorPosRot(void);
void Port_FiPush(void);
void Port_FiPop(void);
void Port_FiMode(int mode);
void Port_FiSetTRYXZ(float x, float y, float z, const int16_t* rot, const float* cur);
void Port_FiToMtx(const float* cur, void* dest);
void Port_FiMtxF(const float* src, void* dest);
void Port_FiSkip(int d);
void Port_FiDontInterpolateCamera(void);
void Port_FiDontInterpolateFrame(void);
int Port_FiCameraEpoch(void);
void Port_FiCameraCheck(const float* eye, const float* at, const float* up, int pauseMode);
void Port_FiDontInterpolateChild(const void* key);
void Port_FiSkinVtx(const void* key, const void* buf, int count);
void Port_FiTexScroll(const void* cmd);
void Port_FiCmd(const void* cmd, int kind); /* 0 = G_SETPRIMCOLOR, 1 = G_TEXRECT (+ RDPHALF_1/2), 2 = G_FILLRECT / G_SETSCISSOR */

/* usadas pelo port (desenho) */
#define FI_REPL_BITS 14
#define FI_REPL_HASH (1 << FI_REPL_BITS)
#define FI_REPL_MAX 12288
#define FI_SKIN_MAX 256   /* limbs com vertices de skin por quadro */
#define FI_SKIN_VTX 16384 /* vertices de skin por quadro */
#define FI_TILE_HASH 2048 /* texturas que rolam por quadro (potencia de 2) */
#define FI_CMD_HASH 512   /* comandos 2D trocados por quadro (caixa de dialogo...) */
typedef struct {
    int n;
    const void* key[FI_REPL_HASH];
    int idx[FI_REPL_HASH];
    float m[FI_REPL_MAX][16];
    const void* seen[FI_REPL_HASH]; /* diagnostico: todas as Mtx gravadas no quadro atual */
    /* vertices de "skin" misturados: G_VTX que le de [dest, dest + 16*count) passa a ler de vtx + 16*off */
    int nSkin;
    uintptr_t skinLo, skinHi;
    struct {
        uintptr_t dest;
        unsigned len, off;
    } skin[FI_SKIN_MAX];
    uint8_t vtx[FI_SKIN_VTX * 16];
    /* texturas que rolam: G_SETTILESIZE no endereco tkey[i] passa a usar tw0/tw1 */
    int nTile;
    const void* tkey[FI_TILE_HASH];
    uint32_t tw0[FI_TILE_HASH], tw1[FI_TILE_HASH];
    /* comandos 2D (Port_FiCmd): o comando no endereco ckey[i] passa a usar cw[i] */
    int nCmd;
    const void* ckey[FI_CMD_HASH];
    uint32_t cw[FI_CMD_HASH][4];
} FiRepl;

extern int port_fi_enabled;
int port_fi_pair(const void* tag, int* curSlot, int* prevSlot);
void port_fi_build(int curSlot, int prevSlot, float step, FiRepl* out);
const float* port_fi_lookup(const FiRepl* r, const void* dest);
const void* port_fi_vtx_lookup(const FiRepl* r, const void* src, unsigned bytes);
int port_fi_tile_lookup(const FiRepl* r, const void* cmd, uint32_t* w0, uint32_t* w1);
const uint32_t* port_fi_cmd_lookup(const FiRepl* r, const void* cmd);

/* diagnostico (log a cada ~2 s, gfx_hook.c): o que ficou sem interpolar */
typedef struct {
    unsigned recs, recBad, camCuts, builds, mtxCur, mtxRepl, mtxNoItem, adjMis, angSkip;
    unsigned chNew, mtxNew, chDont, mtxDont, skinCur, skinRepl, skinVtx, scrCur, scrRepl, scrSkip;
    unsigned ovf[6]; /* estouro: caminhos, itens, matrizes, trs, profundidade, hash */
    float cut[4][4]; /* ate 4 cortes de camera: eyeD lookD upD dirD */
    int nCut;
    struct {
        const void* ka;
        int kb;
        int id;
        unsigned mtx, n;
    } top[24];
} FiDiag;
extern FiDiag port_fi_diag;
int port_fi_seen(const FiRepl* r, const void* dest); /* a Mtx foi gravada no quadro atual? */

#endif
