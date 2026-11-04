/*
 * gfx_tri.c -- rasterizador de triangulos e pipeline de pixel do RDP (combinador, blender,
 * z-buffer, escrita no framebuffer). Versao de referencia (correta primeiro; as versoes rapidas
 * entram depois e sao testadas contra esta).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_priv.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* ------------------------------------------------------------------ combinador */
enum {
    V_COMB = 0, V_T0 = 4, V_T1 = 8, V_PRIM = 12, V_SHADE = 16, V_ENV = 20,
    V_LODF = 24, V_PLODF = 25, V_ONE = 26, V_ZERO = 27, V_NOISE = 28, V_N = 32
};

typedef struct {
    uint8_t rgb[3][4]; /* [canal][a,b,c,d] -> indice em V */
    uint8_t a[4];
} CombCycle;

static struct {
    uint32_t w0, w1;
    int valid;
    CombCycle cy[2];
    int useTex[2][2]; /* [ciclo][textura]: o ciclo le TEXEL0 / TEXEL1 (calculado em comb_decode) */
} comb;

static struct {
    int cyc;
    int bilerp;
    int persp;
    int useT0, useT1;
    int zcmp, zupd, zmode;
    int forceBl, cvgXAlpha, alphaCvgSel, ac;
    int cdith; /* pontilhado da cor ao gravar em 16 bits: 0 nenhum, 1 quadrado magico, 2 bayer (opcao pontilhado) */
    int bl[2][4];
} pc;

static uint8_t idx_rgb(int kind, int sel, int ch) {
    switch (kind) {
        case 0: /* a */
            switch (sel) {
                case 0: return (uint8_t)(V_COMB + ch);
                case 1: return (uint8_t)(V_T0 + ch);
                case 2: return (uint8_t)(V_T1 + ch);
                case 3: return (uint8_t)(V_PRIM + ch);
                case 4: return (uint8_t)(V_SHADE + ch);
                case 5: return (uint8_t)(V_ENV + ch);
                case 6: return V_ONE;
                case 7: return V_NOISE;
                default: return V_ZERO;
            }
        case 1: /* b */
            switch (sel) {
                case 0: return (uint8_t)(V_COMB + ch);
                case 1: return (uint8_t)(V_T0 + ch);
                case 2: return (uint8_t)(V_T1 + ch);
                case 3: return (uint8_t)(V_PRIM + ch);
                case 4: return (uint8_t)(V_SHADE + ch);
                case 5: return (uint8_t)(V_ENV + ch);
                default: return V_ZERO;
            }
        case 2: /* c */
            switch (sel) {
                case 0: return (uint8_t)(V_COMB + ch);
                case 1: return (uint8_t)(V_T0 + ch);
                case 2: return (uint8_t)(V_T1 + ch);
                case 3: return (uint8_t)(V_PRIM + ch);
                case 4: return (uint8_t)(V_SHADE + ch);
                case 5: return (uint8_t)(V_ENV + ch);
                case 7: return V_COMB + 3;
                case 8: return V_T0 + 3;
                case 9: return V_T1 + 3;
                case 10: return V_PRIM + 3;
                case 11: return V_SHADE + 3;
                case 12: return V_ENV + 3;
                case 13: return V_LODF;
                case 14: return V_PLODF;
                default: return V_ZERO;
            }
        default: /* d */
            switch (sel) {
                case 0: return (uint8_t)(V_COMB + ch);
                case 1: return (uint8_t)(V_T0 + ch);
                case 2: return (uint8_t)(V_T1 + ch);
                case 3: return (uint8_t)(V_PRIM + ch);
                case 4: return (uint8_t)(V_SHADE + ch);
                case 5: return (uint8_t)(V_ENV + ch);
                case 6: return V_ONE;
                default: return V_ZERO;
            }
    }
}

static uint8_t idx_a(int kind, int sel) {
    if (kind == 2) { /* c alfa */
        switch (sel) {
            case 0: return V_LODF;
            case 1: return V_T0 + 3;
            case 2: return V_T1 + 3;
            case 3: return V_PRIM + 3;
            case 4: return V_SHADE + 3;
            case 5: return V_ENV + 3;
            case 6: return V_PLODF;
            default: return V_ZERO;
        }
    }
    switch (sel) {
        case 0: return V_COMB + 3;
        case 1: return V_T0 + 3;
        case 2: return V_T1 + 3;
        case 3: return V_PRIM + 3;
        case 4: return V_SHADE + 3;
        case 5: return V_ENV + 3;
        case 6: return V_ONE;
        default: return V_ZERO;
    }
}

static int uses_tex(const uint8_t *idx, int n, int lo);
static void comb_decode(void) {
    uint32_t w0 = gx.combW0, w1 = gx.combW1;
    int sa[2], sb[2], sc[2], sd[2], aa[2], ab[2], ac[2], ad[2], ch, k;
    sa[0] = (int)((w0 >> 20) & 15);
    sc[0] = (int)((w0 >> 15) & 31);
    aa[0] = (int)((w0 >> 12) & 7);
    ac[0] = (int)((w0 >> 9) & 7);
    sb[0] = (int)((w1 >> 28) & 15);
    sd[0] = (int)((w1 >> 15) & 7);
    ab[0] = (int)((w1 >> 12) & 7);
    ad[0] = (int)((w1 >> 9) & 7);
    sa[1] = (int)((w0 >> 5) & 15);
    sc[1] = (int)(w0 & 31);
    sb[1] = (int)((w1 >> 24) & 15);
    aa[1] = (int)((w1 >> 21) & 7);
    ac[1] = (int)((w1 >> 18) & 7);
    sd[1] = (int)((w1 >> 6) & 7);
    ab[1] = (int)((w1 >> 3) & 7);
    ad[1] = (int)(w1 & 7);
    for (k = 0; k < 2; k++) {
        for (ch = 0; ch < 3; ch++) {
            comb.cy[k].rgb[ch][0] = idx_rgb(0, sa[k], ch);
            comb.cy[k].rgb[ch][1] = idx_rgb(1, sb[k], ch);
            comb.cy[k].rgb[ch][2] = idx_rgb(2, sc[k], ch);
            comb.cy[k].rgb[ch][3] = idx_rgb(3, sd[k], ch);
        }
        comb.cy[k].a[0] = idx_a(0, aa[k]);
        comb.cy[k].a[1] = idx_a(1, ab[k]);
        comb.cy[k].a[2] = idx_a(2, ac[k]);
        comb.cy[k].a[3] = idx_a(3, ad[k]);
    }
    comb.w0 = w0;
    comb.w1 = w1;
    comb.valid = 1;
    for (k = 0; k < 2; k++) {
        const CombCycle *c = &comb.cy[k];
        int ch, t;
        for (t = 0; t < 2; t++) {
            int u = uses_tex(c->a, 4, t ? V_T1 : V_T0);
            for (ch = 0; ch < 3; ch++) {
                u |= uses_tex(c->rgb[ch], 4, t ? V_T1 : V_T0);
            }
            comb.useTex[k][t] = u;
        }
    }
}

static int uses_tex(const uint8_t *idx, int n, int lo) {
    int i;
    for (i = 0; i < n; i++) {
        if (idx[i] >= lo && idx[i] < lo + 4) {
            return 1;
        }
    }
    return 0;
}

/* geracao do estado do pipeline: muda so quando algo que o caminho rapido usa mudou (ele remonta o
 * programa do pixel). A assinatura cobre tudo o que fast_build e os spans leem do estado global. */
static uint32_t g_pipeGen = 1;
static struct PipeSig {
    uint32_t omL, omH, combW0, combW1;
    int texTile;
    const GxTex *tx[2];
    const uint32_t *txPx[2];
    const GxTex *txHd[2]; /* textura nova pendurada depois (carregada em segundo plano) */
    uint64_t txKey[2];
    GxTile tile[2];
    uint8_t prim[4], env[4], blendc[4], fogc[4], primLodFrac, pad[3];
    const uint8_t *cimg, *zimg;
    int cwidth, csiz;
} g_sig;

void gx_pipe_begin(void) {
    uint32_t L = gx.omL;
    int k;
    if (!comb.valid || comb.w0 != gx.combW0 || comb.w1 != gx.combW1) {
        comb_decode();
    }
    pc.cyc = (int)((gx.omH >> GX_CYC_SHIFT) & 3);
    pc.bilerp = ((gx.omH >> 12) & 3) != 0;
    pc.persp = (gx.omH & GX_OMH_PERSP) != 0;
    pc.zcmp = (L & GX_RM_Z_CMP) != 0;
    pc.zupd = (L & GX_RM_Z_UPD) != 0;
    pc.zmode = (int)(L & GX_RM_ZMODE_MASK);
    pc.forceBl = (L & GX_RM_FORCE_BL) != 0;
    pc.cvgXAlpha = (L & GX_RM_CVG_X_ALPHA) != 0;
    pc.alphaCvgSel = (L & GX_RM_ALPHA_CVG_SEL) != 0;
    pc.ac = (int)(L & 3);
    pc.bl[0][0] = (int)((L >> 30) & 3);
    pc.bl[0][1] = (int)((L >> 26) & 3);
    pc.bl[0][2] = (int)((L >> 22) & 3);
    pc.bl[0][3] = (int)((L >> 18) & 3);
    pc.bl[1][0] = (int)((L >> 28) & 3);
    pc.bl[1][1] = (int)((L >> 24) & 3);
    pc.bl[1][2] = (int)((L >> 20) & 3);
    pc.bl[1][3] = (int)((L >> 16) & 3);
    {
        /* G_CD_*: bits 6-7 do modo alto (0 quadrado magico, 1 bayer, 2 ruido -> como o magico, 3 desligado) */
        const int cd = (int)((gx.omH >> 6) & 3);
        /* opcao pontilhado: 1 = bayer sempre (trama uniforme, menos visivel em 640 que o quadrado magico, que forma
         * diagonais); 2 = o que o jogo pede */
        pc.cdith = !gx_pontilhado || cd == 3 ? 0 : (gx_pontilhado == 1 || cd == 1) ? 2 : 1;
        if (gx_pontilhado != 2 && pc.forceBl) {
            /* (transparentes - fumaca, poeira, brilhos: misturam com a memoria; texturas lisas, o pontilhado so
             * aparecia). O ceu tambem tem FORCE_BL (G_RM_OPA_SURF), mas nao le a memoria: fica com o pontilhado */
            const int c2 = pc.cyc == GX_CYC_2;
            const int mem = pc.bl[0][0] == 1 || pc.bl[0][2] == 1 || (c2 && (pc.bl[1][0] == 1 || pc.bl[1][2] == 1));
            if (mem) {
                pc.cdith = 0;
            }
        }
    }
    {
        /* quantas linhas do framebuffer (e do z-buffer) cabem na RAM do N64 */
        size_t rowC = (size_t)gx.cimg.width * (gx.cimg.siz == 3 ? 4u : (gx.cimg.siz == 1 ? 1u : 2u));
        size_t rowZ = (size_t)gx.cimg.width * 2u;
        uint32_t cb = (uint32_t)(uintptr_t)gx.cimg.addr, zb = (uint32_t)(uintptr_t)gx.zimg;
        int rc = (cb >= 0x80000000u && cb < gx_ram_end && rowC) ? (int)((gx_ram_end - cb) / rowC) : 0;
        int rz = (zb >= 0x80000000u && zb < gx_ram_end && rowZ) ? (int)((gx_ram_end - zb) / rowZ) : 0;
        if (gx.hrActive) {
            /* buffers da resolucao maior (fora da RAM do N64): cor e z do tamanho do alvo */
            rc = gx.hrH;
            rz = gx.zimg != NULL ? gx.hrH : 0;
        }
        gx.maxYc = rc - 1;
        gx.maxYz = (rz < rc ? rz : rc) - 1;
    }
    /* quais texturas amostrar. 1 ciclo: so o ciclo 2 do combinador (cy[1]) conta. 2 ciclos: o ciclo 1
     * (cy[0]) le TEXEL0 = tile e TEXEL1 = tile+1; no ciclo 2 (cy[1]) TEXEL0 e TEXEL1 sao ambos a tile+1 */
    pc.useT0 = pc.useT1 = 0;
    {
        int (*use)[2] = comb.useTex; /* [ciclo][textura] */
        if (pc.cyc == GX_CYC_2) {
            pc.useT0 = use[0][0];
            pc.useT1 = use[0][1] | use[1][0] | use[1][1];
        } else {
            pc.useT0 = use[1][0];
            pc.useT1 = use[1][1];
        }
    }
    gx_tex_resolve();
    {
        struct PipeSig sg;
        memset(&sg, 0, sizeof sg);
        sg.omL = gx.omL;
        sg.omH = gx.omH;
        sg.combW0 = gx.combW0;
        sg.combW1 = gx.combW1;
        sg.texTile = gx.texTile;
        for (k = 0; k < 2; k++) {
            sg.tx[k] = gx.rtex[k];
            sg.txPx[k] = gx.rtex[k] ? gx.rtex[k]->px : NULL;
            sg.txKey[k] = gx.rtex[k] ? gx.rtex[k]->key : 0;
            sg.txHd[k] = gx.rtex[k] ? gx.rtex[k]->hd : NULL;
            sg.tile[k] = gx.tile[(gx.texTile + k) & 7];
        }
        memcpy(sg.prim, gx.prim, 4);
        memcpy(sg.env, gx.env, 4);
        memcpy(sg.blendc, gx.blendc, 4);
        memcpy(sg.fogc, gx.fogc, 4);
        sg.primLodFrac = gx.primLodFrac;
        sg.cimg = gx.cimg.addr;
        sg.zimg = gx.zimg;
        sg.cwidth = gx.cimg.width;
        sg.csiz = gx.cimg.siz;
        if (memcmp(&sg, &g_sig, sizeof sg) != 0) {
            g_sig = sg;
            g_pipeGen++;
        }
    }
}

static void probe_common(GxProbeSlot *ps, char kind);
static void probe_reject(GxProbeSlot *ps, int z);
static int g_prz[3];

/* ------------------------------------------------------------------ amostragem de textura */
static inline float shift_coord(float v, int sh) {
    if (sh == 0) {
        return v;
    }
    if (sh <= 10) {
        return v / (float)(1 << sh);
    }
    return v * (float)(1 << (16 - sh));
}

static inline int wrap_coord(int i, int cm, int mask, int clipw) {
    if (cm & 2) {
        if (i < 0) {
            i = 0;
        } else if (i >= clipw) {
            i = clipw - 1;
        }
    }
    if (mask) {
        if ((cm & 1) && (i & (1 << mask))) {
            i = ~i;
        }
        i &= (1 << mask) - 1;
    } else {
        if (i < 0) {
            i = 0;
        } else if (i >= clipw) {
            i = clipw - 1;
        }
    }
    return i;
}

static inline uint32_t fetch(const GxTex *tx, const GxTile *tl, int s, int t) {
    s = wrap_coord(s, tl->cms, tl->masks, tx->clipw);
    t = wrap_coord(t, tl->cmt, tl->maskt, tx->cliph);
    if (s >= tx->w) s = tx->w - 1;
    if (t >= tx->h) t = tx->h - 1;
    return tx->px[(size_t)t * tx->w + s];
}

void gx_tex_sample(int ti, const GxTex *tx, float S, float T, int bilerp, int *out) {
    const GxTile *tl = &gx.tile[ti & 7];
    int si, tiv, s0, t0, fs, ft, ch;
    if (tx == NULL) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    S = shift_coord(S, tl->shifts) - (float)tl->uls * 8.0f;
    T = shift_coord(T, tl->shiftt) - (float)tl->ult * 8.0f;
    si = gx_floori(S);
    tiv = gx_floori(T);
    s0 = si >> 5;
    t0 = tiv >> 5;
    if (!bilerp) {
        uint32_t p = fetch(tx, tl, s0, t0);
        out[0] = (int)(p & 255);
        out[1] = (int)((p >> 8) & 255);
        out[2] = (int)((p >> 16) & 255);
        out[3] = (int)(p >> 24);
        return;
    }
    fs = si & 31;
    ft = tiv & 31;
    {
        uint32_t p00 = fetch(tx, tl, s0, t0);
        uint32_t p10 = fetch(tx, tl, s0 + 1, t0);
        uint32_t p01 = fetch(tx, tl, s0, t0 + 1);
        uint32_t p11 = fetch(tx, tl, s0 + 1, t0 + 1);
        for (ch = 0; ch < 4; ch++) {
            int sh = ch * 8;
            int c00 = (int)((p00 >> sh) & 255), c10 = (int)((p10 >> sh) & 255);
            int c01 = (int)((p01 >> sh) & 255), c11 = (int)((p11 >> sh) & 255);
            int v;
            if (fs + ft >= 32) {
                v = c11 + (((32 - fs) * (c01 - c11) + (32 - ft) * (c10 - c11) + 16) >> 5);
            } else {
                v = c00 + ((fs * (c10 - c00) + ft * (c01 - c00) + 16) >> 5);
            }
            out[ch] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
    }
}

/* ------------------------------------------------------------------ framebuffer */
static inline unsigned rd16be(const uint8_t *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static inline void wr16be(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void fb_read(int x, int y, int *rgb) {
    size_t i = (size_t)y * gx.cimg.width + (size_t)x;
    if (gx.cimg.siz == 3) {
        const uint8_t *p = gx.cimg.addr + i * 4;
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
    } else if (gx.cimg.siz == 1) {
        int v = gx.cimg.addr[i];
        rgb[0] = rgb[1] = rgb[2] = v;
    } else {
        unsigned c = gx.cle ? (unsigned)(gx.cimg.addr[i * 2] | gx.cimg.addr[i * 2 + 1] << 8) : rd16be(gx.cimg.addr + i * 2);
        int r = (int)((c >> 11) & 31), g = (int)((c >> 6) & 31), b = (int)((c >> 1) & 31);
        rgb[0] = (r << 3) | (pc.cdith ? 0 : r >> 2); /* (com pontilhado: como o RDP, sem repetir os bits) */
        rgb[1] = (g << 3) | (pc.cdith ? 0 : g >> 2);
        rgb[2] = (b << 3) | (pc.cdith ? 0 : b >> 2);
    }
}

static inline void fb_write(int x, int y, const int *rgb, int a) {
    size_t i = (size_t)y * gx.cimg.width + (size_t)x;
    if (gx.cimg.siz == 3) {
        uint8_t *p = gx.cimg.addr + i * 4;
        p[0] = (uint8_t)rgb[0];
        p[1] = (uint8_t)rgb[1];
        p[2] = (uint8_t)rgb[2];
        p[3] = (uint8_t)a;
    } else if (gx.cimg.siz == 1) {
        gx.cimg.addr[i] = (uint8_t)rgb[0];
    } else {
        unsigned c = ((unsigned)(rgb[0] >> 3) << 11) | ((unsigned)(rgb[1] >> 3) << 6) |
                     ((unsigned)(rgb[2] >> 3) << 1) | (a >= 128 ? 1u : 0u);
        if (gx.cle) {
            gx.cimg.addr[i * 2] = (uint8_t)c;
            gx.cimg.addr[i * 2 + 1] = (uint8_t)(c >> 8);
        } else {
            wr16be(gx.cimg.addr + i * 2, c);
        }
    }
}

void gx_fill_px(int x, int y) {
    size_t i = (size_t)y * gx.cimg.width + (size_t)x;
    if (gx.nProbe) {
        GxProbeSlot *ps = gx_probe_slot(x, y);
        if (ps) {
            probe_common(ps, 'F');
            ps->out[0] = (int)(gx.fillColor >> 24);
            ps->out[1] = ps->out[2] = 0;
        }
    }
    if (gx.cimg.siz == 3) {
        uint8_t *p = gx.cimg.addr + i * 4;
        p[0] = (uint8_t)(gx.fillColor >> 24);
        p[1] = (uint8_t)(gx.fillColor >> 16);
        p[2] = (uint8_t)(gx.fillColor >> 8);
        p[3] = (uint8_t)gx.fillColor;
    } else if (gx.cimg.siz == 1) {
        gx.cimg.addr[i] = (uint8_t)(gx.fillColor >> (24 - 8 * (x & 3)));
    } else {
        const unsigned c = (x & 1) ? (gx.fillColor & 0xFFFFu) : (gx.fillColor >> 16);
        if (gx.cle) {
            gx.cimg.addr[i * 2] = (uint8_t)c;
            gx.cimg.addr[i * 2 + 1] = (uint8_t)(c >> 8);
        } else {
            wr16be(gx.cimg.addr + i * 2, c);
        }
    }
}

void gx_copy_px(int x, int y, float S, float T) {
    int t[4];
    int rgb[3];
    gx_tex_sample(gx.texTile, gx.rtex[0], S, T, 0, t);
    if ((gx.omL & 3) != 0 && t[3] < (gx.blendc[3] > 0 ? gx.blendc[3] : 1)) {
        return;
    }
    rgb[0] = t[0];
    rgb[1] = t[1];
    rgb[2] = t[2];
    if (gx.nProbe) {
        GxProbeSlot *ps = gx_probe_slot(x, y);
        if (ps) {
            probe_common(ps, 'C');
            memcpy(ps->out, rgb, sizeof rgb);
            ps->S = S;
            ps->T = T;
        }
    }
    fb_write(x, y, rgb, t[3]);
}

/* ------------------------------------------------------------------ sonda de pixel */
GxProbeSlot *gx_probe_slot(int x, int y) {
    int i, k;
    for (i = 0; i < gx.nProbe; i++) {
        GxProbe *p = &gx.probe[i];
        if (p->x != x || p->y != y) {
            continue;
        }
        for (k = 0; k < GX_PROBE_SLOTS; k++) {
            if (p->slot[k].fb == gx.cimg.addr) {
                return &p->slot[k];
            }
        }
        for (k = 0; k < GX_PROBE_SLOTS; k++) {
            if (p->slot[k].fb == NULL) {
                p->slot[k].fb = gx.cimg.addr;
                return &p->slot[k];
            }
        }
        /* sem espaco: recicla a mais antiga */
        {
            int old = 0;
            for (k = 1; k < GX_PROBE_SLOTS; k++) {
                if (p->slot[k].task < p->slot[old].task) {
                    old = k;
                }
            }
            memset(&p->slot[old], 0, sizeof p->slot[old]);
            p->slot[old].fb = gx.cimg.addr;
            return &p->slot[old];
        }
    }
    return NULL;
}

/* GX_PROBE_ALL=1 no ambiente: alem do resumo, imprime cada tentativa de escrita no pixel sondado */
static int probe_all(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GX_PROBE_ALL");
        v = (e != NULL && *e == '1');
    }
    return v;
}

static void probe_reject(GxProbeSlot *ps, int z) {
    if (probe_all()) {
        fprintf(stderr, "[sonda-todas] cmd=%08x tipo=%c REJEITADO por %s comb=%06x/%08x omL=%08x z=%d zmem=%d tol=%d\n",
                gx.curCmd, gx.drawKind ? gx.drawKind : 'T', z ? "z" : "alfa", gx.combW0, gx.combW1, gx.omL, g_prz[0],
                g_prz[1], g_prz[2]);
    }
    if (ps->rejTask != gx.taskNo) {
        ps->rejTask = gx.taskNo;
        ps->rejZ = ps->rejAlpha = 0;
    }
    if (z) {
        ps->rejZ++;
    } else {
        ps->rejAlpha++;
    }
}

static void probe_common(GxProbeSlot *ps, char kind) {
    int k;
    ps->kind = kind;
    ps->task = gx.taskNo;
    ps->cmd = gx.curCmd;
    ps->geom = gx.geom;
    ps->omH = gx.omH;
    ps->omL = gx.omL;
    ps->combW0 = gx.combW0;
    ps->combW1 = gx.combW1;
    ps->texTile = gx.texTile;
    for (k = 0; k < 2; k++) {
        const GxTile *tl = &gx.tile[(gx.texTile + k) & 7];
        ps->tile[k] = *tl;
        ps->texW[k] = gx.rtex[k] ? gx.rtex[k]->w : 0;
        ps->texH[k] = gx.rtex[k] ? gx.rtex[k]->h : 0;
        ps->texSrc[k] = gx.tmemSrc[tl->tmem & 511u];
    }
    memcpy(ps->fogc, gx.fogc, 4);
    memcpy(ps->blendc, gx.blendc, 4);
    ps->fill = gx.fillColor;
}

static const char *vname(int i) {
    static const char *rgbN[] = { "COMB", "T0", "T1", "PRIM", "SHADE", "ENV" };
    static char buf[8][16];
    static int r;
    char *b = buf[r++ & 7];
    if (i < 24) {
        snprintf(b, 16, "%s%s", rgbN[i / 4], (i & 3) == 3 ? "_A" : "");
        return b;
    }
    switch (i) {
        case V_LODF: return "LOD";
        case V_PLODF: return "PRIM_LOD";
        case V_ONE: return "1";
        case V_ZERO: return "0";
        case V_NOISE: return "RUIDO";
        default: return "?";
    }
}

void gx_probe_print(const void *fb) {
    static const char *fmtN[] = { "RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7" };
    static const char *sizN[] = { "4b", "8b", "16b", "32b" };
    static const char *blPM[] = { "IN", "MEM", "BLEND", "FOG" };
    static const char *blA[] = { "IN_A", "FOG_A", "SHADE_A", "0" };
    static const char *blB[] = { "1-A", "MEM_A", "1", "0" };
    int i, k;
    for (i = 0; i < gx.nProbe; i++) {
        const GxProbe *p = &gx.probe[i];
        const GxProbeSlot *ps = NULL;
        for (k = 0; k < GX_PROBE_SLOTS; k++) {
            if (p->slot[k].fb != NULL && (const void *)p->slot[k].fb == fb) {
                ps = &p->slot[k];
            }
        }
        if (ps == NULL || ps->kind == 0) {
            fprintf(stderr, "[sonda %d,%d] nada escrito neste framebuffer\n", p->x, p->y);
            continue;
        }
        fprintf(stderr, "[sonda %d,%d] tarefa=%u tipo=%c cmd=%08x saida=(%d %d %d)", p->x, p->y, ps->task, ps->kind,
                ps->cmd, ps->out[0], ps->out[1], ps->out[2]);
        if (ps->rejTask == ps->task) {
            fprintf(stderr, " rejeitados: z=%u alfa=%u", ps->rejZ, ps->rejAlpha);
        }
        fprintf(stderr, "\n");
        if (ps->kind == 'F') {
            fprintf(stderr, "[sonda]   preenchimento cor=%08x\n", ps->fill);
            continue;
        }
        {
            uint32_t L = ps->omL;
            int cyc = (int)((ps->omH >> GX_CYC_SHIFT) & 3);
            fprintf(stderr, "[sonda]   geom=%08x omH=%08x omL=%08x ciclo=%d persp=%d filtro=%d zcmp=%d zupd=%d zmode=%d forceBl=%d "
                            "cvgXa=%d aCvgSel=%d ac=%d\n",
                    ps->geom, ps->omH, L, cyc + 1, (ps->omH & GX_OMH_PERSP) != 0, (int)((ps->omH >> 12) & 3),
                    (L & GX_RM_Z_CMP) != 0, (L & GX_RM_Z_UPD) != 0, (int)((L >> 10) & 3), (L & GX_RM_FORCE_BL) != 0,
                    (L & GX_RM_CVG_X_ALPHA) != 0, (L & GX_RM_ALPHA_CVG_SEL) != 0, (int)(L & 3));
            for (k = 0; k < 2; k++) {
                fprintf(stderr, "[sonda]   blender c%d: (%s*%s + %s*%s)\n", k + 1, blPM[(L >> (30 - 2 * k)) & 3],
                        blA[(L >> (26 - 2 * k)) & 3], blPM[(L >> (22 - 2 * k)) & 3], blB[(L >> (18 - 2 * k)) & 3]);
            }
        }
        if (ps->kind == 'T' || ps->kind == 'R') {
            const int *V = ps->V;
            uint32_t cw0 = comb.w0, cw1 = comb.w1;
            int save = comb.valid;
            CombCycle cy[2];
            /* decodifica o combinador da sonda sem estragar o cache */
            CombCycle bak[2];
            memcpy(bak, comb.cy, sizeof bak);
            {
                uint32_t g0 = gx.combW0, g1 = gx.combW1;
                gx.combW0 = ps->combW0;
                gx.combW1 = ps->combW1;
                comb_decode();
                memcpy(cy, comb.cy, sizeof cy);
                gx.combW0 = g0;
                gx.combW1 = g1;
            }
            memcpy(comb.cy, bak, sizeof bak);
            comb.w0 = cw0;
            comb.w1 = cw1;
            comb.valid = save;
            fprintf(stderr, "[sonda]   combinador %08x %08x\n", ps->combW0, ps->combW1);
            for (k = 0; k < 2; k++) {
                fprintf(stderr, "[sonda]     c%d cor: (%s - %s) * %s + %s   alfa: (%s - %s) * %s + %s\n", k + 1,
                        vname(cy[k].rgb[0][0]), vname(cy[k].rgb[0][1]), vname(cy[k].rgb[0][2]), vname(cy[k].rgb[0][3]),
                        vname(cy[k].a[0]), vname(cy[k].a[1]), vname(cy[k].a[2]), vname(cy[k].a[3]));
            }
            fprintf(stderr, "[sonda]   T0=(%d %d %d %d) T1=(%d %d %d %d) SHADE=(%d %d %d %d) PRIM=(%d %d %d %d) ENV=(%d %d %d %d) "
                            "PRIM_LOD=%d\n",
                    V[V_T0], V[V_T0 + 1], V[V_T0 + 2], V[V_T0 + 3], V[V_T1], V[V_T1 + 1], V[V_T1 + 2], V[V_T1 + 3],
                    V[V_SHADE], V[V_SHADE + 1], V[V_SHADE + 2], V[V_SHADE + 3], V[V_PRIM], V[V_PRIM + 1],
                    V[V_PRIM + 2], V[V_PRIM + 3], V[V_ENV], V[V_ENV + 1], V[V_ENV + 2], V[V_ENV + 3], V[V_PLODF]);
            fprintf(stderr, "[sonda]   combinado=(%d %d %d %d) alfa_entrada=%d FOG=(%d %d %d %d) BLEND=(%d %d %d %d) "
                            "z=%d zmem=%d usaZ=%d S=%.1f T=%.1f\n",
                    ps->cc[0], ps->cc[1], ps->cc[2], ps->cc[3], ps->aIn, ps->fogc[0], ps->fogc[1], ps->fogc[2], ps->fogc[3],
                    ps->blendc[0], ps->blendc[1], ps->blendc[2], ps->blendc[3], ps->z14, ps->zm, ps->useZ, ps->S, ps->T);
        }
        for (k = 0; k < 2; k++) {
            const GxTile *tl = &ps->tile[k];
            fprintf(stderr, "[sonda]   tile%d(#%d) %s%s line=%u tmem=%u pal=%u cms=%u cmt=%u mask=%u,%u shift=%u,%u "
                            "uls=%u ult=%u lrs=%u lrt=%u -> tex %dx%d origem=%08x\n",
                    k, (ps->texTile + k) & 7, fmtN[tl->fmt & 7], sizN[tl->siz & 3], tl->line, tl->tmem, tl->palette,
                    tl->cms, tl->cmt, tl->masks, tl->maskt, tl->shifts, tl->shiftt, tl->uls, tl->ult, tl->lrs, tl->lrt,
                    ps->texW[k], ps->texH[k], ps->texSrc[k]);
        }
    }
}

/* ------------------------------------------------------------------ pipeline */
static inline int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline int cc_calc(const uint8_t *idx, const int *V) {
    int r = (((V[idx[0]] - V[idx[1]]) * V[idx[2]] + 0x80) >> 8) + V[idx[3]];
    return clamp255(r);
}

static inline int bl_col(int sel, int ch, const int *in, const int *mem) {
    switch (sel) {
        case 0: return in[ch];
        case 1: return mem[ch];
        case 2: return gx.blendc[ch];
        default: return gx.fogc[ch];
    }
}

static void blend_cycle(int c, const int *in, int aIn, const int *mem, int shadeA, int *out) {
    int A, B, ch;
    switch (pc.bl[c][1]) {
        case 0: A = aIn; break;
        case 1: A = gx.fogc[3]; break;
        case 2: A = shadeA; break;
        default: A = 0; break;
    }
    switch (pc.bl[c][3]) {
        case 0: B = 255 - A; break;
        case 1: B = 255; break;
        case 2: B = 255; break;
        default: B = 0; break;
    }
    for (ch = 0; ch < 3; ch++) {
        int P = bl_col(pc.bl[c][0], ch, in, mem);
        int M = bl_col(pc.bl[c][2], ch, in, mem);
        int sum = A + B;
        if (sum <= 0) {
            out[ch] = P;
        } else {
            out[ch] = clamp255((P * A + M * B) / sum);
        }
    }
}

static const uint8_t bayer4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* pontilhado da cor do RDP (16 bits): soma 7 - m ao canal de 8 bits antes de cortar para 5 (como o angrylion: o
 * canal sobe 1 degrau quando a parte que se perde passa de m). [modo 1 magico / 2 bayer][y & 3][x & 3] */
int gx_pontilhado = 1; /* opcao pontilhado */
/* opcao fix_vanishing_paths: a estrada de terra (e outros decalques no chao) sumia e aparecia conforme a camera. No modo
 * decalque o RDP aceita o pixel se o z dele estiver ate "a variacao de z no pixel" do z gravado; com a resolucao maior
 * o pixel e menor, a variacao tambem, e a estrada perdia para o chao. 1 = tolerancia de 2 pixels de 320x240 (custa
 * nada: so o numero muda); 0 = a do RDP no tamanho do desenho */
int gx_fix_vanishing_paths = 1;
const uint8_t gx_dith[3][4][4] = {
    { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { { 7, 1, 6, 0 }, { 3, 5, 2, 4 }, { 4, 2, 5, 3 }, { 0, 6, 1, 7 } },
    { { 7, 3, 6, 2 }, { 1, 5, 0, 4 }, { 6, 2, 7, 3 }, { 0, 4, 1, 5 } },
};

static void seg_arej(int why, int a, int shadeA) {
    int sg;
    if (gx.drawKind != 'T') {
        return;
    }
    sg = gx.curSeg;
    if (gx.segARej[sg]++ == 0) {
        gx.segARej1[sg].comb0 = gx.combW0;
        gx.segARej1[sg].comb1 = gx.combW1;
        gx.segARej1[sg].omL = gx.omL;
        gx.segARej1[sg].omH = gx.omH;
        gx.segARej1[sg].geom = gx.geom;
        gx.segARej1[sg].a = a;
        gx.segARej1[sg].shadeA = shadeA;
        gx.segARej1[sg].why = why;
    }
}

void gx_pipe_px(int x, int y, int z18, int useZ, int zTol, const int *shade, float S, float T) {
    const int z14 = (int)gx_zcomp((uint32_t)z18);
    int V[V_N];
    int cc[4];
    int zm = 0;
    int rgb[3], mem[3], aIn, k;
    size_t zi = 0;
    GxProbeSlot *ps = gx.nProbe ? gx_probe_slot(x, y) : NULL;

    if (gx.drawKind == 'T') {
        gx.segVisit[gx.curSeg]++;
    }
    if (useZ && gx.zimg != NULL) {
        zi = ((size_t)y * gx.cimg.width + (size_t)x) * 2;
        zm = (int)(rd16be(gx.zimg + zi) >> 2);
        if (pc.zcmp) {
            if (pc.zmode == GX_RM_ZMODE_DEC) {
                if (z18 > (int)gx_zdecomp((uint32_t)zm) + zTol) {
                    if (ps) { g_prz[0] = z18; g_prz[1] = (int)gx_zdecomp((uint32_t)zm); g_prz[2] = zTol; probe_reject(ps, 1); }
                    if (gx.drawKind == 'T') gx.segZRej[gx.curSeg]++;
                    return;
                }
            } else if (z14 > zm) {
                if (ps) { g_prz[0] = z14; g_prz[1] = zm; g_prz[2] = -1; probe_reject(ps, 1); }
                if (gx.drawKind == 'T') gx.segZRej[gx.curSeg]++;
                return;
            }
        }
    } else {
        useZ = 0;
    }

    memset(V, 0, sizeof V);
    V[V_PRIM] = gx.prim[0];
    V[V_PRIM + 1] = gx.prim[1];
    V[V_PRIM + 2] = gx.prim[2];
    V[V_PRIM + 3] = gx.prim[3];
    V[V_ENV] = gx.env[0];
    V[V_ENV + 1] = gx.env[1];
    V[V_ENV + 2] = gx.env[2];
    V[V_ENV + 3] = gx.env[3];
    V[V_SHADE] = shade[0];
    V[V_SHADE + 1] = shade[1];
    V[V_SHADE + 2] = shade[2];
    V[V_SHADE + 3] = shade[3];
    V[V_PLODF] = gx.primLodFrac;
    V[V_LODF] = GX_LODF_SEM_LOD; /* LOD_FRACTION com o LOD de textura desligado (portais, etc.) */
    V[V_ONE] = 255;
    V[V_NOISE] = 128;

    if (pc.useT0) {
        gx_tex_sample(gx.texTile, gx.rtex[0], S, T, pc.bilerp, V + V_T0);
    }
    if (pc.useT1) {
        gx_tex_sample((gx.texTile + 1) & 7, gx.rtex[1], S, T, pc.bilerp, V + V_T1);
    }

    if (pc.cyc == GX_CYC_2) {
        const CombCycle *c0 = &comb.cy[0], *c1 = &comb.cy[1];
        for (k = 0; k < 3; k++) {
            cc[k] = cc_calc(c0->rgb[k], V);
        }
        cc[3] = cc_calc(c0->a, V);
        V[V_COMB] = cc[0];
        V[V_COMB + 1] = cc[1];
        V[V_COMB + 2] = cc[2];
        V[V_COMB + 3] = cc[3];
        /* ciclo 2: TEXEL0 passa a ser a tile seguinte */
        for (k = 0; k < 4; k++) {
            V[V_T0 + k] = V[V_T1 + k];
        }
        for (k = 0; k < 3; k++) {
            cc[k] = cc_calc(c1->rgb[k], V);
        }
        cc[3] = cc_calc(c1->a, V);
    } else {
        const CombCycle *c1 = &comb.cy[1];
        for (k = 0; k < 3; k++) {
            cc[k] = cc_calc(c1->rgb[k], V);
        }
        cc[3] = cc_calc(c1->a, V);
    }

    /* teste de alfa / cobertura */
    aIn = cc[3];
    if (pc.cvgXAlpha) {
        if ((cc[3] >> 5) == 0) {
            if (ps) probe_reject(ps, 0);
            seg_arej(1, cc[3], shade[3]);
            return;
        }
    }
    if (pc.ac == 1) {
        if (cc[3] < gx.blendc[3]) {
            if (ps) probe_reject(ps, 0);
            seg_arej(2, cc[3], shade[3]);
            return;
        }
    } else if (pc.ac == 3) {
        if (cc[3] < bayer4[y & 3][x & 3] * 16 + 8) {
            if (ps) probe_reject(ps, 0);
            seg_arej(3, cc[3], shade[3]);
            return;
        }
    }
    if (pc.alphaCvgSel && !pc.cvgXAlpha) {
        aIn = 255;
    }

    /* blender */
    if (pc.cyc == GX_CYC_2) {
        int mid[3];
        fb_read(x, y, mem);
        blend_cycle(0, cc, aIn, mem, shade[3], mid);
        if (pc.forceBl) {
            blend_cycle(1, mid, aIn, mem, shade[3], rgb);
        } else {
            rgb[0] = bl_col(pc.bl[1][0], 0, mid, mem);
            rgb[1] = bl_col(pc.bl[1][0], 1, mid, mem);
            rgb[2] = bl_col(pc.bl[1][0], 2, mid, mem);
        }
    } else {
        if (pc.forceBl) {
            fb_read(x, y, mem);
            blend_cycle(0, cc, aIn, mem, shade[3], rgb);
        } else {
            rgb[0] = bl_col(pc.bl[0][0], 0, cc, cc);
            rgb[1] = bl_col(pc.bl[0][0], 1, cc, cc);
            rgb[2] = bl_col(pc.bl[0][0], 2, cc, cc);
            if (pc.bl[0][0] == 1) { /* CLR_MEM como 1a cor sem blend: le a memoria */
                fb_read(x, y, mem);
                rgb[0] = mem[0];
                rgb[1] = mem[1];
                rgb[2] = mem[2];
            }
        }
    }

    if (ps) {
        if (probe_all()) {
            fprintf(stderr, "[sonda-todas] cmd=%08x tipo=%c comb=%06x/%08x T0=(%d %d %d %d) T1=(%d %d %d %d) cc=(%d %d %d %d) "
                            "saida=(%d %d %d) S=%.1f T=%.1f tile=%d src=%08x/%08x\n",
                    gx.curCmd, gx.drawKind ? gx.drawKind : 'T', gx.combW0, gx.combW1, V[V_T0], V[V_T0 + 1], V[V_T0 + 2],
                    V[V_T0 + 3], V[V_T1], V[V_T1 + 1], V[V_T1 + 2], V[V_T1 + 3], cc[0], cc[1], cc[2], cc[3], rgb[0], rgb[1],
                    rgb[2], S, T, gx.texTile, gx.tmemSrc[gx.tile[gx.texTile & 7].tmem & 511u],
                    gx.tmemSrc[gx.tile[(gx.texTile + 1) & 7].tmem & 511u]);
        }
        probe_common(ps, gx.drawKind ? gx.drawKind : 'T');
        memcpy(ps->V, V, sizeof ps->V);
        memcpy(ps->cc, cc, sizeof ps->cc);
        memcpy(ps->out, rgb, sizeof rgb);
        ps->aIn = aIn;
        ps->z14 = z14;
        ps->zm = zm;
        ps->useZ = useZ;
        ps->S = S;
        ps->T = T;
    }
    if (gx.drawKind == 'T') {
        int sg = gx.curSeg;
        gx.segPx[sg]++;
        if (x < gx.segX0[sg]) gx.segX0[sg] = x;
        if (x > gx.segX1[sg]) gx.segX1[sg] = x;
        if (y < gx.segY0[sg]) gx.segY0[sg] = y;
        if (y > gx.segY1[sg]) gx.segY1[sg] = y;
    }
    if (pc.cdith) {
        const int d = gx_dith[pc.cdith][y & 3][x & 3];
        int k;
        for (k = 0; k < 3; k++) {
            rgb[k] = rgb[k] + d > 255 ? 255 : rgb[k] + d;
        }
    }
    fb_write(x, y, rgb, 255);
    if (useZ && pc.zupd) {
        wr16be(gx.zimg + zi, (unsigned)z14 << 2);
    }
}

/* ------------------------------------------------------------------ primitivas e spans
 *
 * Toda primitiva (triangulo, retangulo de textura, retangulo de preenchimento em 1/2 ciclos) vira um
 * GxPrim: planos em PONTO FIXO para cor (8.16) e profundidade (14.10) e planos float para
 * S*q, T*q e q. O valor num pixel (x,y) e calculado de um jeito que nao depende de como se anda:
 *   inteiros: c + dy*y + dx*x   (exato, qualquer ordem)
 *   floats:   (f0 + fdy*(y - fy0)) + fdx*(x - fx0)
 * Assim o caminho de referencia (pixel a pixel) e o caminho rapido (4 pixels por vez) dao
 * EXATAMENTE o mesmo resultado, e os testes comparam os dois bit a bit.
 */
int gx_fast = 0;

/* Arredondamentos sem chamar a libm (no ARMv7 floorf/ceilf/llrint sao chamadas de funcao). Resultados
 * identicos aos da libm: abaixo de 2^23 a conta e exata em inteiros; acima, o float ja e inteiro. */
static inline float gx_floorf(float f) {
    int i;
    if (!(fabsf(f) < 8388608.0f)) {
        return floorf(f);
    }
    i = (int)f;
    if (i == 0 && f == 0.0f) {
        return f; /* +0 / -0 como a floorf */
    }
    return (float)(i - (f < (float)i));
}
static inline int gx_ceili(float f) {
    int i;
    if (!(fabsf(f) < 8388608.0f)) {
        return (int)f;
    }
    i = (int)f;
    return i + (f > (float)i);
}
/* llrint para valores que cabem em 32 bits (arredonda para o par mais proximo, como o llrint) */
static inline int32_t gx_rint32(double v) {
#if defined(__arm__) && defined(__ARM_FP) && (__ARM_FP & 8)
    float r;
    int32_t i;
    __asm__("vcvtr.s32.f64 %0, %P1" : "=t"(r) : "w"(v));
    memcpy(&i, &r, 4);
    return i;
#else
    return (int32_t)llrint(v);
#endif
}

static int32_t fix_grad(float g, float scale, int32_t lim) {
    double v = (double)g * scale;
    if (v > lim) {
        v = lim;
    } else if (v < -lim) {
        v = -lim;
    }
    return gx_rint32(v); /* |v| <= lim <= 2^22 */
}

/* plano inteiro: valor 'v0' no ponto (x0,y0), gradientes gdx/gdy (por pixel), escala de ponto fixo 'scale' */
static void prim_fix(GxPrim *p, int k, float v0, float gdx, float gdy, float x0, float y0, float scale, int32_t lim) {
    p->dx[k] = fix_grad(gdx, scale, lim);
    p->dy[k] = fix_grad(gdy, scale, lim);
    {
        /* llrint da libm e lento no ARMv7 (chamada + long double): quase sempre o valor cabe em 32 bits */
        double c = (double)v0 * scale - (double)p->dx[k] * x0 - (double)p->dy[k] * y0;
        p->c[k] = fabs(c) < 2147483000.0 ? gx_rint32(c) : llrint(c);
    }
}

/* valores de um pixel (usado pela referencia) */
static inline int32_t prim_ival(const GxPrim *p, int k, int x, int y) {
    return (int32_t)(p->c[k] + (int64_t)p->dy[k] * y + (int64_t)p->dx[k] * x);
}

void gx_span_ref(const GxPrim *p, int y, int xs, int xe) {
    int x, k;
    float rowF[3];
    for (k = 0; k < 3; k++) {
        rowF[k] = p->f0[k] + p->fdy[k] * ((float)y - p->fy0);
    }
    for (x = xs; x <= xe; x++) {
        int shade[4];
        int z18 = 0;
        float S = 0.0f, T = 0.0f;
        for (k = 0; k < 4; k++) {
            shade[k] = clamp255((prim_ival(p, k, x, y) + 0x8000) >> 16);
        }
        if (p->useZ) {
            int32_t zv = (prim_ival(p, GXP_Z, x, y) + 128) >> 8;
            z18 = zv < 0 ? 0 : (zv > 0x3FFFF ? 0x3FFFF : zv);
        }
        if (p->hasTex) {
            float fx = (float)x - p->fx0;
            float q = rowF[2] + p->fdx[2] * fx;
            float rq;
            if (fabsf(q) < 1e-12f) {
                q = 1e-12f;
            }
            /* uma divisao por pixel (o reciproco), como o caminho rapido */
            rq = 1.0f / q;
            S = (rowF[0] + p->fdx[0] * fx) * rq;
            T = (rowF[1] + p->fdx[1] * fx) * rq;
        }
        gx_pipe_px(x, y, z18, p->useZ, p->zTol, shade, S, T);
    }
}

/* ---- triangulo pronto para varrer linhas (o mesmo para a referencia e para a fila do caminho rapido) */
typedef struct {
    float px[3], py[3];
    int et0[3], et1[3];
    double slope[3];
    /* as 2 arestas ativas de cada linha: a longa (de cima ate embaixo) sempre, e a de cima ou a de baixo
     * conforme a linha passa do vertice do meio (mesmo calculo das 3 arestas, sem testar todas) */
    float bx[3], by[3]; /* 0 = longa, 1 = de cima, 2 = de baixo */
    double bs[3];
    float ymid;
    int ys, ye;   /* linhas */
    int sx0, sx1; /* limites em x (tesoura e largura do framebuffer) */
} TriEdges;

/* span da linha y; devolve 0 se vazio */
static inline int tri_span(const TriEdges *g, int y, int *pxs, int *pxe) {
    float cy = (float)y + 0.5f;
    int k = cy < g->ymid ? 1 : 2;
    float xa = (float)((double)g->bx[0] + ((double)cy - g->by[0]) * g->bs[0]);
    float xb = (float)((double)g->bx[k] + ((double)cy - g->by[k]) * g->bs[k]);
    float xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
    int xs, xe;
    xs = gx_ceili(xl - 0.5f);
    xe = gx_ceili(xr - 0.5f) - 1;
    if (xs < g->sx0) xs = g->sx0;
    if (xe > g->sx1) xe = g->sx1;
#ifdef TRI_SPAN_CHECK
    {
        extern int tri_span_chk(const TriEdges *g, int y, int xs, int xe);
        tri_span_chk(g, y, xs, xe);
    }
#endif
    *pxs = xs;
    *pxe = xe;
    return xs <= xe;
}

/* referencia antiga (as 3 arestas testadas em cada linha), para conferir */
static inline int tri_span3(const TriEdges *g, int y, int *pxs, int *pxe) {
    float cy = (float)y + 0.5f;
    float xl = 1e30f, xr = -1e30f;
    int e, xs, xe;
    for (e = 0; e < 3; e++) {
        int t0 = g->et0[e], t1 = g->et1[e];
        if (g->py[t0] <= cy && g->py[t1] > cy) {
            double xx = (double)g->px[t0] + ((double)cy - g->py[t0]) * g->slope[e];
            if (xx < xl) xl = (float)xx;
            if (xx > xr) xr = (float)xx;
        }
    }
    if (xr < xl) {
        return 0;
    }
    xs = gx_ceili(xl - 0.5f);
    xe = gx_ceili(xr - 0.5f) - 1;
    if (xs < g->sx0) xs = g->sx0;
    if (xe > g->sx1) xe = g->sx1;
    *pxs = xs;
    *pxe = xe;
    return xs <= xe;
}

#ifdef TRI_SPAN_CHECK
int tri_span_chk(const TriEdges *g, int y, int xs, int xe) {
    int a, b, r = tri_span3(g, y, &a, &b);
    if (!r ? xs <= xe : (a != xs || b != xe)) {
        fprintf(stderr, "tri_span difere y=%d %d %d / %d %d %d\n", y, xs, xe, r, a, b);
        abort();
    }
    return 0;
}
#endif

/* caminho rapido (gfx_fast.inc): fila de primitivas desenhada depois, em 1 ou 2 threads */
static int fast_ok(void);
static void batch_tri(const GxPrim *p, const TriEdges *g);
static void batch_rect(const GxPrim *p, int x0, int y0, int x1, int y1);

/* retangulo (texrect ou fillrect em 1/2 ciclos): cor constante, sem z */
void gx_draw_rect_prim(GxPrim *p, int x0, int y0, int x1, int y1) {
    int y;
    gx.drawKind = 'R';
    p->useZ = 0;
    p->zTol = 0;
    if (fast_ok()) {
        batch_rect(p, x0, y0, x1, y1);
        return;
    }
    for (y = y0; y <= y1; y++) {
        gx_span_ref(p, y, x0, x1);
    }
}

void gx_prim_const(GxPrim *p, const int *shade) {
    int k;
    memset(p, 0, sizeof *p);
    for (k = 0; k < 4; k++) {
        p->c[k] = (int64_t)shade[k] << 16;
    }
    p->f0[2] = 1.0f;
}

/* Triangulo como chega (vertices na tela + o estado que a montagem le). A montagem (tri_setup: gradientes, arestas)
 * pode rodar depois, em qualquer thread: no caminho rapido ela vai para a fila (JOBF_LAZY) e quem desenhar o
 * triangulo primeiro (quase sempre a auxiliar, que esta adiantada) monta; a thread do desenho so le os comandos. */
typedef struct {
    float sx[3], sy[3], sz[3], w[3], r[3], g[3], b[3], a[3], s[3], t[3];
    int useZ, hasTex, persp;
    int scX0, scX1, scY0, scY1, maxY, cwidth;
} TriRaw;

static void tri_raw(TriRaw *R, const GxVtx *a, const GxVtx *b, const GxVtx *c) {
    const GxVtx *v[3];
    int k;
    v[0] = a;
    v[1] = b;
    v[2] = c;
    for (k = 0; k < 3; k++) {
        R->sx[k] = v[k]->sx;
        R->sy[k] = v[k]->sy;
        R->sz[k] = v[k]->sz;
        R->w[k] = v[k]->w;
        R->r[k] = v[k]->r;
        R->g[k] = v[k]->g;
        R->b[k] = v[k]->b;
        R->a[k] = v[k]->a;
        R->s[k] = v[k]->s;
        R->t[k] = v[k]->t;
    }
    R->useZ = (gx.geom & GX_G_ZBUFFER) != 0 && gx.zimg != NULL && (pc.zcmp || pc.zupd);
    R->hasTex = gx.texOn != 0;
    R->persp = pc.persp != 0;
    R->scX0 = (int)gx.scissorX0;
    R->scX1 = (int)gx.scissorX1;
    R->scY0 = (int)gx.scissorY0;
    R->scY1 = (int)gx.scissorY1;
    R->maxY = R->useZ ? gx.maxYz : gx.maxYc;
    R->cwidth = (int)gx.cimg.width;
}

/* linhas do triangulo (as mesmas que tri_setup acha); 0 = nada a desenhar (degenerado ou fora) */
static int tri_rows(const TriRaw *R, int *ys, int *ye) {
    float x[3], y[3], area, ymin, ymax;
    int k;
    for (k = 0; k < 3; k++) {
        x[k] = gx_floorf(R->sx[k] * 4.0f + 0.5f) * 0.25f;
        y[k] = gx_floorf(R->sy[k] * 4.0f + 0.5f) * 0.25f;
    }
    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (fabsf(area) < 1.0f / 64.0f) {
        return 0;
    }
    ymin = y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2]) : (y[1] < y[2] ? y[1] : y[2]);
    ymax = y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2]) : (y[1] > y[2] ? y[1] : y[2]);
    *ys = gx_ceili(ymin - 0.5f);
    *ye = gx_ceili(ymax - 0.5f) - 1;
    if (*ys < R->scY0) *ys = R->scY0;
    if (*ye > R->scY1 - 1) *ye = R->scY1 - 1;
    if (*ye > R->maxY) *ye = R->maxY;
    return *ys <= *ye;
}

/* montagem (antes no meio de gx_draw_tri: as mesmas contas): 0 = nada a desenhar */
static int tri_setup(const TriRaw *R, GxPrim *P, TriEdges *G) {
    float x0, y0, x1, y1, x2, y2, area, inv;
    float rx1, ry1, rx2, ry2;
    float ymin, ymax;
    int e;
    float qa, qb, qc;
    float *px = G->px, *py = G->py;

    x0 = gx_floorf(R->sx[0] * 4.0f + 0.5f) * 0.25f;
    y0 = gx_floorf(R->sy[0] * 4.0f + 0.5f) * 0.25f;
    x1 = gx_floorf(R->sx[1] * 4.0f + 0.5f) * 0.25f;
    y1 = gx_floorf(R->sy[1] * 4.0f + 0.5f) * 0.25f;
    x2 = gx_floorf(R->sx[2] * 4.0f + 0.5f) * 0.25f;
    y2 = gx_floorf(R->sy[2] * 4.0f + 0.5f) * 0.25f;
    rx1 = x1 - x0;
    ry1 = y1 - y0;
    rx2 = x2 - x0;
    ry2 = y2 - y0;
    area = rx1 * ry2 - rx2 * ry1;
    if (fabsf(area) < 1.0f / 64.0f) {
        return 0;
    }
    inv = 1.0f / area;

    /* (todos os campos sao preenchidos abaixo: sem zerar antes) */
    P->useZ = R->useZ;
    P->hasTex = R->hasTex;
    P->fx0 = x0;
    P->fy0 = y0;
    {
        /* gradientes: d/dx e d/dy de cada atributo */
#define GRAD(f0, f1, f2, gdx, gdy)                                       \
        do {                                                             \
            gdx = (((f1) - (f0)) * ry2 - ((f2) - (f0)) * ry1) * inv;     \
            gdy = (((f2) - (f0)) * rx1 - ((f1) - (f0)) * rx2) * inv;     \
        } while (0)
        float gdx, gdy;
        GRAD(R->r[0], R->r[1], R->r[2], gdx, gdy);
        prim_fix(P, GXP_R, R->r[0], gdx, gdy, x0, y0, 65536.0f, 1 << 22);
        GRAD(R->g[0], R->g[1], R->g[2], gdx, gdy);
        prim_fix(P, GXP_G, R->g[0], gdx, gdy, x0, y0, 65536.0f, 1 << 22);
        GRAD(R->b[0], R->b[1], R->b[2], gdx, gdy);
        prim_fix(P, GXP_B, R->b[0], gdx, gdy, x0, y0, 65536.0f, 1 << 22);
        GRAD(R->a[0], R->a[1], R->a[2], gdx, gdy);
        prim_fix(P, GXP_A, R->a[0], gdx, gdy, x0, y0, 65536.0f, 1 << 22);
        GRAD(R->sz[0], R->sz[1], R->sz[2], gdx, gdy);
        prim_fix(P, GXP_Z, R->sz[0], gdx, gdy, x0, y0, GX_Z18_SCALE * 256.0f, 1 << 22);
        /* tolerancia do modo decalque (z de 18 bits): a variacao de z no pixel + 16 */
        P->zTol = (int)(((int64_t)abs(P->dx[GXP_Z]) + abs(P->dy[GXP_Z])) >> 8);
        if (gx_fix_vanishing_paths) {
            P->zTol = (int)((float)P->zTol * 2.0f * (gx.rs > 1.0f ? gx.rs : 1.0f));
        }
        P->zTol += 16;
        qa = R->w[0] > 1e-6f ? 1.0f / R->w[0] : 1.0f;
        qb = R->w[1] > 1e-6f ? 1.0f / R->w[1] : 1.0f;
        qc = R->w[2] > 1e-6f ? 1.0f / R->w[2] : 1.0f;
        if (!R->persp) {
            qa = qb = qc = 1.0f;
        }
        P->f0[0] = R->s[0] * qa;
        GRAD(R->s[0] * qa, R->s[1] * qb, R->s[2] * qc, P->fdx[0], P->fdy[0]);
        P->f0[1] = R->t[0] * qa;
        GRAD(R->t[0] * qa, R->t[1] * qb, R->t[2] * qc, P->fdx[1], P->fdy[1]);
        P->f0[2] = qa;
        GRAD(qa, qb, qc, P->fdx[2], P->fdy[2]);
#undef GRAD
    }

    px[0] = x0;
    px[1] = x1;
    px[2] = x2;
    py[0] = y0;
    py[1] = y1;
    py[2] = y2;
    /* arestas em ordem canonica (de cima para baixo): dois triangulos que dividem uma aresta
     * calculam exatamente o mesmo x em cada linha -> sem frestas entre eles */
    for (e = 0; e < 3; e++) {
        int f = (e + 1) % 3;
        int t0 = e, t1 = f;
        if (py[t0] > py[t1] || (py[t0] == py[t1] && px[t0] > px[t1])) {
            t0 = f;
            t1 = e;
        }
        G->et0[e] = t0;
        G->et1[e] = t1;
        G->slope[e] = py[t1] > py[t0] ? ((double)px[t1] - px[t0]) / ((double)py[t1] - py[t0]) : 0.0;
    }
    {
        /* vertices em ordem de y: ia (cima) <= ib (meio) <= ic (baixo) */
        int ia = 0, ib = 1, ic = 2, tt, k;
        int eb[3];
        if (py[ib] < py[ia]) { tt = ia; ia = ib; ib = tt; }
        if (py[ic] < py[ib]) { tt = ib; ib = ic; ic = tt; }
        if (py[ib] < py[ia]) { tt = ia; ia = ib; ib = tt; }
#define EDGE_OF(i, j) (((i) + 1) % 3 == (j) ? (i) : (j))
        eb[0] = EDGE_OF(ia, ic);
        eb[1] = EDGE_OF(ia, ib);
        eb[2] = EDGE_OF(ib, ic);
#undef EDGE_OF
        for (k = 0; k < 3; k++) {
            G->bx[k] = px[G->et0[eb[k]]];
            G->by[k] = py[G->et0[eb[k]]];
            G->bs[k] = G->slope[eb[k]];
        }
        G->ymid = py[ib];
    }
    ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    G->ys = gx_ceili(ymin - 0.5f);
    G->ye = gx_ceili(ymax - 0.5f) - 1;
    if (G->ys < R->scY0) G->ys = R->scY0;
    if (G->ye > R->scY1 - 1) G->ye = R->scY1 - 1;
    if (G->ye > R->maxY) G->ye = R->maxY;
    G->sx0 = R->scX0;
    G->sx1 = R->scX1 - 1;
    if (G->sx1 > R->cwidth - 1) G->sx1 = R->cwidth - 1;
    return G->ys <= G->ye;
}

static void fast_slot(GxPrim **p, TriEdges **g);
static int fast_ok(void);
static int fast_tri_lazy(const TriRaw *R);

void gx_draw_tri(const GxVtx *a, const GxVtx *b, const GxVtx *c) {
    GxPrim Ploc, *Pp = &Ploc;
    TriEdges Gloc, *Gp = &Gloc;
    TriRaw R;
    int y;

    gx_pipe_begin();
    gx.drawKind = 'T';
    if (gx.cimg.addr == NULL) {
        return;
    }
    tri_raw(&R, a, b, c);
    if (fast_ok()) {
        if (fast_tri_lazy(&R)) {
            return; /* (na fila: montado por quem desenhar) */
        }
        fast_slot(&Pp, &Gp); /* (monta direto no lugar do trabalho na fila: sem copiar ~300 bytes por triangulo) */
    }
    if (!tri_setup(&R, Pp, Gp)) {
        return;
    }
    if (fast_ok()) {
        batch_tri(Pp, Gp);
        return;
    }
    for (y = Gp->ys; y <= Gp->ye; y++) {
        int xs, xe;
        if (tri_span(Gp, y, &xs, &xe)) {
            gx_span_ref(Pp, y, xs, xe);
        }
    }
}

/* caminho rapido (mesmas contas, 4 pixels por vez) */
#include "gfx_fast.inc"

/* ------------------------------------------------------------------ acelerador 2D (gfx_ge.c)
 * A cor de cada texel de um retangulo de textura do jogo como os nucleos calculariam (combinador, teste de
 * alfa), sem o blender: o acelerador mistura com a imagem depois (alfa pre-multiplicado). */
static int comb_uses(int c, int v) {
    const CombCycle *cy = &comb.cy[c];
    int ch, k;
    for (ch = 0; ch < 3; ch++) {
        for (k = 0; k < 4; k++) {
            if (cy->rgb[ch][k] == v) {
                return 1;
            }
        }
    }
    for (k = 0; k < 4; k++) {
        if (cy->a[k] == v) {
            return 1;
        }
    }
    return 0;
}

/* depois de gx_pipe_begin: 0 = os nucleos desenham (z, pontilhado, ruido, outro blender); 1 = opaco;
 * 2 = misturado pelo alfa (cor * a + imagem * (1 - a)) */
int gx_bake_mode(void) {
    int b;
    if (pc.cyc == GX_CYC_FILL) {
        return 0;
    }
    if (pc.cyc == GX_CYC_COPY) {
        return 1;
    }
    if (pc.zcmp || pc.zupd || pc.ac == 3) {
        return 0;
    }
    if (comb_uses(1, V_NOISE) || (pc.cyc == GX_CYC_2 && comb_uses(0, V_NOISE))) {
        return 0;
    }
    b = 0;
    if (pc.cyc == GX_CYC_2) {
        /* 1o ciclo do blender so passa a cor adiante (G_RM_PASS) */
        if (!(pc.bl[0][0] == 0 && pc.bl[0][1] == 3 && pc.bl[0][2] == 0 && pc.bl[0][3] == 2)) {
            return 0;
        }
        b = 1;
    }
    if (pc.forceBl) {
        return (pc.bl[b][0] == 0 && pc.bl[b][1] == 0 && pc.bl[b][2] == 1 && pc.bl[b][3] == 0) ? 2 : 0;
    }
    return pc.bl[b][0] == 0 ? 1 : 0;
}

/* escala da textura nova (gfx_hd.c) que o retangulo usa: 1 = so a original */
int gx_bake_hdk(void) {
    extern int gx_hd_use;
    const GxTex *t = gx.rtex[0];
    if (!gx_hd_use || t == NULL || t->hd == NULL || !pc.useT0) {
        return 1;
    }
    return t->hd->hdk > 1 ? t->hd->hdk : 1;
}

/* um texel da textura (original, ou a nova k vezes maior) no ponto (S, T), sem filtro */
static void bake_sample(int ti, const GxTex *tx, int k, float S, float T, int *out) {
    const GxTile *tl = &gx.tile[ti & 7];
    int s, t, w, h;
    uint32_t p;
    if (tx == NULL) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    if (k > 1 && tx->hd != NULL && tx->hd->hdk == k) {
        tx = tx->hd;
    } else if (k > 1) {
        gx_tex_sample(ti, tx, S, T, 0, out);
        return;
    }
    if (k == 1) {
        gx_tex_sample(ti, tx, S, T, 0, out);
        return;
    }
    S = shift_coord(S, tl->shifts) - (float)tl->uls * 8.0f;
    T = shift_coord(T, tl->shiftt) - (float)tl->ult * 8.0f;
    s = gx_floori(S * (float)k / 32.0f);
    t = gx_floori(T * (float)k / 32.0f);
    w = tx->w;
    h = tx->h;
    if ((tl->cms & 2) || !tl->masks) {
        const int cw = tx->clipw > 0 ? tx->clipw : w;
        s = s < 0 ? 0 : (s >= cw ? cw - 1 : s);
    }
    if ((tl->cmt & 2) || !tl->maskt) {
        const int ch = tx->cliph > 0 ? tx->cliph : h;
        t = t < 0 ? 0 : (t >= ch ? ch - 1 : t);
    }
    if (tl->masks) {
        const int m = k << tl->masks;
        const int q = s >= 0 ? s / m : -((-s + m - 1) / m);
        s -= q * m;
        if ((tl->cms & 1) && (q & 1)) {
            s = m - 1 - s;
        }
    }
    if (tl->maskt) {
        const int m = k << tl->maskt;
        const int q = t >= 0 ? t / m : -((-t + m - 1) / m);
        t -= q * m;
        if ((tl->cmt & 1) && (q & 1)) {
            t = m - 1 - t;
        }
    }
    s = s < 0 ? 0 : (s >= w ? w - 1 : s);
    t = t < 0 ? 0 : (t >= h ? h - 1 : t);
    p = tx->px[(size_t)t * (size_t)w + (size_t)s];
    out[0] = (int)(p & 255);
    out[1] = (int)((p >> 8) & 255);
    out[2] = (int)((p >> 16) & 255);
    out[3] = (int)(p >> 24);
}

/* um texel: ARGB com a cor ja multiplicada pelo alfa (0 = transparente). mode = gx_bake_mode(); k = gx_bake_hdk() */
uint32_t gx_bake_texel(float S, float T, int mode, int hk, const int *shade) {
    int V[V_N];
    int cc[4], k, a;
    if (pc.cyc == GX_CYC_COPY) {
        int t[4];
        bake_sample(gx.texTile, gx.rtex[0], hk, S, T, t);
        if ((gx.omL & 3) != 0 && t[3] < (gx.blendc[3] > 0 ? gx.blendc[3] : 1)) {
            return 0;
        }
        return 0xFF000000u | (uint32_t)t[0] << 16 | (uint32_t)t[1] << 8 | (uint32_t)t[2];
    }
    memset(V, 0, sizeof V);
    for (k = 0; k < 4; k++) {
        V[V_PRIM + k] = gx.prim[k];
        V[V_ENV + k] = gx.env[k];
        V[V_SHADE + k] = shade != NULL ? shade[k] : 255; /* (retangulo: sem cor de vertice) */
    }
    V[V_PLODF] = gx.primLodFrac;
    V[V_LODF] = GX_LODF_SEM_LOD; /* LOD_FRACTION com o LOD de textura desligado (portais, etc.) */
    V[V_ONE] = 255;
    V[V_NOISE] = 128;
    if (pc.useT0) {
        bake_sample(gx.texTile, gx.rtex[0], hk, S, T, V + V_T0);
    }
    if (pc.useT1) {
        bake_sample((gx.texTile + 1) & 7, gx.rtex[1], hk, S, T, V + V_T1);
    }
    if (pc.cyc == GX_CYC_2) {
        const CombCycle *c0 = &comb.cy[0], *c1 = &comb.cy[1];
        int i;
        for (i = 0; i < 3; i++) {
            cc[i] = cc_calc(c0->rgb[i], V);
        }
        cc[3] = cc_calc(c0->a, V);
        for (i = 0; i < 4; i++) {
            V[V_COMB + i] = cc[i];
            V[V_T0 + i] = V[V_T1 + i];
        }
        for (k = 0; k < 3; k++) {
            cc[k] = cc_calc(c1->rgb[k], V);
        }
        cc[3] = cc_calc(c1->a, V);
    } else {
        const CombCycle *c1 = &comb.cy[1];
        for (k = 0; k < 3; k++) {
            cc[k] = cc_calc(c1->rgb[k], V);
        }
        cc[3] = cc_calc(c1->a, V);
    }
    if (pc.cvgXAlpha && (cc[3] >> 5) == 0) {
        return 0;
    }
    if (pc.ac == 1 && cc[3] < gx.blendc[3]) {
        return 0;
    }
    a = (mode == 2 && !(pc.alphaCvgSel && !pc.cvgXAlpha)) ? cc[3] : 255;
    if (a == 0) {
        return 0;
    }
    if (a != 255) {
        for (k = 0; k < 3; k++) {
            const int x = cc[k] * a + 128;
            cc[k] = (x + (x >> 8)) >> 8;
        }
    }
    return (uint32_t)a << 24 | (uint32_t)cc[0] << 16 | (uint32_t)cc[1] << 8 | (uint32_t)cc[2];
}

/* o retangulo le TEXEL0 (t = 0) / TEXEL1 (t = 1)? (depois de gx_pipe_begin) */
int gx_bake_uses(int t) {
    return t ? pc.useT1 : pc.useT0;
}
