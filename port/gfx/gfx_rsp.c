/*
 * gfx_rsp.c -- interpretador de display lists F3DZEX2 (parte "RSP"):
 * segmentos, matrizes, transformacao e iluminacao de vertices, recorte, descarte (culling).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfx_priv.h"
int port_texgen_old; /* opcao reflexo_antigo: 1 = o mapeamento de reflexo de antes */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

GxState gx;
uint32_t gx_ram_end = GX_RAM_END;
void (*gx_debug_hook)(const char *msg);

void gx_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (gx_debug_hook != NULL) {
        gx_debug_hook(buf);
    }
}

/* ------------------------------------------------------------------ enderecos */
const void *gx_resolve(uint32_t a) {
    uint32_t phys = (gx.seg[(a >> 24) & 0xF] + (a & 0x00FFFFFFu)) & 0x1FFFFFFFu;
    if (phys >= 0x01000000u) {
        gx.nBad++;
        return NULL;
    }
    return (const void *)(uintptr_t)(0x80000000u + phys);
}

static inline int seg_index(uint32_t a) {
    uint32_t s = a >> 24;
    return s < 16 ? (int)s : 16;
}

void gx_seg_print(void) {
    int i;
    fprintf(stderr, "[seg] tarefa=%u cor=%p z=%p linhas_validas cor=%d z=%d limite=%08x (segmento: vertices dl tris desenhados fora face)\n",
            gx.taskNo, (void *)gx.cimg.addr, (void *)gx.zimg, gx.maxYc, gx.maxYz, gx_ram_end);
    for (i = 0; i <= 16; i++) {
        if (gx.segVtx[i] || gx.segDl[i] || gx.segTri[i]) {
            char nm[16];
            if (i < 16) {
                snprintf(nm, sizeof nm, "%02X", i);
            } else {
                snprintf(nm, sizeof nm, "direto");
            }
            fprintf(stderr, "[seg]   %s: %u %u %u %u %u %u | visitados=%u pixels=%u z_reprovados=%u", nm, gx.segVtx[i], gx.segDl[i],
                    gx.segTri[i], gx.segDrawn[i], gx.segClip[i], gx.segCull[i], gx.segVisit[i], gx.segPx[i], gx.segZRej[i]);
            if (gx.segPx[i]) {
                fprintf(stderr, " area=(%d,%d)-(%d,%d)", gx.segX0[i], gx.segY0[i], gx.segX1[i], gx.segY1[i]);
            }
            fprintf(stderr, "\n");
            if (gx.segARej[i]) {
                static const char *why[] = { "?", "cvg_x_alpha", "alpha_compare", "dither" };
                fprintf(stderr, "[seg]     alfa_reprovados=%u  1o: motivo=%s alfa=%d shadeA=%d comb=%08x %08x omH=%08x omL=%08x geom=%08x\n",
                        gx.segARej[i], why[gx.segARej1[i].why & 3], gx.segARej1[i].a, gx.segARej1[i].shadeA,
                        gx.segARej1[i].comb0, gx.segARej1[i].comb1, gx.segARej1[i].omH, gx.segARej1[i].omL,
                        gx.segARej1[i].geom);
            }
        }
    }
}

/* ------------------------------------------------------------------ matrizes (linha x matriz) */
static void mat_ident(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b  (a aplicada primeiro, vetor linha) */
static void mat_mul(float *out, const float *a, const float *b) {
    float t[16];
    int r, c, k;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            t[r * 4 + c] = s;
        }
    }
    memcpy(out, t, sizeof t);
}

/* Mtx do N64: 8 palavras com partes inteiras (2 colunas por palavra) e 8 com as fracoes */
static void mat_load_n64(float *out, const uint32_t *w) {
    int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            uint32_t wi = w[r * 2 + (c >> 1)];
            uint32_t wf = w[8 + r * 2 + (c >> 1)];
            int sh = (c & 1) ? 0 : 16;
            int16_t ip = (int16_t)((wi >> sh) & 0xFFFF);
            uint16_t fp = (uint16_t)((wf >> sh) & 0xFFFF);
            out[r * 4 + c] = (float)ip + (float)fp / 65536.0f;
        }
    }
}

static void update_mp(void) {
    if (gx.mpDirty) {
        mat_mul(gx.mp, gx.mv[gx.mvTop], gx.proj);
        gx.mpDirty = 0;
    }
}

/* ------------------------------------------------------------------ luzes */
static void update_lights(void) {
    const float *m = gx.mv[gx.mvTop];
    int i, r;
    if (!gx.lightsDirty) {
        return;
    }
    gx.lightsDirty = 0;
    for (i = 1; i <= gx.numLights && i <= 7; i++) {
        float L[3], C[3], len;
        for (r = 0; r < 3; r++) {
            L[r] = (float)gx.light[i].dir[r] / 127.0f;
        }
        /* C = M3x3 * L  (coeficientes no espaco do modelo) */
        for (r = 0; r < 3; r++) {
            C[r] = m[r * 4 + 0] * L[0] + m[r * 4 + 1] * L[1] + m[r * 4 + 2] * L[2];
        }
        len = sqrtf(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
        if (len > 1e-9f) {
            len = 1.0f / len;
        } else {
            len = 0.0f;
        }
        for (r = 0; r < 3; r++) {
            gx.lightDirModel[i][r] = C[r] * len;
        }
    }
}

/* ------------------------------------------------------------------ vertices */
/* quadros intermediarios: vertices misturados no lugar dos do jogo (skin; NULL = nenhum) */
const void *(*gx_vtx_lookup)(const void *src, unsigned bytes);
/* quadros intermediarios: comandos 2D trocados (G_TEXRECT, G_SETPRIMCOLOR; NULL = nenhum) */
const uint32_t *(*gx_cmd_lookup)(const void *cmd);

static void cmd_vtx(uint32_t w0, uint32_t w1) {
    int n = (int)((w0 >> 12) & 0xFF);
    int v0 = (int)((w0 >> 1) & 0x7F) - n;
    const uint8_t *src = (const uint8_t *)gx_resolve(w1);
    const float *mp;
    int i;
    if (src == NULL || n <= 0 || v0 < 0 || v0 + n > GX_VTX_MAX) {
        gx.nBad++;
        return;
    }
    if (gx_vtx_lookup != NULL) {
        const uint8_t *alt = (const uint8_t *)gx_vtx_lookup(src, (unsigned)n * 16u);
        if (alt != NULL) {
            src = alt;
        }
    }
    gx_touch(src, (size_t)n * 16u);
    update_mp();
    mp = gx.mp;
    gx.nVtx += (uint32_t)n;
    gx.curSeg = seg_index(w1);
    gx.segVtx[gx.curSeg] += (uint32_t)n;
    if (gx.traceN > 0) {
        if (gx.shownV < 8) {
            int16_t o[3];
            gx.shownV++;
            memcpy(o, src, 6);
            fprintf(stderr, "[trace] vtx addr=%08x -> %p n=%d v0=%d  1o vertice ob=(%d %d %d) cor=(%d %d %d %d) geom=%08x\n", w1,
                    (const void *)src, n, v0, o[0], o[1], o[2], src[12], src[13], src[14], src[15], gx.geom);
        }
    }
    if (gx.geom & GX_G_LIGHTING) {
        update_lights();
    }
    {
        /* o estado lido em todo vertice vai para variaveis locais: as escritas em gx.vtx[] (dentro de gx)
         * obrigavam o compilador a reler gx.geom, as luzes, a escala... a cada campo gravado. As contas sao
         * as mesmas, na mesma ordem. */
        const uint32_t geom = gx.geom;
        const int lit = (geom & GX_G_LIGHTING) != 0, tgen = (geom & GX_G_TEXTURE_GEN) != 0;
        const int fog = (geom & GX_G_FOG) != 0;
        const float tsS = gx.texScaleS, tsT = gx.texScaleT;
        const float fogMul = (float)gx.fogMul, fogOff = (float)gx.fogOff;
        const float vs0 = gx.vpScale[0], vs1 = gx.vpScale[1], vs2 = gx.vpScale[2];
        const float vt0 = gx.vpTrans[0], vt1 = gx.vpTrans[1], vt2 = gx.vpTrans[2];
        float ldir[8][3], lcol[8][3], amb[3];
        int nl = 0, l;
        if (lit) {
            int am = gx.numLights + 1;
            nl = gx.numLights < 7 ? gx.numLights : 7;
            for (l = 1; l <= nl; l++) {
                ldir[l][0] = gx.lightDirModel[l][0];
                ldir[l][1] = gx.lightDirModel[l][1];
                ldir[l][2] = gx.lightDirModel[l][2];
                lcol[l][0] = (float)gx.light[l].col[0];
                lcol[l][1] = (float)gx.light[l].col[1];
                lcol[l][2] = (float)gx.light[l].col[2];
            }
            amb[0] = (float)gx.light[am].col[0];
            amb[1] = (float)gx.light[am].col[1];
            amb[2] = (float)gx.light[am].col[2];
        }
    for (i = 0; i < n; i++) {
        const uint8_t *p = src + (size_t)i * 16;
        GxVtx *v = &gx.vtx[v0 + i];
        int16_t ob[3], tc[2];
        float x, y, z, w, vx_, vy_, vz_;
        uint8_t clip;
        memcpy(ob, p, 6);
        memcpy(tc, p + 8, 4);
        x = (float)ob[0];
        y = (float)ob[1];
        z = (float)ob[2];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        {
            /* as 4 colunas de uma vez, na mesma ordem de contas (vmla do NEON nao e fundida: arredonda o
             * produto e a soma como o codigo escalar) */
            float32x4_t r = vmulq_n_f32(vld1q_f32(mp), x);
            r = vmlaq_n_f32(r, vld1q_f32(mp + 4), y);
            r = vmlaq_n_f32(r, vld1q_f32(mp + 8), z);
            r = vaddq_f32(r, vld1q_f32(mp + 12));
            vx_ = vgetq_lane_f32(r, 0);
            vy_ = vgetq_lane_f32(r, 1);
            vz_ = vgetq_lane_f32(r, 2);
            w = vgetq_lane_f32(r, 3);
        }
#else
        vx_ = x * mp[0] + y * mp[4] + z * mp[8] + mp[12];
        vy_ = x * mp[1] + y * mp[5] + z * mp[9] + mp[13];
        vz_ = x * mp[2] + y * mp[6] + z * mp[10] + mp[14];
        w = x * mp[3] + y * mp[7] + z * mp[11] + mp[15];
#endif
        v->x = vx_;
        v->y = vy_;
        v->z = vz_;
        v->w = w;

        if (lit) {
            float nx = (float)(int8_t)p[12], ny = (float)(int8_t)p[13], nz = (float)(int8_t)p[14];
            {
                float cr = amb[0], cg = amb[1], cb = amb[2];
                for (l = 1; l <= nl; l++) {
                    float d = (nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2]) * (1.0f / 127.0f);
                    if (d > 0.0f) {
                        cr += d * lcol[l][0];
                        cg += d * lcol[l][1];
                        cb += d * lcol[l][2];
                    }
                }
                v->r = cr > 255.0f ? 255.0f : cr;
                v->g = cg > 255.0f ? 255.0f : cg;
                v->b = cb > 255.0f ? 255.0f : cb;
                if (gx_gamma_on) {
                    v->r = gx_gammaf[(int)v->r];
                    v->g = gx_gammaf[(int)v->g];
                    v->b = gx_gammaf[(int)v->b];
                }
            }
            if (tgen) {
                /* reflexo: normal no espaco da camera dot lookat */
                const float *mv = gx.mv[gx.mvTop];
                float vx = nx * mv[0] + ny * mv[4] + nz * mv[8];
                float vy = nx * mv[1] + ny * mv[5] + nz * mv[9];
                float vz = nx * mv[2] + ny * mv[6] + nz * mv[10];
                float len = sqrtf(vx * vx + vy * vy + vz * vz);
                float dx, dy;
                if (len > 1e-9f) {
                    len = 127.0f / len;
                } else {
                    len = 0.0f;
                }
                vx *= len;
                vy *= len;
                vz *= len;
                dx = vx * gx.lookat[0].dir[0] + vy * gx.lookat[0].dir[1] + vz * gx.lookat[0].dir[2];
                dy = vx * gx.lookat[1].dir[0] + vy * gx.lookat[1].dir[1] + vz * gx.lookat[1].dir[2];
                dx = dx / (127.0f * 127.0f);
                dy = dy / (127.0f * 127.0f);
                if (port_texgen_old) {
                    v->s = (dx + 1.0f) * 0.5f * (tsS * 65536.0f / 2.0f) * 0.5f; /* (antes: metade da faixa) */
                    v->t = (dy + 1.0f) * 0.5f * (tsT * 65536.0f / 2.0f) * 0.5f;
                } else if (geom & 0x80000u) {
                    /* G_TEXTURE_GEN_LINEAR: o ucode usa o angulo, nao o cosseno (GLideN64, port do mr-finster):
                     * s = acos(-n.l) / pi * escala / 2 (logo da Nintendo, reflexos com "linear") */
                    dx = dx < -1.0f ? -1.0f : (dx > 1.0f ? 1.0f : dx);
                    dy = dy < -1.0f ? -1.0f : (dy > 1.0f ? 1.0f : dy);
                    v->s = acosf(-dx) * (0.5f / 3.14159265f) * (tsS * 65536.0f);
                    v->t = acosf(-dy) * (0.5f / 3.14159265f) * (tsT * 65536.0f);
                } else {
                    /* (n.l + 1) / 4 * escala (como o Fast3D/Ship of Harkinian): a faixa toda da textura */
                    v->s = (dx + 1.0f) * 0.25f * (tsS * 65536.0f);
                    v->t = (dy + 1.0f) * 0.25f * (tsT * 65536.0f);
                }
            } else {
                v->s = (float)tc[0] * tsS;
                v->t = (float)tc[1] * tsT;
            }
        } else {
            v->r = gx_gamma_on ? gx_gammaf[p[12]] : (float)p[12];
            v->g = gx_gamma_on ? gx_gammaf[p[13]] : (float)p[13];
            v->b = gx_gamma_on ? gx_gammaf[p[14]] : (float)p[14];
            v->s = (float)tc[0] * tsS;
            v->t = (float)tc[1] * tsT;
        }

        if (fog) {
            float f;
            if (w > 1e-6f) {
                f = (vz_ / w) * fogMul + fogOff;
            } else {
                f = 255.0f;
            }
            if (f < 0.0f) {
                f = 0.0f;
            } else if (f > 255.0f) {
                f = 255.0f;
            }
            v->fog = f;
            v->a = f;
        } else {
            v->fog = 0.0f;
            v->a = (float)p[15];
        }

        clip = 0;
        if (vx_ < -w) clip |= 1;
        if (vx_ > w) clip |= 2;
        if (vy_ < -w) clip |= 4;
        if (vy_ > w) clip |= 8;
        /* o OoT usa o microcodigo F3DZEX2 "NoN" (sem recorte no plano perto): o que esta mais perto que o
         * plano perto e desenhado (com profundidade 0). So fica "fora" o que esta atras da camera. */
        if (w <= 0.0f) clip |= 16;
        if (vz_ > w) clip |= 32;
        v->clip = clip;
        if (w > 1e-6f) {
            float inv = 1.0f / w;
            v->sx = vx_ * inv * vs0 + vt0;
            v->sy = vy_ * inv * vs1 + vt1;
            v->sz = (vz_ * inv * vs2 + vt2) * (1.0f / 1023.0f);
        } else {
            v->sx = v->sy = v->sz = 0.0f;
        }
        v->valid = 1;
    }
    }
}

/* ------------------------------------------------------------------ triangulos */

int gx_clip_cor = 1; /* 0 = cor do ponto recortado misturada em 3D (como antes) */

/* interpola todos os atributos */
/* As cores (shade) sao interpoladas na tela sem perspectiva (como no RDP); se o ponto novo levasse a cor
 * misturada no espaco 3D, o mesmo triangulo mudaria de iluminacao de repente quando passa a ser recortado
 * (um vertice sai da folga da tela com o movimento da camera). Entao a cor do ponto novo e a que o
 * triangulo inteiro teria ali, na tela: parametro k * wb / w. So perto da borda da folga (onde o salto
 * aparece); longe dela, e com uma ponta atras da camera, continua a mistura em 3D, como no N64. */
static void vtx_lerp(GxVtx *o, const GxVtx *a, const GxVtx *b, float k) {
    float kc;
    o->x = a->x + (b->x - a->x) * k;
    o->y = a->y + (b->y - a->y) * k;
    o->z = a->z + (b->z - a->z) * k;
    o->w = a->w + (b->w - a->w) * k;
    o->s = a->s + (b->s - a->s) * k;
    o->t = a->t + (b->t - a->t) * k;
    kc = k;
    if (gx_clip_cor && o->w > 1e-6f && a->w > 1e-6f && b->w > 1e-6f) {
        /* peso: 1 quando a ponta de fora esta logo depois da folga (onde o recorte comeca: sem salto), caindo
         * ate 0 quando ela esta 4x mais longe (triangulos enormes, como o ceu, ficam como no N64) */
        const float ax = fabsf(b->x), ay = fabsf(b->y);
        const float m = (ax > ay ? ax : ay) / b->w;
        float wt = (8.0f - m) * (1.0f / 6.0f); /* folga GB = 2: peso 1 ate 2x a tela, 0 a partir de 8x */
        if (wt > 0.0f) {
            float ka = k * b->w / o->w;
            ka = ka < 0.0f ? 0.0f : (ka > 1.0f ? 1.0f : ka);
            wt = wt > 1.0f ? 1.0f : wt;
            kc = k + (ka - k) * wt;
        }
    }
    o->r = a->r + (b->r - a->r) * kc;
    o->g = a->g + (b->g - a->g) * kc;
    o->b = a->b + (b->b - a->b) * kc;
    o->a = a->a + (b->a - a->a) * kc;
    o->fog = a->fog + (b->fog - a->fog) * k;
    o->valid = 1;
}

static void project(GxVtx *v) {
    float inv = 1.0f / v->w;
    v->sx = v->x * inv * gx.vpScale[0] + gx.vpTrans[0];
    v->sy = v->y * inv * gx.vpScale[1] + gx.vpTrans[1];
    v->sz = (v->z * inv * gx.vpScale[2] + gx.vpTrans[2]) * (1.0f / 1023.0f);
    if (gx.geom & GX_G_FOG) {
        /* neblina do ponto novo calculada pela posicao dele (como o RSP faz no fim do recorte), e nao
         * misturada: um vertice atras da camera tem neblina 255 e "embacava" o chao perto da camera */
        float f = (v->z * inv) * (float)gx.fogMul + (float)gx.fogOff;
        f = f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f);
        v->fog = f;
        v->a = f;
    }
}

#define W_EPS 1e-3f
/* Recorte como o F3DZEX2 NoN do jogo: SEM plano perto (o que fica entre a camera e o plano perto aparece,
 * com profundidade 0, em vez de sumir: chao cortado na frente da camera, cabeca "oca"...). So os planos
 * laterais com folga (x e y ate 2x a tela, a "clip ratio" do RSP) recortam, e eles tambem tiram o que
 * fica atras da camera. GB = folga. */
#define GB 2.0f
#define GB_OUT(v) ((v)->x < -GB * (v)->w || (v)->x > GB * (v)->w || (v)->y < -GB * (v)->w || (v)->y > GB * (v)->w || \
                   (v)->w < W_EPS)

static inline float clip_dist(const GxVtx *v, int plane) {
    switch (plane) {
        case 0: return v->x + GB * v->w;
        case 1: return GB * v->w - v->x;
        case 2: return v->y + GB * v->w;
        case 3: return GB * v->w - v->y;
        default: return v->w - W_EPS;
    }
}

/* recorta o poligono 'in' (n vertices) contra o plano d(v) >= 0; devolve o novo numero. O ponto novo e
 * sempre calculado do vertice de dentro para o de fora: dois triangulos com a mesma aresta recortada
 * chegam exatamente ao mesmo ponto (sem frestas entre eles). */
static int clip_poly(GxVtx *out, const GxVtx *in, int n, int plane) {
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const GxVtx *cur = &in[i], *nxt = &in[(i + 1) % n];
        const float dc = clip_dist(cur, plane), dn = clip_dist(nxt, plane);
        if (dc >= 0.0f) {
            out[m++] = *cur;
        }
        if ((dc >= 0.0f) != (dn >= 0.0f)) {
            if (dc >= 0.0f) {
                vtx_lerp(&out[m], cur, nxt, dc / (dc - dn));
            } else {
                vtx_lerp(&out[m], nxt, cur, dn / (dn - dc));
            }
            m++;
        }
    }
    return m;
}

static void emit_tri(int i0, int i1, int i2) {
    GxVtx *a = &gx.vtx[i0], *b = &gx.vtx[i1], *c = &gx.vtx[i2];
    GxVtx in[3];
    int k;
    int allIn;
    if (gx_ge_tri(a, b, c)) {
        return; /* (varredura do 2D, gfx_ge.c) */
    }
    {
        /* GX_TRILOG=1 no ambiente: lista todo triangulo (depuracao com o oot_replay) */
        static int tl = -1;
        if (tl < 0) {
            const char *e = getenv("GX_TRILOG");
            tl = (e != NULL && *e == '1');
        }
        if (tl) {
            fprintf(stderr, "[tri] cmd=%08x comb=%06x/%08x geom=%08x omH=%08x omL=%08x clip=%02x/%02x/%02x tela=(%.0f %.0f)(%.0f %.0f)(%.0f %.0f) w=%.2f/%.2f/%.2f\n",
                    gx.curCmd, gx.combW0, gx.combW1, gx.geom, gx.omH, gx.omL, a->clip, b->clip, c->clip, a->sx, a->sy, b->sx, b->sy,
                    c->sx, c->sy, a->w, b->w, c->w);
        }
    }

    gx.nTri++;
    gx.segTri[gx.curSeg]++;
    if (a->clip & b->clip & c->clip) {
        gx.nClipRej++;
        gx.segClip[gx.curSeg]++;
        if (gx.traceN > 0 && (gx.traceSeg < 0 || gx.traceSeg == gx.curSeg)) {
            const GxVtx *q[3];
            int j;
            q[0] = a;
            q[1] = b;
            q[2] = c;
            gx.traceN--;
            {
                if (!gx.shownT++) {
                    int r;
                    fprintf(stderr, "[trace] matriz mv*proj (linhas):\n");
                    for (r = 0; r < 4; r++) {
                        fprintf(stderr, "[trace]   %10.3f %10.3f %10.3f %10.3f\n", gx.mp[r * 4], gx.mp[r * 4 + 1],
                                gx.mp[r * 4 + 2], gx.mp[r * 4 + 3]);
                    }
                    fprintf(stderr, "[trace] viewport escala %.1f %.1f %.1f  trans %.1f %.1f %.1f\n", gx.vpScale[0],
                            gx.vpScale[1], gx.vpScale[2], gx.vpTrans[0], gx.vpTrans[1], gx.vpTrans[2]);
                }
            }
            fprintf(stderr, "[trace] tri fora geom=%08x clip=%02x/%02x/%02x\n", gx.geom, a->clip, b->clip, c->clip);
            for (j = 0; j < 3; j++) {
                fprintf(stderr, "[trace]   v clip(%.1f %.1f %.1f %.1f) tela(%.1f %.1f %.3f)\n", q[j]->x, q[j]->y, q[j]->z,
                        q[j]->w, q[j]->sx, q[j]->sy, q[j]->sz);
            }
        }
        return;
    }
    if (!(gx.geom & GX_G_SHADING_SMOOTH)) {
        in[0] = *a;
        in[1] = *b;
        in[2] = *c;
        in[1].r = in[2].r = in[0].r;
        in[1].g = in[2].g = in[0].g;
        in[1].b = in[2].b = in[0].b;
        if (!(gx.geom & GX_G_FOG)) {
            in[1].a = in[2].a = in[0].a;
        }
        a = &in[0];
        b = &in[1];
        c = &in[2];
    }
    /* precisa recortar? (algum vertice fora da folga lateral, ou atras da camera) */
    allIn = !(GB_OUT(a) || GB_OUT(b) || GB_OUT(c));
    /* descarte de faces: area com sinal em coordenadas de tela (y para baixo);
     * frente = anti-horario como visto na tela => area negativa.
     * Triangulos que cruzam o plano perto sao testados depois do recorte. */
    if ((gx.geom & (GX_G_CULL_FRONT | GX_G_CULL_BACK)) && allIn) {
        float cross = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
        if ((gx.geom & (GX_G_CULL_FRONT | GX_G_CULL_BACK)) == (GX_G_CULL_FRONT | GX_G_CULL_BACK)) {
            gx.nCull++;
            gx.segCull[gx.curSeg]++;
            return;
        }
        if (gx.cullFlip) {
            cross = -cross;
        }
        if (((gx.geom & GX_G_CULL_FRONT) && cross < 0.0f) || ((gx.geom & GX_G_CULL_BACK) && cross > 0.0f)) {
            gx.nCull++;
            gx.segCull[gx.curSeg]++;
            if (gx.traceN > 0 && (gx.traceSeg < 0 || gx.traceSeg == gx.curSeg)) {
                gx.traceN--;
                fprintf(stderr, "[trace] tri descartado por FACE geom=%08x area=%.1f flip=%d\n", gx.geom, cross, gx.cullFlip);
                fprintf(stderr, "[trace]   v tela(%.1f %.1f) w=%.2f | (%.1f %.1f) w=%.2f | (%.1f %.1f) w=%.2f\n", a->sx, a->sy,
                        a->w, b->sx, b->sy, b->w, c->sx, c->sy, c->w);
            }
            return;
        }
    }

    if (gx.traceN > 0 && gx.traceSeg >= 0 && gx.traceSeg == gx.curSeg) {
        gx.traceN--;
        fprintf(stderr, "[trace] tri ACEITO seg=%02X geom=%08x %s\n", gx.curSeg, gx.geom, allIn ? "" : "(recortado no plano perto)");
        fprintf(stderr, "[trace]   tela(%.1f %.1f %.3f) w=%.1f | (%.1f %.1f %.3f) w=%.1f | (%.1f %.1f %.3f) w=%.1f\n", a->sx, a->sy,
                a->sz, a->w, b->sx, b->sy, b->sz, b->w, c->sx, c->sy, c->sz, c->w);
    }
    if (allIn) {
        gx.nTriDrawn++;
        gx.segDrawn[gx.curSeg]++;
        gx_draw_tri(a, b, c);
        return;
    }
    /* recorta contra os 4 planos laterais com folga e, por seguranca, w >= W_EPS */
    {
        GxVtx pa[16], pb[16];
        int m = 3, pl;
        float area = 0.0f;
        pa[0] = *a;
        pa[1] = *b;
        pa[2] = *c;
        for (pl = 0; pl < 5; pl++) {
            m = clip_poly(pb, pa, m, pl);
            if (m < 3) {
                return;
            }
            memcpy(pa, pb, (size_t)m * sizeof pa[0]);
        }
        for (k = 0; k < m; k++) {
            project(&pa[k]);
        }
        if (gx.geom & (GX_G_CULL_FRONT | GX_G_CULL_BACK)) {
            /* area com sinal do poligono recortado (mesma regra do caso sem recorte) */
            for (k = 0; k < m; k++) {
                const GxVtx *p0 = &pa[k], *p1 = &pa[(k + 1) % m];
                area += p0->sx * p1->sy - p1->sx * p0->sy;
            }
            if (gx.cullFlip) {
                area = -area;
            }
            if ((gx.geom & (GX_G_CULL_FRONT | GX_G_CULL_BACK)) == (GX_G_CULL_FRONT | GX_G_CULL_BACK) ||
                ((gx.geom & GX_G_CULL_FRONT) && area < 0.0f) || ((gx.geom & GX_G_CULL_BACK) && area > 0.0f)) {
                gx.nCull++;
                gx.segCull[gx.curSeg]++;
                return;
            }
        }
        gx.segDrawn[gx.curSeg]++;
        for (k = 1; k + 1 < m; k++) {
            gx.nTriDrawn++;
            gx_draw_tri(&pa[0], &pa[k], &pa[k + 1]);
        }
    }
}

static void cmd_tri1(uint32_t w0) {
    int a = (int)((w0 >> 16) & 0xFF) >> 1, b = (int)((w0 >> 8) & 0xFF) >> 1, c = (int)(w0 & 0xFF) >> 1;
    if (a >= GX_VTX_MAX || b >= GX_VTX_MAX || c >= GX_VTX_MAX) {
        gx.nBad++;
        return;
    }
    emit_tri(a, b, c);
}

/* ------------------------------------------------------------------ outros comandos RSP */
/* quadros intermediarios: matriz interpolada no lugar da Mtx do jogo (NULL = nenhuma) */
const float *(*gx_mtx_lookup)(const void *mtx);

/* gIdentityMtx do jogo original (codigo do N64, US 1.1; baseroms/ntsc-1.1/config.yml do decomp): algumas salas trazem nas
 * listas de desenho o endereco fixo dela (o Jabu-Jabu: gsSPMatrix(0x800FEF70) antes do teste da caixa de cada pedaco). No
 * port o codigo do jogo nao fica ali, e o endereco na RAM do N64 tem outros dados: a caixa saia com uma matriz qualquer, o
 * teste dava "fora da tela" e partes da sala sumiam ate o Link chegar perto (11/2026) */
#define GX_N64_IDENTITY_MTX 0x800FEF70u

static void cmd_mtx(uint32_t w0, uint32_t w1) {
    uint32_t p = (w0 & 0xFF) ^ 1u; /* G_MTX_PUSH invertido na codificacao do F3DEX2 */
    const uint32_t *src;
    float L[16];
    if (w1 == GX_N64_IDENTITY_MTX) {
        static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(L, id, sizeof L);
        src = NULL;
        goto loaded;
    }
    src = (const uint32_t *)gx_resolve(w1);
    if (src == NULL) {
        return;
    }
    gx_touch(src, 64);
    {
        /* quadro intermediario (frame_interp.c): esta Mtx tem uma versao interpolada? */
        const float *ri = gx_mtx_lookup != NULL ? gx_mtx_lookup(src) : NULL;
        if (ri != NULL) {
            memcpy(L, ri, sizeof L);
        } else {
            mat_load_n64(L, src);
        }
    }
    if (gx.traceN > 0) {
        if (gx.shownM < 6) {
            int r;
            gx.shownM++;
            fprintf(stderr, "[trace] mtx addr=%08x -> %p flags=%u (%s%s%s)\n", w1, (const void *)src, p,
                    (p & 4) ? "proj " : "mv ", (p & 2) ? "load " : "mul ", (p & 1) ? "push" : "");
            fprintf(stderr, "[trace]   palavras: ");
            for (r = 0; r < 16; r++) {
                fprintf(stderr, "%08x ", src[r]);
            }
            fprintf(stderr, "\n");
            for (r = 0; r < 4; r++) {
                fprintf(stderr, "[trace]   %10.4f %10.4f %10.4f %10.4f\n", L[r * 4], L[r * 4 + 1], L[r * 4 + 2], L[r * 4 + 3]);
            }
        }
    }
    if ((p & 4) && !(p & 2) && gx.traceArmed && gx.shownP < 6) {
        /* matriz de visao (guLookAt) multiplicada na projecao: decodifica camera */
        float ex, ey, ez;
        gx.shownP++;
        ex = -(L[12] * L[0] + L[13] * L[1] + L[14] * L[2]);
        ey = -(L[12] * L[4] + L[13] * L[5] + L[14] * L[6]);
        ez = -(L[12] * L[8] + L[13] * L[9] + L[14] * L[10]);
        fprintf(stderr, "[trace] visao addr=%08x: olho=(%.1f %.1f %.1f) olhando para (%.3f %.3f %.3f) cima=(%.3f %.3f %.3f)\n", w1,
                ex, ey, ez, -L[2], -L[6], -L[10], L[1], L[5], L[9]);
    }
loaded:
    if (p & 4) { /* projecao */
        if (p & 2) {
            memcpy(gx.proj, L, sizeof L);
        } else {
            mat_mul(gx.proj, L, gx.proj);
        }
    } else {
        if (p & 1) {
            if (gx.mvTop + 1 < GX_MV_STACK) {
                memcpy(gx.mv[gx.mvTop + 1], gx.mv[gx.mvTop], sizeof L);
                gx.mvTop++;
            }
        }
        if (p & 2) {
            memcpy(gx.mv[gx.mvTop], L, sizeof L);
        } else {
            mat_mul(gx.mv[gx.mvTop], L, gx.mv[gx.mvTop]);
        }
    }
    gx.mpDirty = 1;
    gx.lightsDirty = 1;
}

static void cmd_movemem(uint32_t w0, uint32_t w1) {
    int index = (int)(w0 & 0xFF);
    int offset = (int)((w0 >> 8) & 0xFF) * 8;
    const uint8_t *src = (const uint8_t *)gx_resolve(w1);
    if (src == NULL) {
        return;
    }
    gx_touch(src, 32);
    if (index == GX_MV_VIEWPORT) {
        int16_t vp[8];
        int i;
        memcpy(vp, src, 16);
        for (i = 0; i < 4; i++) {
            /* x,y em 1/4 de pixel; z em unidades de 10 bits (0..1023), sem escala */
            float k = (i == 2) ? 1.0f : 0.25f;
            gx.vpScale[i] = (float)vp[i] * k;
            gx.vpTrans[i] = (float)vp[4 + i] * k;
        }
        /* o RSP inverte o Y: Y da tela cresce para baixo, Y do clip cresce para cima.
           O jogo manda vscale[1] positivo (z_view.c). --vp-flip desfaz isto (diagnostico). */
        if (!gx.vpFlip) {
            gx.vpScale[1] = -gx.vpScale[1];
        }
        gx.vpRawS[0] = gx.vpScale[0];
        gx.vpRawS[1] = gx.vpScale[1];
        gx.vpRawT[0] = gx.vpTrans[0];
        gx.vpRawT[1] = gx.vpTrans[1];
        gx_hr_viewport(); /* x,y na escala do alvo (resolucao maior) */
    } else if (index == GX_MV_LIGHT) {
        int slot = offset / 24;
        if (slot == 0 || slot == 1) {
            gx.lookat[slot].dir[0] = (int8_t)src[8];
            gx.lookat[slot].dir[1] = (int8_t)src[9];
            gx.lookat[slot].dir[2] = (int8_t)src[10];
        } else {
            int n = slot - 1;
            if (n >= 1 && n <= 9) {
                gx.light[n].col[0] = src[0];
                gx.light[n].col[1] = src[1];
                gx.light[n].col[2] = src[2];
                gx.light[n].dir[0] = (int8_t)src[8];
                gx.light[n].dir[1] = (int8_t)src[9];
                gx.light[n].dir[2] = (int8_t)src[10];
            }
            gx.lightsDirty = 1;
        }
    }
}

static void cmd_moveword(uint32_t w0, uint32_t w1) {
    int index = (int)((w0 >> 16) & 0xFF);
    int offset = (int)(w0 & 0xFFFF);
    switch (index) {
        case GX_MW_SEGMENT:
            gx.seg[(offset >> 2) & 0xF] = w1 & 0x1FFFFFFFu;
            break;
        case GX_MW_NUMLIGHT:
            gx.numLights = (int)(w1 / 24);
            if (gx.numLights > 7) {
                gx.numLights = 7;
            }
            gx.lightsDirty = 1;
            break;
        case GX_MW_FOG:
            gx.fogMul = (int16_t)(w1 >> 16);
            gx.fogOff = (int16_t)(w1 & 0xFFFF);
            break;
        case GX_MW_LIGHTCOL: {
            int n = offset / 0x18 + 1;
            if (n >= 1 && n <= 9) {
                gx.light[n].col[0] = (uint8_t)(w1 >> 24);
                gx.light[n].col[1] = (uint8_t)(w1 >> 16);
                gx.light[n].col[2] = (uint8_t)(w1 >> 8);
            }
            break;
        }
        default:
            break;
    }
}

static void cmd_modifyvtx(uint32_t w0, uint32_t w1) {
    int where = (int)((w0 >> 16) & 0xFF);
    int vi = (int)(w0 & 0xFFFF) / 2;
    GxVtx *v;
    if (vi < 0 || vi >= GX_VTX_MAX) {
        return;
    }
    v = &gx.vtx[vi];
    switch (where) {
        case 0x10:
            v->r = gx_gamma_on ? gx_gammaf[w1 >> 24] : (float)(w1 >> 24);
            v->g = gx_gamma_on ? gx_gammaf[(w1 >> 16) & 0xFF] : (float)((w1 >> 16) & 0xFF);
            v->b = gx_gamma_on ? gx_gammaf[(w1 >> 8) & 0xFF] : (float)((w1 >> 8) & 0xFF);
            v->a = (float)(w1 & 0xFF);
            break;
        case 0x14:
            v->s = (float)(int16_t)(w1 >> 16);
            v->t = (float)(int16_t)(w1 & 0xFFFF);
            break;
        case 0x18:
            v->sx = (float)(int16_t)(w1 >> 16) * 0.25f;
            v->sy = (float)(int16_t)(w1 & 0xFFFF) * 0.25f;
            break;
        case 0x1C:
            v->sz = (float)(w1 >> 16) * (1.0f / 65535.0f);
            break;
        default:
            break;
    }
}

void gx_reset_frame(void) {
    int i;
    memset(gx.seg, 0, sizeof gx.seg);
    gx.mvTop = 0;
    mat_ident(gx.mv[0]);
    mat_ident(gx.proj);
    mat_ident(gx.mp);
    gx.mpDirty = 1;
    gx.lightsDirty = 1;
    gx.geom = 0;
    gx.numLights = 0;
    gx.texScaleS = gx.texScaleT = 1.0f;
    for (i = 0; i < 4; i++) {
        gx.vpScale[i] = 0.0f;
        gx.vpTrans[i] = 0.0f;
    }
    gx.vpScale[0] = 160.0f;
    gx.vpScale[1] = -120.0f;
    gx.vpScale[2] = 511.0f;
    gx.vpTrans[0] = 160.0f;
    gx.vpTrans[1] = 120.0f;
    gx.vpTrans[2] = 511.0f;
    gx.vpRawS[0] = 160.0f;
    gx.vpRawS[1] = -120.0f;
    gx.vpRawT[0] = 160.0f;
    gx.vpRawT[1] = 120.0f;
    gx_hr_viewport();
    gx.nCmd = gx.nTri = gx.nTriDrawn = gx.nRect = gx.nTexLoad = gx.nBad = gx.nCull = gx.nClipRej = gx.nVtx = 0;
    memset(gx.opHist, 0, sizeof gx.opHist);
    gx.ucodeSwitch = 0;
    gx.curSeg = 16;
    memset(gx.segVtx, 0, sizeof gx.segVtx);
    memset(gx.segDl, 0, sizeof gx.segDl);
    memset(gx.segTri, 0, sizeof gx.segTri);
    memset(gx.segDrawn, 0, sizeof gx.segDrawn);
    memset(gx.segClip, 0, sizeof gx.segClip);
    memset(gx.segCull, 0, sizeof gx.segCull);
    memset(gx.segPx, 0, sizeof gx.segPx);
    memset(gx.segZRej, 0, sizeof gx.segZRej);
    memset(gx.segARej, 0, sizeof gx.segARej);
    memset(gx.segVisit, 0, sizeof gx.segVisit);
    for (i = 0; i < 17; i++) {
        gx.segX0[i] = gx.segY0[i] = 100000;
        gx.segX1[i] = gx.segY1[i] = -1;
    }
}

/* Executa a display list a partir de 'addr'. Devolve 0 se ok. */
/* 2D pelo acelerador (gfx_ge.c): na marca do jogo guarda o estado do RSP/RDP e da lista; no fim da lista
 * (varredura feita) volta a ele e passa de novo, agora desenhando */
static GxState g_geSnap;
static const GxCmd *g_geStack[GX_DL_STACK], *g_gePc;
static int g_geSp;

int gx_run_dl(uint32_t addr, volatile int *yield) {
    const GxCmd *stack[GX_DL_STACK];
    int sp = 0;
    const GxCmd *pc;
    uint32_t guard = 0;
    (void)yield;
    gx_gamma_update(); /* opcao brightness (gfx_rdp.c) */
    pc = (const GxCmd *)gx_resolve(addr);
    if (pc == NULL) {
        return 1;
    }
    for (;;) {
        if (0) {
        dl_end:
            /* fim da lista: se estava so varrendo o 2D, volta a marca e desenha */
            if (!gx_ge_scanning()) {
                return 0;
            }
            gx_ge_scan_end(0);
        ge_rewind:
            memcpy(&gx, &g_geSnap, sizeof gx);
            memcpy(stack, g_geStack, sizeof stack);
            sp = g_geSp;
            pc = g_gePc;
            continue;
        }
        uint32_t w0, w1, op;
        gx_touch(pc, 24); /* comando (+ RDPHALF_1/2 dos retangulos) */
        w0 = pc->w0;
        w1 = pc->w1;
        op = w0 >> 24;
        gx.curCmd = (uint32_t)(uintptr_t)pc;
        pc++;
        gx.nCmd++;
        gx.opHist[op]++;
        if (++guard > 4000000u) {
            gx_log("DL: limite de comandos excedido");
            if (gx_ge_scanning()) {
                gx_ge_scan_end(1);
                memcpy(&gx, &g_geSnap, sizeof gx);
            }
            return 1;
        }
        switch (op) {
            case GX_NOOP:
                if (w1 == 0x54585440u) {
                    extern int gx_arquivos_320;
                    if (gx_arquivos_320) {
                        gx_hr_menu_mark(1); /* tela de arquivos: em 320x240 como o menu de pausa (gfx_hires.c) */
                    }
                } else if (w1 == 0x5458543Au || w1 == 0x5458543Bu) {
                    gx_solid_alpha = (w1 == 0x5458543Au) && gx_menu_opaco ? 1 : 0; /* menu de pausa opaco (gfx_rdp.c) */
                    if (w1 == 0x5458543Au && gx_menu_320) {
                        gx_hr_menu_mark(1); /* (e o desenho seguinte pode ser em 320: gfx_hires.c) */
                    }
                } else if (w1 >= 0x54585441u && w1 <= 0x54585444u) {
                    extern void gx_hd_logo_mark(int bit, int on) __attribute__((weak));
                    if (gx_hd_logo_mark) {
                        /* (opcao hd_textures 2, gfx_hd.c: 41/42 fundos pre-renderizados, 43/44 a logo da abertura inteira) */
                        gx_hd_logo_mark(w1 <= 0x54585442u ? 8 : 1, w1 == 0x54585441u || w1 == 0x54585443u);
                    }
                } else if (w1 == 0x5458543Eu || w1 == 0x5458543Fu) {
                    extern int gx_hd_font_ok;
                    extern void gx_hd_logo_mark(int bit, int on) __attribute__((weak));
                    gx_hd_font_ok = (w1 == 0x5458543Eu); /* PRESS START com as letras novas (gfx_hd.c) */
                    if (gx_hd_logo_mark) {
                        gx_hd_logo_mark(4, w1 == 0x5458543Eu); /* (opcao hd_textures 2) */
                    }
                } else if (w1 == 0x5458543Cu || w1 == 0x5458543Du) {
                    gx_solid_alpha = (w1 == 0x5458543Cu) && gx_logo_opaca ? 2 : 0; /* logo do escudo sem transparencia */
                    if (w1 == 0x5458543Cu) {
                        gx_ge_logo_mark(); /* (a logo e os textos dela vao todos pelo acelerador: gfx_ge.c) */
                    }
                    {
                        extern void gx_hd_logo_mark(int bit, int on) __attribute__((weak));
                        if (gx_hd_logo_mark) {
                            gx_hd_logo_mark(2, w1 == 0x5458543Cu); /* (opcao hd_textures 2) */
                        }
                    }
                } else if ((w1 & 0xFFFFFFF0u) == 0x54585430u) {
                    /* marcas da caixa de texto (z_message.c, tools/oot-patch.py) */
                    static int tx0, ty0;
                    if (gx_ge_scanning()) {
                        /* (varredura do 2D: a caixa de texto pode ir inteira para o acelerador, gfx_ge.c) */
                        if (w1 == GX_TAG_TXT_A) {
                            tx0 = (int)((w0 >> 12) & 0xFFF);
                            ty0 = (int)(w0 & 0xFFF);
                        } else if (w1 == GX_TAG_TXT_B) {
                            gx_ge_text(1, tx0, ty0, (int)((w0 >> 12) & 0xFFF), (int)(w0 & 0xFFF));
                        } else if (w1 == GX_TAG_TXT_END) {
                            gx_ge_text(0, 0, 0, 0, 0);
                        }
                    } else if (w1 == GX_TAG_TXT_A) {
                        tx0 = (int)((w0 >> 12) & 0xFFF);
                        ty0 = (int)(w0 & 0xFFF);
                    } else if (w1 == GX_TAG_TXT_B) {
                        if (!gx_ge_group(1)) {
                            gx_hr_text(1, tx0, ty0, (int)((w0 >> 12) & 0xFFF), (int)(w0 & 0xFFF));
                        }
                    } else if (w1 == GX_TAG_TXT_END) {
                        if (!gx_ge_group(0)) {
                            gx_hr_text(0, 0, 0, 0, 0);
                        }
                    } else if (w1 >= 0x54585434u && w1 <= 0x54585439u) {
                        gx_hr_tag(w1, w0); /* fundo da pausa, Link do menu em 2x (gfx_hires.c) */
                    } else if (w1 == GX_TAG_GE && gx_ge_scan_begin()) {
                        /* comeco do 2D: guarda tudo para passar de novo depois da varredura */
                        memcpy(&g_geSnap, &gx, sizeof gx);
                        memcpy(g_geStack, stack, sizeof stack);
                        g_geSp = sp;
                        g_gePc = pc;
                    }
                }
                break;
            case GX_SPNOOP:
            case GX_RDPNOOP:
            case GX_RDPLOADSYNC:
            case GX_RDPPIPESYNC:
            case GX_RDPTILESYNC:
            case GX_RDPFULLSYNC:
            case GX_LINE3D:
                break;
            case GX_VTX:
                cmd_vtx(w0, w1);
                break;
            case GX_MODIFYVTX:
                cmd_modifyvtx(w0, w1);
                break;
            case GX_CULLDL: {
                int a = (int)(w0 & 0xFFFF) / 2, b = (int)(w1 & 0xFFFF) / 2, i;
                uint8_t f = 0xFF;
                if (a >= 0 && b < GX_VTX_MAX && a <= b) {
                    for (i = a; i <= b; i++) {
                        f &= gx.vtx[i].clip;
                    }
                    if (f != 0) {
                        if (sp == 0) {
                            goto dl_end;
                        }
                        pc = stack[--sp];
                    }
                }
                break;
            }
            case GX_BRANCH_Z: {
                int vi = (int)(w0 & 0xFFF) / 2;
                if (vi >= 0 && vi < GX_VTX_MAX) {
                    /* nivel de detalhe (gsSPBranchLessZraw): o valor do comando e uma distancia (800, 1200, 2800...
                     * unidades; 400000000 = sempre): vai para a lista detalhada se o z do vertice no espaco de recorte
                     * (antes de dividir por w) for ate ele, como no libultraship do SoH. Antes comparava o z de tela
                     * (0..1) escalado: a lista detalhada quase nunca era escolhida e partes do cenario sumiam conforme
                     * a camera (o chao da subida da Montanha da Morte, 03/11). */
                    if (gx.vtx[vi].z <= (float)(int32_t)w1 || gx.vtx[vi].w <= 0.0f) {
                        const GxCmd *t = (const GxCmd *)gx_resolve(gx.rdphalf1);
                        if (t != NULL) {
                            pc = t;
                        }
                    }
                }
                break;
            }
            case GX_TRI1:
                cmd_tri1(w0);
                break;
            case GX_TRI2:
            case GX_QUAD:
                cmd_tri1(w0);
                cmd_tri1(w1);
                break;
            case GX_TEXTURE:
                gx.texLevel = (int)((w0 >> 11) & 7);
                gx.texTile = (int)((w0 >> 8) & 7);
                gx.texOn = (int)((w0 >> 1) & 0x7F);
                {
                    uint32_t sc = (w1 >> 16) & 0xFFFF, tc = w1 & 0xFFFF;
                    gx.texScaleS = (sc == 0xFFFF) ? 1.0f : (float)sc / 65536.0f;
                    gx.texScaleT = (tc == 0xFFFF) ? 1.0f : (float)tc / 65536.0f;
                }
                gx.texDirty = 1;
                break;
            case GX_POPMTX: {
                int n = (int)(w1 >> 6);
                if (n > 0 && gx.mvTop - n >= 0) {
                    gx.mvTop -= n;
                    gx.mpDirty = 1;
                    gx.lightsDirty = 1;
                }
                break;
            }
            case GX_GEOMETRYMODE:
                gx.geom = (gx.geom & (w0 & 0x00FFFFFFu)) | w1;
                break;
            case GX_MTX:
                cmd_mtx(w0, w1);
                break;
            case GX_MOVEWORD:
                cmd_moveword(w0, w1);
                break;
            case GX_MOVEMEM:
                cmd_movemem(w0, w1);
                break;
            case GX_LOAD_UCODE:
                gx.ucodeSwitch++; /* S2DEX2 e volta: so os comandos de fundo (abaixo) mudam */
                break;
            case GX_BG_COPY:
            case GX_BG_1CYC:
                gx_bg_rect(w1, op == GX_BG_COPY);
                break;
            case GX_OBJ_RENDERMODE:
                break;
            case GX_DL: {
                const GxCmd *t = (const GxCmd *)gx_resolve(w1);
                gx.segDl[seg_index(w1)]++;
                if (t == NULL) {
                    break;
                }
                if (((w0 >> 16) & 0xFF) == 0) { /* chamada */
                    if (sp < GX_DL_STACK) {
                        stack[sp++] = pc;
                    }
                }
                pc = t;
                break;
            }
            case GX_ENDDL:
                if (sp == 0) {
                    goto dl_end;
                }
                pc = stack[--sp];
                break;
            case GX_RDPHALF_1:
                gx.rdphalf1 = w1;
                break;
            case GX_RDPHALF_2:
                gx.rdphalf2 = w1;
                break;
            case GX_SETOTHERMODE_L: {
                int len = (int)(w0 & 0xFF) + 1, sft = 32 - (int)((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (len >= 32) ? 0xFFFFFFFFu : (((1u << len) - 1u) << sft);
                gx.omL = (gx.omL & ~mask) | (w1 & mask);
                break;
            }
            case GX_SETOTHERMODE_H: {
                int len = (int)(w0 & 0xFF) + 1, sft = 32 - (int)((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (len >= 32) ? 0xFFFFFFFFu : (((1u << len) - 1u) << sft);
                gx.omH = (gx.omH & ~mask) | (w1 & mask);
                gx.texDirty = 1;
                break;
            }
            case GX_RDPSETOTHERMODE:
                gx.omH = w0 & 0x00FFFFFFu;
                gx.omL = w1;
                gx.texDirty = 1;
                break;
            case GX_TEXRECT:
            case GX_TEXRECTFLIP: {
                /* seguido de RDPHALF_1 e RDPHALF_2 */
                uint32_t h1 = pc[0].w1, h2 = pc[1].w1;
                pc += 2;
                if (gx_cmd_lookup != NULL) {
                    /* quadro intermediario: retangulo 2D com posicao/tamanho intermediarios (caixa de dialogo) */
                    const uint32_t *r = gx_cmd_lookup((const void *)(uintptr_t)gx.curCmd);
                    if (r != NULL) {
                        w0 = r[0];
                        w1 = r[1];
                        h1 = r[2];
                        h2 = r[3];
                    }
                }
                gx.opHist[GX_RDPHALF_1]++;
                gx.opHist[GX_RDPHALF_2]++;
                gx_texrect(w0, w1, h1, h2, op == GX_TEXRECTFLIP);
                break;
            }
            default:
                if (op >= 0xC0) {
                    if ((op == GX_SETCIMG || op == GX_SETZIMG) && gx_ge_scanning()) {
                        /* troca de alvo no meio do 2D: sem acelerador neste desenho */
                        gx_ge_scan_end(1);
                        goto ge_rewind;
                    }
                    gx_rdp_cmd(op, w0, w1);
                } else if (op >= 0xD7 || op <= 0x08) {
                    /* comando RSP desconhecido */
                } else {
                    /* byte de comando de outro microcodigo (S2DEX etc.) */
                }
                break;
        }
    }
}

/* Renderiza uma tarefa grafica completa (uma display list de nivel mais alto). */
int gx_render(uint32_t dlAddr, volatile int *yield) {
    static int inited;
    if (!inited) {
        gx_rdp_init();
        inited = 1;
    }
    gx_reset_frame();
    gx.taskNo++;
    {
        int r = gx_run_dl(dlAddr, yield);
        gx_batch_flush(); /* a fila do caminho rapido termina junto com a tarefa */
        return r;
    }
}
