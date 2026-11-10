/*
 * gfx_hires.c -- resolucao maior (e dinamica) para a tela do jogo.
 *
 * O jogo desenha a tela em 320x240 (RGBA5551, na RAM do N64). Aqui, quando o alvo de um SETCIMG e um
 * framebuffer que vai para o VI (uma "tela"), o desenho vai para um buffer nosso de W x H (ate 640x480),
 * e todas as coordenadas de tela sao escaladas por rs = W / 320: viewport (gfx_rsp.c), tesoura e
 * retangulos (gfx_rdp.c). O z-buffer do jogo tambem ganha um buffer maior.
 *
 * O jogo continua vendo a sua tela de 320x240: no fim de cada desenho (e antes de uma textura ser tirada
 * da tela no meio dele), a imagem maior e reduzida para a RAM do N64. Quem mostra a imagem (a thread de
 * tela) pega a maior (gx_hr_image).
 *
 * Tamanho dinamico: degraus de 640x480 ate 320x240. Depois de cada desenho, se ele passou do alvo de
 * tempo, o proximo desce os degraus que a estimativa pede; se a do degrau de cima cabe com folga por um
 * tempo, sobe um.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_priv.h"
#include <time.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

int gx_hr_max = 640;       /* largura maxima em uso (gx_hr_sync_opts: a opcao, ou 320 com o filtro retro) */
int gx_hr_max_opt = 640;   /* opcao resolucao: largura maxima (320 = como o jogo, desligado) */
/* opcao retro_filter: filtro de TV de tubo (video_miyoo.c): 0 = nao, 1 = linhas, 2 = linhas e mascara de cor. Com ele, o
 * jogo fica em 320x240 (como o N64) e cada linha do jogo ocupa 2 linhas da tela: as linhas escuras do filtro casam com
 * as do jogo. retro_filter_strength: quanto as linhas escurecem (%) */
int gx_retro = 0;
int gx_retro_forca = 50;
int gx_retro_tv = 1; /* opcao retro_filter_tv: cantos arredondados e bordas escurecidas de TV de tubo (video_miyoo.c) */
int gx_hr_alvo = 30;       /* opcao resolucao_alvo: ms por desenho que a resolucao dinamica tenta nao passar */
int gx_hr_fixo;            /* 1 = sempre o tamanho maximo (teste no PC) */
int gx_hr_ctl2 = 2;        /* opcao controle_resolucao: 1 = como antes; 2 = previsao por pixel (gx_hr_pass_end) */
double gx_hr_fixo_ms = 3.0; /* parte do desenho que nao depende da resolucao (vertices, comandos), ms */

/* degraus de 32 pixels de largura (cerca de 10% de pixels cada): com degraus grandes (640, 560, 480, 400)
 * o desenho ficava bem abaixo do alvo (o degrau de cima nao cabia) e os nucleos sobravam */
#define HR_LV 11
static int k_lvW[HR_LV] = { 640, 608, 576, 544, 512, 480, 448, 416, 384, 352, 320 };
/* tela de 752x560 (Mini v4, Mini Flip; gx_hr_set_panel): degraus ate o tamanho da tela. A imagem tem o formato da tela
 * (altura = largura * 560 / 752) e a escala e a mesma nos dois sentidos (largura / 320): a imagem do N64 sai inteira na
 * largura e perde ~1,7 linha do N64 embaixo (0,7%). Com o filtro retro: 376x280 fixo, cada linha do jogo em 2 da tela */
static const int k_lvW560[HR_LV] = { 752, 712, 672, 632, 592, 552, 512, 464, 416, 376, 320 };
#define RETRO560_W 376
static int g_maxW = 640, g_maxH = 480; /* maior imagem (buffers) */
#define HR_MAXW g_maxW
#define HR_MAXH g_maxH
#define FB_W 320
#define FB_H 240
int gx_hr_pan560; /* tela de 752x560 */

void gx_hr_set_panel(int w, int h) {
    if (w == 752 && h == 560) {
        int i;
        for (i = 0; i < HR_LV; i++) {
            k_lvW[i] = k_lvW560[i];
        }
        g_maxW = 752;
        g_maxH = 560;
        gx_hr_pan560 = 1;
        fprintf(stderr, "[resolucao] tela de 752x560: desenho ate 752x560 (filtro retro: 376x280)\n");
    }
}

/* bytes da maior imagem (gfx_hook.c: imagens da fila) */
size_t gx_hr_img_bytes(void) {
    return (size_t)g_maxW * (size_t)g_maxH * 2u + 64u;
}

/* altura da imagem para uma largura */
static int pass_h(int w) {
    return gx_hr_pan560 ? (w * 560 + 376) / 752 : w * 3 / 4;
}

/* linha da imagem maior (w x h) que corresponde a linha y do N64: a escala e w / 320 tambem na altura */
static int src_row(int y, int w, int h) {
    const int hc = w * 3 / 4; /* (altura que a imagem teria inteira; = h fora da tela de 752x560) */
    const int r = (y * hc + hc / 2) / FB_H;
    return r < h ? r : h - 1;
}

typedef struct {
    const uint8_t *ram; /* framebuffer do jogo (tela) */
    uint8_t *buf;       /* imagem maior (a propria, own, ou a imagem da fila da tela em que o desenho foi feito) */
    uint8_t *own;
    int w, h;           /* tamanho da imagem em buf */
    int dirty;          /* desenhado desde a ultima reducao para a RAM */
    int valid;          /* buf tem a imagem do ultimo desenho desta tela */
    unsigned use;
    /* caixa de texto em 640x480 (gx_hr_text): a regiao txt[0..3] de tbuf vai por cima da imagem na tela */
    uint8_t *tbuf;
    uint8_t *t320;      /* (text_box_320) a caixa desenhada em 320x240 antes de ir para tbuf */
    int txt[4];
    int txtOn;
    unsigned pass;      /* desenho em que foi alvo pela ultima vez */
} HrSlot;

#define HR_SLOTS 3
static HrSlot g_slot[HR_SLOTS];
static unsigned g_use;
static const uint8_t *g_zRam; /* z-buffer do jogo (o que acompanha as telas) */
static uint8_t *g_zBuf;
static int g_zDirty;
static HrSlot *g_cur; /* alvo atual (NULL: z ou nativo) */

static unsigned g_passNo;
/* caixa de texto (gx_hr_text) */
static int g_tx[4];       /* regiao pedida (pixels de 320x240): x0 y0 x1 y1 */
static int g_tOn;         /* desenhando a caixa de texto em 640x480 */
static int g_t320;        /* (text_box_320) a caixa vai em 320x240 e e ampliada para tbuf no fim */
static unsigned g_t320Stat[2]; /* caixas pelos nucleos em 320: sobre imagem maior / desenho ja em 320 */
extern int gx_texto_320;  /* gfx_ge.c: opcao text_box_320 */
static HrSlot *g_tSlot;   /* a tela que recebe a caixa */
int gx_hr_texto = 0;      /* opcao texto_nitido (sharp_text_boxes): 0 = a caixa sai no proprio desenho, sem sincronizar os nucleos */
static unsigned g_tStat[5]; /* diagnostico: caixas em 640 sobre imagem maior / sobre 320 / ja em 640 / recusadas / sem fim */

static int g_lv = HR_LV - 1; /* degrau do proximo desenho */
static int g_passW = FB_W, g_passH = FB_H;
static float g_passS = 1.0f;
static double g_ema;
static int g_up, g_hold;
static unsigned g_lvHist[HR_LV], g_lvDown, g_lvUp;

/* fundo do menu de pausa (marcas em z_play.c): a imagem maior do quadro em que o jogo guarda a tela */
static uint8_t *g_pbg;
static int g_pbgW, g_pbgH, g_pbgLv, g_pbgValid, g_pbgSkip, g_pbgUsed, g_pbgUsedLast;
static unsigned g_pbgStat[3]; /* guardadas, desenhadas, sem imagem maior */
/* quadro em que a pausa comeca (marca 0x34): o jogo copia a tela para o z-buffer (fundo do menu). Ate o fim desse
 * desenho, escrever no z-buffer e escrever na RAM do N64 (antes ia para o z maior, e o fundo do menu saia com lixo de
 * profundidade: listras coloridas), e o z maior nao e mais reduzido por cima do fundo guardado. */
static int g_pbgCopy;

/* Link da tela de equipamentos em 2x (marcas em z_player_lib.c e z_kaleido_scope.c) */
#define PRV_W 64
#define PRV_H 112
static int g_prvArm;               /* entre as marcas do desenho do Link */
static const uint8_t *g_prvRam[2]; /* imagens de 64 de largura que o jogo usou (cor e z) */
static uint8_t *g_prvBuf[2];       /* as mesmas em 2x (128 x 224) */
static int g_prvColor = -1;        /* qual delas e a cor (o ultimo alvo de desenho) */
static uint8_t *g_prvImg;          /* o ultimo Link desenhado em 2x */
static int g_prvImgOk;
static uint8_t *g_prvTexImg;       /* o Link que foi para a textura do menu */
static uint32_t g_prvTex;          /* endereco da textura do menu (0 = nenhuma) */
/* a marca 0x39 apareceu neste desenho (o menu de pausa esta aberto e o jogo acabou de gravar a textura do Link nele).
 * Sem o menu, o endereco da textura volta a ser memoria comum do jogo: uma textura carregada depois dali (o cabelo do
 * Link, que bugava preto/escuro) recebia o Link do menu em 2x como "textura nova", e com hd_textures ligado ela era
 * usada. Agora so dentro do desenho com o menu. */
static int g_prvLive;
static unsigned g_prvStat[7];      /* desenhos em 2x, texturas trocadas, cargas recusadas; marcas da textura (com / sem Link
                                    * em 2x); cargas de outro endereco (diagnostico) */
static uint32_t g_prvLastOff;

/* desenho direto na imagem da fila da tela (opcao desenho_direto): o desenho intermediario/do quadro usa como imagem
 * maior o proprio buffer que vai para a tela (gfx_hook.c), sem a copia de ~600 KB depois de cada desenho. A tela do
 * N64 continua tendo a imagem dela enquanto o buffer nao volta a ser usado pela fila (gx_hr_release reduz antes) */
int gx_hr_direto = 1;
static uint8_t *g_ext;
static int g_inBegin; /* (gx_hr_pass_begin reaplica o alvo do desenho anterior: nao troca o buffer dele) */

void gx_hr_target(void *p) {
    g_ext = gx_hr_direto ? (uint8_t *)p : NULL;
}

static void slot_reduce(HrSlot *s);

/* o buffer p vai ser reaproveitado pela fila: a tela que o usava como imagem maior volta a ter a propria */
void gx_hr_release(const void *p) {
    int i;
    if (p == NULL) {
        return;
    }
    for (i = 0; i < HR_SLOTS; i++) {
        HrSlot *s = &g_slot[i];
        if (s->buf != NULL && s->buf == (const uint8_t *)p) {
            if (s->dirty) {
                slot_reduce(s);
                s->dirty = 0;
            }
            s->valid = 0;
            s->buf = s->own;
        }
    }
}

/* fim do desenho com alvo externo: so a tela que vai para a fila (fbRaw) fica com ele */
void gx_hr_target_done(const void *fbRaw) {
    int i;
    if (g_ext != NULL) {
        for (i = 0; i < HR_SLOTS; i++) {
            HrSlot *s = &g_slot[i];
            if (s->buf == g_ext && s->ram != (const uint8_t *)fbRaw) {
                if (s->dirty) {
                    slot_reduce(s);
                    s->dirty = 0;
                }
                s->valid = 0;
                s->buf = s->own;
            }
        }
    }
    g_ext = NULL;
}

/* z dos pontos (opcao z_pontos): o jogo le pela CPU so alguns pixels do z (o do sol e o centro de cada halo), uma vez
 * por quadro dele. Em vez de reduzir o z inteiro (640x480 -> 320x240) no fim de cada quadro de verdade, guarda so uma
 * janela de 9x9 em volta de cada ponto que o jogo leu nos ultimos desenhos (a Navi anda alguns pixels por quadro: o
 * ponto novo cai na janela). Um ponto fora de todas as janelas e lido na hora da imagem maior. */
int gx_hr_zpontos = 1;
#define ZP_N 24
#define ZP_R 4
#define ZP_D (2 * ZP_R + 1)
static struct {
    int16_t x, y;            /* centro da janela (o ultimo lido) */
    int16_t wx, wy;          /* centro em que a janela foi guardada */
    uint16_t v[ZP_D * ZP_D];
    unsigned seen;           /* desenho em que foi lido pela ultima vez */
    int ok;                  /* janela guardada */
} g_zp[ZP_N];
static volatile int g_zHi; /* o z do ultimo quadro de verdade esta na imagem maior (nao na RAM) */
static int g_zW = FB_W, g_zH = FB_H;

static uint16_t zbuf_at(int x, int y) {
    int sx, sy;
    x = x < 0 ? 0 : (x >= FB_W ? FB_W - 1 : x);
    y = y < 0 ? 0 : (y >= FB_H ? FB_H - 1 : y);
    sx = (x * g_zW + g_zW / 2) / FB_W;
    sy = src_row(y, g_zW, g_zH);
    return ((const uint16_t *)g_zBuf)[(size_t)sy * (size_t)g_zW + (size_t)sx];
}

static void zp_store(int i) {
    int dx, dy;
    g_zp[i].wx = g_zp[i].x;
    g_zp[i].wy = g_zp[i].y;
    for (dy = 0; dy < ZP_D; dy++) {
        for (dx = 0; dx < ZP_D; dx++) {
            g_zp[i].v[dy * ZP_D + dx] = zbuf_at(g_zp[i].x + dx - ZP_R, g_zp[i].y + dy - ZP_R);
        }
    }
    g_zp[i].ok = 1;
}

unsigned short gx_hr_zread(const void *zram, int x, int y) {
    int i, old = 0;
    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) {
        return 0;
    }
    if (!g_zHi || g_zBuf == NULL || zram != (const void *)g_zRam) {
        return ((const uint16_t *)zram)[y * FB_W + x];
    }
    for (i = 0; i < ZP_N; i++) {
        const int dx = x - g_zp[i].wx, dy = y - g_zp[i].wy;
        if (g_zp[i].seen && g_zp[i].ok && dx >= -ZP_R && dx <= ZP_R && dy >= -ZP_R && dy <= ZP_R) {
            g_zp[i].seen = g_passNo;
            g_zp[i].x = (int16_t)x; /* (a proxima janela em volta deste) */
            g_zp[i].y = (int16_t)y;
            return g_zp[i].v[(dy + ZP_R) * ZP_D + dx + ZP_R];
        }
        if (g_zp[i].seen < g_zp[old].seen) {
            old = i;
        }
    }
    g_zp[old].ok = 0;
    g_zp[old].x = (int16_t)x;
    g_zp[old].y = (int16_t)y;
    g_zp[old].seen = g_passNo;
    return zbuf_at(x, y);
}

static uint8_t *buf_alloc(void) {
    uint8_t *p = (uint8_t *)malloc((size_t)HR_MAXW * HR_MAXH * 2u + 64u);
    if (p != NULL) {
        memset(p, 0, (size_t)HR_MAXW * HR_MAXH * 2u + 64u);
    }
    return p;
}

static HrSlot *slot_of(const uint8_t *ram) {
    int i;
    if (ram == NULL) {
        return NULL;
    }
    for (i = 0; i < HR_SLOTS; i++) {
        if (g_slot[i].ram == ram) {
            return &g_slot[i];
        }
    }
    return NULL;
}

/* telas do N64 que ja foram para a tela (tambem em 320x240, quando nao ha imagem maior): o fundo da pausa em 320x240
 * (pbg_begin) so vale numa delas */
static const uint8_t *g_fbSeen[4];

static int fb_seen(const uint8_t *fb) {
    int i;
    for (i = 0; i < 4; i++) {
        if (fb != NULL && g_fbSeen[i] == fb) {
            return 1;
        }
    }
    return 0;
}

void gx_hr_screen(const void *fbRaw) {
    const uint8_t *fb = (const uint8_t *)fbRaw;
    HrSlot *s;
    int i, best = 0;
    if (fb != NULL && !fb_seen(fb)) {
        static int k;
        g_fbSeen[k] = fb;
        k = (k + 1) & 3;
    }
    if (fb == NULL || gx_hr_max <= FB_W) {
        return;
    }
    s = slot_of(fb);
    if (s == NULL) {
        for (i = 1; i < HR_SLOTS; i++) {
            if (g_slot[i].ram == NULL || (g_slot[best].ram != NULL && g_slot[i].use < g_slot[best].use)) {
                best = i;
            }
        }
        s = &g_slot[best];
        s->ram = fb;
        s->valid = 0;
        s->dirty = 0;
    }
    s->use = ++g_use;
}

void gx_hr_scissor(void) {
    const float r = gx.rs;
    if (r == 1.0f) {
        gx.scissorX0 = gx.scRaw[0] >> 2;
        gx.scissorY0 = gx.scRaw[1] >> 2;
        gx.scissorX1 = (gx.scRaw[2] + 3) >> 2;
        gx.scissorY1 = (gx.scRaw[3] + 3) >> 2;
        return;
    }
    gx.scissorX0 = (uint32_t)((int)((float)gx.scRaw[0] * r) >> 2);
    gx.scissorY0 = (uint32_t)((int)((float)gx.scRaw[1] * r) >> 2);
    gx.scissorX1 = (uint32_t)(((int)((float)gx.scRaw[2] * r + 0.999f) + 3) >> 2);
    gx.scissorY1 = (uint32_t)(((int)((float)gx.scRaw[3] * r + 0.999f) + 3) >> 2);
    if (gx.scissorX1 > (uint32_t)gx.hrW) {
        gx.scissorX1 = (uint32_t)gx.hrW;
    }
    if (gx.scissorY1 > (uint32_t)gx.hrH) {
        gx.scissorY1 = (uint32_t)gx.hrH;
    }
    if (g_tOn) {
        /* so a regiao da caixa existe no buffer do texto */
        const uint32_t x0 = (uint32_t)g_tx[0] * 2u, y0 = (uint32_t)g_tx[1] * 2u;
        const uint32_t x1 = (uint32_t)g_tx[2] * 2u, y1 = (uint32_t)g_tx[3] * 2u;
        if (gx.scissorX0 < x0) {
            gx.scissorX0 = x0;
        }
        if (gx.scissorY0 < y0) {
            gx.scissorY0 = y0;
        }
        if (gx.scissorX1 > x1) {
            gx.scissorX1 = x1;
        }
        if (gx.scissorY1 > y1) {
            gx.scissorY1 = y1;
        }
        if (gx.scissorX1 < gx.scissorX0) {
            gx.scissorX1 = gx.scissorX0;
        }
        if (gx.scissorY1 < gx.scissorY0) {
            gx.scissorY1 = gx.scissorY0;
        }
    }
}

void gx_hr_viewport(void) {
    const float r = gx.rs;
    gx.vpScale[0] = gx.vpRawS[0] * r;
    gx.vpScale[1] = gx.vpRawS[1] * r;
    gx.vpTrans[0] = gx.vpRawT[0] * r;
    gx.vpTrans[1] = gx.vpRawT[1] * r;
}

void gx_hr_retarget(void) {
    HrSlot *s = NULL;
    int toZ = 0;
    gx.cle = 0;
    gx.cimg.addr = gx.cimgRaw;
    gx.cimg.width = gx.cimgRawW;
    gx.zimg = gx.zimgRaw;
    gx.hrActive = 0;
    gx.rs = 1.0f;
    g_cur = NULL;
    if (gx.cimgRaw != NULL && gx.cimgRawW == FB_W && gx.cimg.siz == 2) {
        s = slot_of(gx.cimgRaw);
        if (s != NULL && s->pass != g_passNo) {
            s->pass = g_passNo;
            s->txtOn = 0; /* desenho novo nesta tela: a caixa de texto do anterior saiu */
        }
        if (s != NULL) {
            if (gx.zimgRaw != NULL) {
                g_zRam = gx.zimgRaw; /* o z que o jogo usa com a tela */
            }
            if (g_passS == 1.0f) {
                s->valid = 0; /* desenho nativo nesta tela: a imagem maior dela ficou velha */
                s->dirty = 0; /* (e nao pode mais ser reduzida por cima do desenho nativo) */
                s = NULL;
            }
        } else if (gx.cimgRaw == g_zRam && g_passS != 1.0f && !g_pbgCopy) {
            toZ = 1; /* o jogo limpa o z desenhando nele como se fosse cor */
        }
    }
    if (s != NULL || toZ) {
        if (s != NULL && g_inBegin && s->buf != NULL) {
            /* comeco do desenho: o alvo ainda e a tela do anterior (o jogo troca logo em seguida) */
        } else if (s != NULL) {
            int j, taken = 0;
            for (j = 0; j < HR_SLOTS; j++) {
                taken |= &g_slot[j] != s && g_slot[j].buf == g_ext && g_ext != NULL;
            }
            if (g_ext != NULL && !taken) {
                s->buf = g_ext; /* (desenho direto na imagem da fila) */
            } else {
                if (s->own == NULL) {
                    s->own = buf_alloc();
                }
                s->buf = s->own;
            }
        }
        if (g_zBuf == NULL) {
            g_zBuf = buf_alloc();
        }
        if ((s != NULL && s->buf == NULL) || g_zBuf == NULL) {
            gx_hr_scissor();
            gx_hr_viewport();
            return; /* sem memoria: fica no nativo */
        }
        gx.hrActive = 1;
        gx.rs = g_passS;
        gx.hrW = g_passW;
        gx.hrH = g_passH;
        gx.cimg.width = (uint16_t)g_passW;
        if (s != NULL) {
            gx.cimg.addr = s->buf;
            gx.cle = gx_hr_le; /* (imagem maior: cor na ordem do processador) */
            if (!g_inBegin) {
                s->dirty = 1; /* (no comeco do desenho o alvo ainda e a tela anterior, com a imagem dela: nao mexe) */
                s->w = g_passW;
                s->h = g_passH;
            }
            g_cur = s;
            if (gx.zimgRaw != NULL && gx.zimgRaw == g_zRam) {
                gx.zimg = g_zBuf;
                g_zDirty = 1;
            } else {
                gx.zimg = NULL; /* z de outro tamanho: sem teste de profundidade (nao acontece no jogo) */
            }
        } else {
            gx.cimg.addr = g_zBuf;
            gx.zimg = NULL;
            g_zDirty = 1;
        }
    }
    if (g_tOn && g_t320 && g_tSlot != NULL && gx.cimgRaw == g_tSlot->ram) {
        /* text_box_320: a caixa e desenhada em 320x240 (ampliada para tbuf no fim) */
        gx.cimg.addr = g_tSlot->t320;
        gx.cle = gx_hr_le;
        gx.cimg.width = 320;
        gx.zimg = NULL;
        gx.hrActive = 1;
        gx.rs = 1.0f;
        gx.hrW = 320;
        gx.hrH = 240;
    } else if (g_tOn && g_tSlot != NULL && gx.cimgRaw == g_tSlot->ram) {
        /* dentro da caixa de texto: desenha no buffer dela, em 640x480 (tambem quando o desenho e em 320) */
        gx.cimg.addr = g_tSlot->tbuf;
        gx.cle = gx_hr_le;
        gx.cimg.width = 640;
        gx.zimg = NULL;
        gx.hrActive = 1;
        gx.rs = 2.0f;
        gx.hrW = 640;
        gx.hrH = 480;
    }
    if (g_prvArm && gx.cimg.siz == 2 && (gx.cimgRawW == PRV_W || (gx.zimgRaw != NULL && gx.zimgRaw == g_prvRam[0]) ||
                                         (gx.zimgRaw != NULL && gx.zimgRaw == g_prvRam[1]))) {
        /* Link do menu: as imagens de 64 de largura (cor e z) viram buffers nossos de 128 x 224 */
        int i, k = -1;
        if (gx.cimgRawW == PRV_W && gx.cimgRaw != NULL) {
            for (i = 0; i < 2; i++) {
                if (g_prvRam[i] == gx.cimgRaw) {
                    k = i;
                }
            }
            if (k < 0) {
                k = g_prvRam[0] == NULL ? 0 : g_prvRam[1] == NULL ? 1 : -1;
                if (k >= 0) {
                    g_prvRam[k] = gx.cimgRaw;
                }
            }
        }
        if (k >= 0) {
            if (g_prvBuf[k] == NULL) {
                g_prvBuf[k] = (uint8_t *)calloc(1, (size_t)PRV_W * 2 * PRV_H * 2 * 2 + 64);
            }
            if (g_prvBuf[k] != NULL) {
                gx.cimg.addr = g_prvBuf[k];
                gx.cle = 0;
                gx.cimg.width = PRV_W * 2;
                gx.hrActive = 1;
                gx.rs = 2.0f;
                gx.hrW = PRV_W * 2;
                gx.hrH = PRV_H * 2;
                g_prvColor = k;
                g_cur = NULL;
                gx.zimg = NULL;
                for (i = 0; i < 2; i++) {
                    if (gx.zimgRaw != NULL && gx.zimgRaw == g_prvRam[i] && i != k && g_prvBuf[i] != NULL) {
                        gx.zimg = g_prvBuf[i];
                    }
                }
            }
        }
    }
    gx_hr_scissor();
    gx_hr_viewport();
}

/* ---- caixa de texto nitida ----
 * O texto das caixas de dialogo fica feio quando a resolucao dinamica cai. O jogo marca (z_message.c,
 * tools/oot-patch.py) o comeco e o fim do desenho das caixas, com a regiao delas. Entre as marcas o desenho
 * vai para um buffer de 640x480 so daquela regiao: o fundo dela e a imagem 3D ampliada (bilinear), e a caixa
 * e o texto sao desenhados em cima, em 640. A tela mostra a imagem 3D (no tamanho que estiver) e por cima
 * essa regiao (video_miyoo.c). */
static void up_region(const uint8_t *src, int w, int h, uint8_t *dst, int X0, int Y0, int X1, int Y1, int sle, int dle) {
    static uint16_t xi[640];
    static uint8_t xw[640];
    static uint8_t ra[3][644], rb[3][644];
    int x, y, last = -1;
    if (w < 2 || h < 2) {
        return;
    }
    for (x = X0; x < X1; x++) {
        int fx = ((x * 2 + 1) * w * 128) / 640 - 128; /* centro do pixel, 8.8 */
        if (fx < 0) {
            fx = 0;
        }
        xi[x] = (uint16_t)(fx >> 8);
        xw[x] = (uint8_t)(fx & 255);
        if (xi[x] >= w - 1) {
            xi[x] = (uint16_t)(w - 2);
            xw[x] = 255;
        }
    }
    for (y = Y0; y < Y1; y++) {
        int fy = ((y * 2 + 1) * h * 128) / 480 - 128, ya, wy;
        uint8_t *d = dst + ((size_t)y * 640 + (size_t)X0) * 2u;
        if (fy < 0) {
            fy = 0;
        }
        ya = fy >> 8;
        wy = fy & 255;
        if (ya >= h - 1) {
            ya = h - 2;
            wy = 255;
        }
        if (ya != last) {
            int k, xa = xi[X0], xb = xi[X1 - 1] + 1;
            for (k = 0; k < 2; k++) {
                uint8_t(*R)[644] = k ? rb : ra;
                const uint8_t *sr = src + (size_t)(ya + k) * (size_t)w * 2u;
                for (x = xa; x <= xb; x++) {
                    const unsigned c = sle ? ((unsigned)sr[2 * x + 1] << 8) | sr[2 * x] : ((unsigned)sr[2 * x] << 8) | sr[2 * x + 1];
                    R[0][x] = (uint8_t)((c >> 11) & 31);
                    R[1][x] = (uint8_t)((c >> 6) & 31);
                    R[2][x] = (uint8_t)((c >> 1) & 31);
                }
            }
            last = ya;
        }
        for (x = X0; x < X1; x++) {
            const int i = xi[x], a = xw[x];
            unsigned c[3], o;
            int k;
            for (k = 0; k < 3; k++) {
                const int t = ra[k][i] * (256 - a) + ra[k][i + 1] * a;
                const int b = rb[k][i] * (256 - a) + rb[k][i + 1] * a;
                c[k] = (unsigned)((t * (256 - wy) + b * wy + 32768) >> 16);
            }
            o = (c[0] << 11) | (c[1] << 6) | (c[2] << 1) | 1u;
            if (dle) {
                *d++ = (uint8_t)o;
                *d++ = (uint8_t)(o >> 8);
            } else {
                *d++ = (uint8_t)(o >> 8);
                *d++ = (uint8_t)o;
            }
        }
    }
}

/* marca do jogo: begin = 1 com a regiao (pixels de 320x240), 0 = fim da caixa */
void gx_hr_text(int begin, int x0, int y0, int x1, int y1) {
    HrSlot *s = g_cur;
    int native = 0;
    if (!begin) {
        if (g_tOn) {
            gx_batch_flush();
            if (g_t320 && g_tSlot != NULL && g_tSlot->t320 != NULL) {
                /* a caixa em 320x240 ampliada (com filtro) para a regiao de tbuf que vai para a tela */
                up_region(g_tSlot->t320, 320, 240, g_tSlot->tbuf, g_tx[0] * 2, g_tx[1] * 2, g_tx[2] * 2, g_tx[3] * 2,
                          gx_hr_le, gx_hr_le);
            }
            g_t320 = 0;
            g_tOn = 0;
            gx_hr_retarget(); /* volta para a imagem 3D */
        }
        return;
    }
    {
        /* o fundo da caixa: a imagem maior deste desenho, ou a tela do N64 quando o desenho e em 320 */
        HrSlot *t = slot_of(gx.cimgRaw);
        if (g_tOn || !gx_hr_texto || gx_hr_pan560 || gx_hr_max <= FB_W || gx_capturing || t == NULL || gx.cimg.siz != 2 ||
            gx.cimgRawW != FB_W) { /* (caixa nitida: so na tela de 640x480, ela e montada em 640) */
            g_tStat[3]++;
            return;
        }
        if (gx_texto_320) {
            /* text_box_320: a caixa sai em 320x240 como o resto da interface */
            if (!(s == t && gx.hrActive && s->buf != NULL)) {
                g_t320Stat[1]++;
                return; /* desenho ja em 320: a caixa sai nele mesmo */
            }
        } else if (s == t && s->buf != NULL && gx.hrActive && s->w >= 640) {
            g_tStat[2]++;
            return; /* a imagem ja esta em 640: o texto sai nitido normal */
        }
        if (!(s == t && gx.hrActive && s->buf != NULL)) {
            s = t; /* desenho em 320 nesta tela: o fundo vem da RAM */
            native = 1;
        }
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > FB_W) {
        x1 = FB_W;
    }
    if (y1 > FB_H) {
        y1 = FB_H;
    }
    if (x1 - x0 < 4 || y1 - y0 < 4) {
        return;
    }
    if (s->tbuf == NULL) {
        s->tbuf = (uint8_t *)malloc(640u * 480u * 2u + 64u);
        if (s->tbuf == NULL) {
            return;
        }
    }
    gx_batch_flush();
    g_tx[0] = x0;
    g_tx[1] = y0;
    g_tx[2] = x1;
    g_tx[3] = y1;
    if (gx_texto_320 && !native) {
        /* o fundo da regiao reduzido para 320x240 (ponto mais proximo; a caixa cobre quase tudo), a caixa e
         * desenhada em cima e ampliada no fim */
        int x, y;
        if (s->t320 == NULL) {
            s->t320 = (uint8_t *)malloc(320u * 240u * 2u + 64u);
            if (s->t320 == NULL) {
                return;
            }
        }
        /* (1 pixel a mais em volta: a ampliacao com filtro le os vizinhos da borda; sem isso aparecia uma linha) */
        for (y = (y0 > 0 ? y0 - 1 : 0); y < (y1 < FB_H ? y1 + 1 : FB_H); y++) {
            const uint16_t *src = (const uint16_t *)s->buf + (size_t)(y * s->h / 240) * (size_t)s->w;
            uint16_t *dst = (uint16_t *)s->t320 + (size_t)y * 320u;
            for (x = (x0 > 0 ? x0 - 1 : 0); x < (x1 < FB_W ? x1 + 1 : FB_W); x++) {
                dst[x] = src[x * s->w / 320];
            }
        }
        s->txt[0] = x0 * 2;
        s->txt[1] = y0 * 2;
        s->txt[2] = x1 * 2;
        s->txt[3] = y1 * 2;
        s->txtOn = 1;
        g_t320Stat[0]++;
        g_tSlot = s;
        g_t320 = 1;
        g_tOn = 1;
        gx_hr_retarget();
        return;
    }
    if (!native) {
        up_region(s->buf, s->w, s->h, s->tbuf, x0 * 2, y0 * 2, x1 * 2, y1 * 2, gx_hr_le, gx_hr_le);
    } else {
        up_region(s->ram, FB_W, FB_H, s->tbuf, x0 * 2, y0 * 2, x1 * 2, y1 * 2, 0, gx_hr_le);
    }
    s->txt[0] = x0 * 2;
    s->txt[1] = y0 * 2;
    s->txt[2] = x1 * 2;
    s->txt[3] = y1 * 2;
    s->txtOn = 1;
    g_tStat[native]++;
    g_tSlot = s;
    g_tOn = 1;
    gx_hr_retarget();
}

int gx_hr_text_of(const void *fbRaw, const uint8_t **buf, int rect[4]) {
    HrSlot *s = slot_of((const uint8_t *)fbRaw);
    if (s == NULL || !s->txtOn || s->tbuf == NULL) {
        return 0;
    }
    *buf = s->tbuf;
    rect[0] = s->txt[0];
    rect[1] = s->txt[1];
    rect[2] = s->txt[2];
    rect[3] = s->txt[3];
    return 1;
}

/* reduz a imagem maior para a tela de 320x240 do N64 (vizinho mais proximo; 2 bytes por pixel) */
/* reducoes da imagem maior para a tela do N64 (opcao reducao_sempre: 1 = como antes, todas no fim de cada desenho).
 * Ninguem precisa da cor reduzida no fim de todo desenho: o que le a tela do N64 (fundo da pausa, transicoes, textura
 * tirada da tela) passa por gx_hr_reading, que reduz na hora. O z o jogo le pela CPU (sol, halos) uma vez por quadro do
 * jogo: so o do desenho do quadro de verdade (os intermediarios, de camera deslocada, nao). */
int gx_hr_reducao_sempre = 0;
/* opcao cor_direta: a imagem maior (e a caixa de texto em 640) gravada com a cor na ordem do processador, que o chip 2D
 * da tela le direto (RGBA5551) - sem a conversao de cada imagem na CPU. A tela do N64 (RAM) continua na ordem do N64:
 * as reducoes trocam os bytes. So no aparelho (no PC as imagens vao para PNG pelo caminho antigo) */
#ifdef TARGET_MIYOO
int gx_hr_le = 1;
#else
int gx_hr_le = 0;
#endif
int gx_hr_interm; /* (gfx_hook.c) o desenho atual e intermediario */
static uint64_t g_redNs;
static unsigned g_redN, g_redLazy;
uint64_t gx_hr_copyNs; /* (gfx_hook.c) copia da imagem maior para a fila da tela */
unsigned gx_hr_copyN;

static uint64_t red_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reduce(const uint8_t *src, int w, int h, uint8_t *dst, int le) {
    int x, y;
    const uint64_t t0 = red_now();
    uint16_t xo[FB_W];
    for (x = 0; x < FB_W; x++) {
        xo[x] = (uint16_t)((x * w + w / 2) / FB_W);
    }
    for (y = 0; y < FB_H; y++) {
        const uint16_t *s = (const uint16_t *)(src + (size_t)src_row(y, w, h) * (size_t)w * 2u);
        uint16_t *d = (uint16_t *)(dst + (size_t)y * FB_W * 2u);
        if (w == 2 * FB_W) {
            x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            for (; x + 8 <= FB_W; x += 8) {
                const uint16x8_t v = vld2q_u16(s + 2 * x).val[1]; /* (os impares de 16) */
                vst1q_u16(d + x, le ? vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(v))) : v);
            }
#endif
            for (; x < FB_W; x++) {
                d[x] = le ? __builtin_bswap16(s[2 * x + 1]) : s[2 * x + 1];
            }
        } else if (le) {
            for (x = 0; x < FB_W; x++) {
                d[x] = __builtin_bswap16(s[xo[x]]);
            }
        } else {
            for (x = 0; x < FB_W; x++) {
                d[x] = s[xo[x]];
            }
        }
    }
    g_redNs += red_now() - t0;
    g_redN++;
}

static void slot_reduce(HrSlot *s) {
    if (s->buf != NULL && s->ram != NULL) {
        reduce(s->buf, s->w, s->h, (uint8_t *)(uintptr_t)s->ram, gx_hr_le);
    }
}

void gx_hr_reading(const void *p) {
    const uint8_t *a = (const uint8_t *)p;
    const size_t n = (size_t)FB_W * FB_H * 2u;
    int i;
    if (a == NULL) {
        return;
    }
    for (i = 0; i < HR_SLOTS; i++) {
        HrSlot *s = &g_slot[i];
        if (s->dirty && s->ram != NULL && a >= s->ram && a < s->ram + n) {
            gx_batch_flush();
            g_redLazy++;
            slot_reduce(s);
            s->dirty = s == g_cur; /* se ainda e o alvo, continua sendo desenhada */
        }
    }
    if (g_zDirty && g_zRam != NULL && a >= g_zRam && a < g_zRam + n) {
        gx_batch_flush();
        reduce(g_zBuf, g_passW, g_passH, (uint8_t *)(uintptr_t)g_zRam, 0);
        g_zDirty = gx.hrActive && (gx.zimg == g_zBuf || gx.cimg.addr == g_zBuf);
    }
}

int gx_menu_320 = 1;
int gx_arquivos_320 = 1; /* opcao arquivos_320: a tela de arquivos (menu azul do novo jogo) em 320x240 */
static int g_menuSeen, g_menuLast, g_menuForced, g_lvSaved;

void gx_hr_menu_mark(int open) {
    if (open) { /* (quem chama ja olhou a opcao: menu_320 ou arquivos_320) */
        g_menuSeen = 1;
    }
}

/* a resolucao e a caixa de texto em uso, a partir das opcoes (o filtro retro prende as duas em 320x240) */
void gx_hr_sync_opts(void) {
    extern int gx_texto_320, gx_texto_320_opt; /* gfx_ge.c */
    if (gx_hr_pan560) {
        /* tela de 752x560: a opcao vale proporcional (640 = 752, 480 = 564...); o filtro retro fica em 376x280 */
        gx_hr_max = gx_retro ? RETRO560_W : gx_hr_max_opt <= FB_W ? FB_W : gx_hr_max_opt * 752 / 640; /* (proporcional) */
    } else {
        gx_hr_max = gx_retro ? FB_W : gx_hr_max_opt;
    }
    gx_texto_320 = gx_retro ? 1 : gx_texto_320_opt;
}

void gx_hr_pass_begin(void) {
    int lvMax = 0, w;
    gx_hr_sync_opts();
    gx_solid_alpha = 0;
    g_prvLive = 0; /* (Link do menu em 2x: so no desenho com a marca 0x39) */
    gx_ge_pass_begin(); /* (2D pelo acelerador: gfx_ge.c) */
    while (lvMax < HR_LV - 1 && k_lvW[lvMax] > gx_hr_max) {
        lvMax++;
    }
    { static int e = -1; if (e < 0) { e = getenv("OOT_RES_FIXA") != NULL; } if (e) { gx_hr_fixo = 1; } }
    if (gx_hr_max <= FB_W || gx_capturing) {
        g_lv = HR_LV - 1;
    } else if (g_lv < lvMax || gx_hr_fixo || (gx_hr_pan560 && gx_retro)) {
        g_lv = lvMax; /* (filtro retro na tela de 752x560: sempre 376x280, as linhas casam com as do filtro) */
    }
    /* menu de pausa: enquanto o fundo guardado e desenhado, o tamanho fica o dele (copia exata, sem ampliar) */
    g_pbgUsedLast = g_pbgUsed;
    g_pbgUsed = 0;
    g_pbgSkip = 0;
    g_pbgCopy = 0;
    g_prvArm = 0;
    if (g_pbgUsedLast && g_pbgValid && gx_hr_max > FB_W && !gx_capturing && g_pbgLv >= lvMax && g_pbgLv < HR_LV) {
        g_lv = g_pbgLv;
    }
    /* menu de pausa em 320x240 (opcao menu_320): o desenho todo do menu (paginas em tela cheia, com mistura) e bem mais
     * leve na resolucao original. Vale a partir do desenho seguinte ao que mostrou o menu; o controle de resolucao nao
     * conta esses desenhos (gx_hr_pass_end) e o degrau volta ao que era quando o menu fecha. */
    g_menuForced = 0;
    if (g_menuLast && gx_hr_max > FB_W && !gx_capturing && g_lv != HR_LV - 1 && !(gx_hr_pan560 && gx_retro)) { /* (menu_320 / arquivos_320: em gx_hr_menu_mark) */
        g_lvSaved = g_lv;
        g_lv = HR_LV - 1;
        g_menuForced = 1;
        g_pbgUsedLast = 0;
    }
    w = k_lvW[g_lv];
    g_passW = w;
    g_passH = pass_h(w);
    g_passS = (float)w / (float)FB_W;
    g_lvHist[g_lv]++;
    g_passNo++;
    g_tOn = 0;
    g_inBegin = 1;
    gx_hr_retarget(); /* o alvo que ficou do desenho anterior, no tamanho novo */
    g_inBegin = 0;
}

void gx_hr_pass_end(double ms) {
    int i, lvMax = 0;
    if (g_tOn) {
        gx_hr_text(0, 0, 0, 0, 0); /* (o jogo nao marcou o fim) */
    }
    for (i = 0; i < HR_SLOTS; i++) {
        HrSlot *s = &g_slot[i];
        if (s->dirty) {
            if (gx_hr_reducao_sempre) {
                slot_reduce(s);
                s->dirty = 0;
            } /* (senao fica marcada: gx_hr_reading reduz se alguem ler a tela do N64) */
            s->valid = 1;
        }
    }
    if (g_zDirty && g_zRam != NULL && g_zBuf != NULL) {
        if (!gx_hr_interm || gx_hr_reducao_sempre) {
            if (gx_hr_zpontos && !gx_hr_reducao_sempre) {
                /* so os pontos que o jogo le (sol, halos) */
                g_zW = g_passW;
                g_zH = g_passH;
                for (i = 0; i < ZP_N; i++) {
                    if (g_zp[i].seen && g_passNo - g_zp[i].seen > 12u) {
                        g_zp[i].seen = 0; /* (nao e lido ha um tempo) */
                        g_zp[i].ok = 0;
                    } else if (g_zp[i].seen) {
                        zp_store(i);
                    }
                }
                g_zHi = 1;
            } else {
                reduce(g_zBuf, g_passW, g_passH, (uint8_t *)(uintptr_t)g_zRam, 0); /* o jogo le o z (o sol atras de algo) */
                g_zHi = 0;
            }
        }
        g_zDirty = 0; /* (intermediario: a RAM fica com o z do ultimo quadro de verdade) */
    } else if (!gx_hr_interm && !g_zDirty) {
        g_zHi = 0; /* (quadro desenhado em 320: o z esta na RAM) */
    }
    {
        const int forced = g_menuForced;
        g_menuForced = 0;
        g_menuLast = g_menuSeen;
        g_menuSeen = 0;
        if (forced) {
            g_lv = g_lvSaved; /* o menu nao conta para o controle de resolucao */
            return;
        }
    }
    if (gx_hr_max <= FB_W || (gx_hr_pan560 && gx_retro)) {
        return; /* (sem resolucao dinamica) */
    }
    {
        extern int port_res_hold __attribute__((weak)); /* input_miyoo.c: print de tela do OnionOS em andamento */
        if (&port_res_hold != NULL && port_res_hold) {
            return; /* (resolucao parada: o print sai na resolucao de antes) */
        }
    }
    while (lvMax < HR_LV - 1 && k_lvW[lvMax] > gx_hr_max) {
        lvMax++;
    }
    if (gx_hr_ctl2 == 2) {
        /* controle 2 (opcao controle_resolucao 2): o tempo do desenho como parte fixa (vertices, comandos: nao muda com a
         * resolucao) + custo por pixel; o custo por pixel e a media movel, e cada degrau e previsto por ele. Sobe para o
         * degrau mais alto previsto ate 0,92 do alvo depois de 10 desenhos seguidos com folga (o 1 pedia 20 e 0,90, e
         * contava o desenho inteiro como pixels: previa errado para cima e ficava abaixo do que cabia); desce como o 1 */
        static double cpx, lastMs;
        const double F = gx_hr_fixo_ms;
        const double px = (double)k_lvW[g_lv] * k_lvW[g_lv] * 0.75;
        double c, best;
        int lv, spike2;
        if (ms > gx_hr_alvo * 2.2) {
            return;
        }
        c = (ms - F) / px;
        if (c < 0.0) {
            c = 0.0;
        }
        cpx = cpx == 0.0 ? c : cpx * 0.7 + c * 0.3;
        if (g_hold > 0) {
            g_hold--;
        }
        spike2 = ms > gx_hr_alvo * 1.3 && lastMs > gx_hr_alvo * 1.3;
        lastMs = ms;
        g_ema = F + cpx * px;
        if ((g_ema > gx_hr_alvo || spike2) && g_lv < HR_LV - 1 && g_hold == 0) {
            const double cb = spike2 && ms > g_ema ? ((ms + lastMs) * 0.5 - F) / px : cpx;
            lv = g_lv;
            while (lv < HR_LV - 1 && F + cb * k_lvW[lv] * k_lvW[lv] * 0.75 > gx_hr_alvo * 0.92) {
                lv++;
            }
            if (lv == g_lv) {
                lv++;
            }
            g_lv = lv;
            cpx = cb;
            g_hold = 4;
            g_up = 0;
            g_lvDown++;
            return;
        }
        lv = g_lv;
        while (lv > lvMax && F + cpx * k_lvW[lv - 1] * k_lvW[lv - 1] * 0.75 < gx_hr_alvo * 0.92) {
            lv--;
        }
        best = F + cpx * k_lvW[lv] * k_lvW[lv] * 0.75;
        (void)best;
        if (lv < g_lv && g_hold == 0) {
            if (++g_up >= 10) {
                g_lv = lv;
                g_hold = 10;
                g_up = 0;
                g_lvUp++;
            }
        } else {
            g_up = 0;
        }
        return;
    }
    /* tempo do desenho: media movel. Com degraus pequenos, cada troca aparece (a nitidez muda), entao o
     * controle troca pouco e de uma vez:
     *  - um desenho muito acima do alvo (mais que o dobro: carregando cena, texturas novas...) nao conta;
     *  - desce quando a media passa do alvo, ou com 2 desenhos seguidos bem acima dele, ja para o degrau em
     *    que a media cabe;
     *  - sobe so depois de 20 desenhos seguidos com folga, ja para o degrau mais alto que cabe com folga (0,90 do alvo). */
    if (ms > gx_hr_alvo * 2.2) {
        return;
    }
    g_ema = g_ema == 0.0 ? ms : g_ema * 0.7 + ms * 0.3;
    if (g_hold > 0) {
        g_hold--;
    }
    {
        static double lastMs;
        const int spike2 = ms > gx_hr_alvo * 1.3 && lastMs > gx_hr_alvo * 1.3;
        lastMs = ms;
        if ((g_ema > gx_hr_alvo || spike2) && g_lv < HR_LV - 1 && g_hold == 0) {
            const double base = g_ema > ms ? g_ema : (g_ema + ms) * 0.5;
            const int lv0 = g_lv;
            do {
                g_lv++;
            } while (g_lv < HR_LV - 1 &&
                     base * ((double)k_lvW[g_lv] * k_lvW[g_lv]) / ((double)k_lvW[lv0] * k_lvW[lv0]) > gx_hr_alvo * 0.92);
            {
                const double k = (double)k_lvW[g_lv] / (double)k_lvW[lv0];
                g_ema = base * k * k;
            }
            g_hold = 4;
            g_up = 0;
            g_lvDown++;
            return;
        }
    }
    if (g_lv > lvMax && g_hold == 0) {
        const double k = (double)k_lvW[g_lv - 1] / (double)k_lvW[g_lv];
        if (g_ema * k * k < gx_hr_alvo * 0.90) {
            if (++g_up >= 20) {
                const int lv0 = g_lv;
                double kk;
                while (g_lv > lvMax &&
                       g_ema * ((double)k_lvW[g_lv - 1] * k_lvW[g_lv - 1]) / ((double)k_lvW[lv0] * k_lvW[lv0]) <
                           gx_hr_alvo * 0.90) {
                    g_lv--;
                }
                kk = (double)k_lvW[g_lv] / (double)k_lvW[lv0];
                g_ema *= kk * kk;
                g_hold = 10;
                g_up = 0;
                g_lvUp++;
            }
        } else {
            g_up = 0;
        }
    }
}

int gx_hr_image(const void *fbRaw, const uint16_t **img, int *w, int *h) {
    HrSlot *s = slot_of((const uint8_t *)fbRaw);
    if (s == NULL || !s->valid || s->buf == NULL || s->w <= FB_W) {
        return 0;
    }
    *img = (const uint16_t *)s->buf;
    *w = s->w;
    *h = s->h;
    return 1;
}

void gx_hr_print_stats(void) {
    int i;
    unsigned tot = 0;
    if (gx_hr_max <= FB_W) {
        return;
    }
    for (i = 0; i < HR_LV; i++) {
        tot += g_lvHist[i];
    }
    if (tot == 0) {
        return;
    }
    fprintf(stderr, "[resolucao] desenhos por tamanho:");
    for (i = 0; i < HR_LV; i++) {
        fprintf(stderr, " %d %u%%", k_lvW[i], (g_lvHist[i] * 100u + tot / 2) / tot);
    }
    fprintf(stderr, " | desceu %u, subiu %u (alvo %d ms) | reducoes %u (%.2f ms cada, %u pedidas na hora), copias p/ fila %u (%.2f ms cada)\n",
            g_lvDown, g_lvUp, gx_hr_alvo, g_redN, g_redN ? (double)g_redNs / 1e6 / g_redN : 0.0, g_redLazy, gx_hr_copyN,
            gx_hr_copyN ? (double)gx_hr_copyNs / 1e6 / gx_hr_copyN : 0.0);
    g_redNs = gx_hr_copyNs = 0;
    g_redN = g_redLazy = gx_hr_copyN = 0;
    gx_ge_print_stats();
    {
        extern void gx_premix_print(void);
        gx_premix_print();
    }
    if (g_tStat[0] + g_tStat[1] + g_tStat[2] + g_tStat[3]) {
        fprintf(stderr, "[resolucao] caixas de texto: em 640 sobre imagem maior %u, sobre 320 %u, imagem ja em 640 %u, recusadas %u\n",
                g_tStat[0], g_tStat[1], g_tStat[2], g_tStat[3]);
        memset(g_tStat, 0, sizeof g_tStat);
    }
    if (g_t320Stat[0] + g_t320Stat[1]) {
        fprintf(stderr, "[resolucao] caixas de texto pelos nucleos em 320: sobre imagem maior %u, desenho ja em 320 %u\n",
                g_t320Stat[0], g_t320Stat[1]);
        memset(g_t320Stat, 0, sizeof g_t320Stat);
    }
    if (g_pbgStat[0] + g_pbgStat[1] + g_pbgStat[2] + g_prvStat[0] + g_prvStat[1] + g_prvStat[2] + g_prvStat[3] + g_prvStat[4]) {
        fprintf(stderr, "[resolucao] fundo da pausa: guardado %u, desenhado %u, sem imagem maior %u | Link do menu em 2x: "
                        "desenhos %u, texturas trocadas %u, cargas recusadas %u, marcas da textura %u (sem o Link em 2x %u), "
                        "textura em %08x, cargas de 64 em outro endereco %u (ultima %08x)\n",
                g_pbgStat[0], g_pbgStat[1], g_pbgStat[2], g_prvStat[0], g_prvStat[1], g_prvStat[2], g_prvStat[3],
                g_prvStat[4], g_prvTex, g_prvStat[5], g_prvLastOff);
        memset(g_pbgStat, 0, sizeof g_pbgStat);
        memset(g_prvStat, 0, sizeof g_prvStat);
    }
    memset(g_lvHist, 0, sizeof g_lvHist);
    g_lvDown = g_lvUp = 0;
}

/* ---- fundo do menu de pausa ----
 * O jogo guarda a tela (em 320x240) no quadro em que a pausa comeca, passa nela um filtro de bordas do N64
 * na CPU e, enquanto o menu esta aberto, a redesenha ampliada (retangulos de copia): ficava quadriculada.
 * Marcas (z_play.c): 0x34 = guarde a imagem maior desta tela agora; 0x35/0x36 = em volta da copia de volta:
 * no lugar dela vai a imagem maior guardada (os retangulos entre as marcas sao ignorados). */
static void pbg_blit(HrSlot *s) {
    const int W = g_pbgW, H = g_pbgH, w = s->w, h = s->h;
    int x, y;
    if (W == w && H == h) {
        memcpy(s->buf, g_pbg, (size_t)w * h * 2u);
        return;
    }
    for (y = 0; y < h; y++) { /* (tamanho diferente so no 1o quadro: vizinho mais proximo) */
        const uint16_t *sr = (const uint16_t *)g_pbg + (size_t)((y * H) / h) * W;
        uint16_t *d = (uint16_t *)s->buf + (size_t)y * w;
        for (x = 0; x < w; x++) {
            d[x] = sr[(x * W) / w];
        }
    }
}

/* a tela do N64 recebe a imagem maior reduzida com media (cada pixel de 320x240 = media dos pixels que ele cobre),
 * sem o serrilhado do vizinho mais proximo: e dela que o jogo tira o fundo do menu de pausa (em 320x240) */
static void reduce_box(const uint8_t *src, int w, int h, uint8_t *dst, int le) {
    int x, y;
    const int hc = w * 3 / 4; /* (altura da imagem inteira; a de 752x560 perde as ultimas linhas) */
    for (y = 0; y < FB_H; y++) {
        int y0 = y * hc / FB_H, y1 = (y + 1) * hc / FB_H > y0 ? (y + 1) * hc / FB_H : y0 + 1;
        if (y1 > h) {
            y1 = h;
        }
        if (y0 >= y1) {
            y0 = y1 - 1;
        }
        uint16_t *d = (uint16_t *)(dst + (size_t)y * FB_W * 2u);
        for (x = 0; x < FB_W; x++) {
            const int x0 = x * w / FB_W, x1 = (x + 1) * w / FB_W > x0 ? (x + 1) * w / FB_W : x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0, o;
            int yy, xx;
            for (yy = y0; yy < y1; yy++) {
                const uint16_t *sr = (const uint16_t *)(src + (size_t)yy * (size_t)w * 2u);
                for (xx = x0; xx < x1; xx++) {
                    const unsigned c = le ? sr[xx] : __builtin_bswap16(sr[xx]);
                    r += (c >> 11) & 31;
                    g += (c >> 6) & 31;
                    b += (c >> 1) & 31;
                    a |= c & 1;
                    n++;
                }
            }
            o = ((r + n / 2) / n << 11) | ((g + n / 2) / n << 6) | ((b + n / 2) / n << 1) | a;
            d[x] = __builtin_bswap16((uint16_t)o); /* (RAM do N64: big-endian) */
        }
    }
}

static void pbg_save(void) {
    HrSlot *s = g_cur;
    g_pbgCopy = 1;
    g_zDirty = 0; /* (o z maior deste quadro nao e mais copiado para a RAM: o fundo do menu vai para la) */
    if (s != NULL && s->buf != NULL && s->ram != NULL && gx.cimg.addr == s->buf && !gx_capturing) {
        gx_batch_flush();
        reduce_box(s->buf, s->w, s->h, (uint8_t *)(uintptr_t)s->ram, gx_hr_le);
        s->dirty = 0; /* (a copia do jogo le esta, sem reduzir de novo pelo vizinho mais proximo) */
    }
    g_pbgValid = 0;
    if (s == NULL || s->buf == NULL || gx.cimg.addr != s->buf || gx_capturing) {
        /* sem imagem maior (o desenho deste quadro saiu em 320x240, no degrau mais baixo da resolucao): guarda a tela do
         * N64 mesmo assim. O jogo guarda a copia dele no z-buffer da RAM, e o que foi desenhado com profundidade depois
         * disso escrevia por cima dela (o fundo do menu saia com listras coloridas embaixo e dos lados). */
        static int warned;
        const uint8_t *fb = gx.cimgRaw;
        if (!gx_capturing && fb != NULL && gx.cimgRawW == FB_W && gx.cimg.siz == 2 && !gx.hrActive &&
            gx.cimg.addr == fb) {
            if (g_pbg == NULL) {
                g_pbg = (uint8_t *)malloc((size_t)HR_MAXW * HR_MAXH * 2u);
            }
            if (g_pbg != NULL) {
                const uint16_t *src = (const uint16_t *)fb;
                uint16_t *dst = (uint16_t *)g_pbg;
                int i;
                gx_batch_flush();
                for (i = 0; i < FB_W * FB_H; i++) {
                    dst[i] = gx_hr_le ? __builtin_bswap16(src[i]) : src[i]; /* (RAM do N64: big-endian) */
                }
                g_pbgW = FB_W;
                g_pbgH = FB_H;
                g_pbgLv = g_lv;
                g_pbgValid = 1;
                g_pbgStat[0]++;
                return;
            }
        }
        if (warned < 8) {
            warned++;
            fprintf(stderr, "[resolucao] fundo da pausa sem imagem: atual=%p buf=%p cor=%p tela=%p larg=%d maior=%d\n",
                    (void *)s, s != NULL ? (void *)s->buf : NULL, (void *)gx.cimg.addr, (const void *)gx.cimgRaw,
                    gx.cimgRawW, gx.hrActive);
        }
        g_pbgStat[2]++;
        return;
    }
    if (g_pbg == NULL) {
        g_pbg = (uint8_t *)malloc((size_t)HR_MAXW * HR_MAXH * 2u);
        if (g_pbg == NULL) {
            return;
        }
    }
    gx_batch_flush();
    memcpy(g_pbg, s->buf, (size_t)s->w * s->h * 2u);
    g_pbgW = s->w;
    g_pbgH = s->h;
    g_pbgLv = g_lv;
    g_pbgValid = 1;
    g_pbgStat[0]++;
}

static void pbg_begin(void) {
    HrSlot *s = g_cur;
    g_pbgSkip = 0;
    if (g_pbgValid && g_pbg != NULL && !gx.hrActive && gx.cimgRaw != NULL && gx.cimgRawW == FB_W && gx.cimg.siz == 2 &&
        gx.cimg.addr == gx.cimgRaw && (slot_of(gx.cimgRaw) != NULL || fb_seen(gx.cimgRaw))) {
        /* menu de pausa desenhado em 320x240 (pause_menu_320): o fundo vem da imagem maior guardada, reduzida com media,
         * direto na tela do N64 (a copia que o jogo guardou no z-buffer da RAM nao e usada: la ficava o z).
         * fb_seen: tambem quando o jogo inteiro roda em 320x240 (filtro retro, ou resolucao 320 desde a abertura), sem
         * imagem maior nem vagas: antes, o fundo ia pela copia do jogo, como textura, e levava o brilho (gamma) de novo
         * (com o filtro ligado ao abrir, +4 de brilho: a pausa clareava; 11/2026) */
        gx_batch_flush();
        reduce_box(g_pbg, g_pbgW, g_pbgH, (uint8_t *)gx.cimg.addr, gx_hr_le);
        g_pbgSkip = 1;
        g_pbgStat[1]++;
        return;
    }
    if (!g_pbgValid || s == NULL || s->buf == NULL || gx.cimg.addr != s->buf || gx_capturing) {
        return;
    }
    gx_batch_flush();
    pbg_blit(s);
    g_pbgSkip = 1;
    g_pbgUsed = 1;
    g_pbgStat[1]++;
}

int gx_hr_skip_rect(void) {
    return g_pbgSkip;
}

/* estado rapido (port/savestate.c): a imagem maior do fundo da pausa vai junto no estado (o jogo so guarda a de
 * 320x240, no gZBuffer). Chamados com o desenho parado (Port_GfxIdle). Sem imagem valida: devolve 0. */
int gx_hr_pbg_get(const void **px, int *w, int *h, int *lv) {
    if (!g_pbgValid || g_pbg == NULL) {
        return 0;
    }
    *px = g_pbg;
    *w = g_pbgW;
    *h = g_pbgH;
    *lv = g_pbgLv;
    return 1;
}

void gx_hr_pbg_set(const void *px, int w, int h, int lv) {
    g_pbgValid = 0;
    g_pbgSkip = 0;
    if (px == NULL || w <= 0 || h <= 0 || w > HR_MAXW || h > HR_MAXH) {
        return; /* (estado salvo sem a imagem: o menu usa a de 320x240 do jogo) */
    }
    if (g_pbg == NULL) {
        g_pbg = (uint8_t *)malloc((size_t)HR_MAXW * HR_MAXH * 2u);
        if (g_pbg == NULL) {
            return;
        }
    }
    memcpy(g_pbg, px, (size_t)w * h * 2u);
    g_pbgW = w;
    g_pbgH = h;
    g_pbgLv = lv;
    g_pbgValid = 1;
}

/* ---- Link da tela de equipamentos em 2x ----
 * 0x37/0x38 (z_player_lib.c): em volta do desenho do Link do menu. As imagens de 64 de largura que ele usa
 * (cor e z) viram buffers nossos de 128 x 224 (gx_hr_retarget); no fim, a cor e reduzida (media de 2x2)
 * para a imagem do jogo, que segue o caminho de sempre.
 * 0x39 (z_kaleido_scope.c, w0 = endereco): o jogo copia agora a imagem para a textura do menu; o Link em 2x
 * vira a "textura nova" desse endereco (gx_hr_prv_lookup, chamado pela cache de texturas). */
static void prv_end(void) {
    const int k = g_prvColor;
    uint8_t *ram;
    int x, y;
    if (!g_prvArm) {
        return;
    }
    g_prvArm = 0;
    if (k >= 0 && g_prvBuf[k] != NULL && g_prvRam[k] != NULL) {
        gx_batch_flush();
        ram = (uint8_t *)(uintptr_t)g_prvRam[k];
        for (y = 0; y < PRV_H; y++) {
            const uint8_t *a = g_prvBuf[k] + (size_t)(2 * y) * PRV_W * 4u, *b = a + PRV_W * 4u;
            uint8_t *d = ram + (size_t)y * PRV_W * 2u;
            for (x = 0; x < PRV_W; x++) {
                const unsigned c0 = ((unsigned)a[4 * x] << 8) | a[4 * x + 1], c1 = ((unsigned)a[4 * x + 2] << 8) | a[4 * x + 3];
                const unsigned c2 = ((unsigned)b[4 * x] << 8) | b[4 * x + 1], c3 = ((unsigned)b[4 * x + 2] << 8) | b[4 * x + 3];
                const unsigned r = (((c0 >> 11) & 31) + ((c1 >> 11) & 31) + ((c2 >> 11) & 31) + ((c3 >> 11) & 31) + 2) >> 2;
                const unsigned g = (((c0 >> 6) & 31) + ((c1 >> 6) & 31) + ((c2 >> 6) & 31) + ((c3 >> 6) & 31) + 2) >> 2;
                const unsigned bl = (((c0 >> 1) & 31) + ((c1 >> 1) & 31) + ((c2 >> 1) & 31) + ((c3 >> 1) & 31) + 2) >> 2;
                const unsigned o = (r << 11) | (g << 6) | (bl << 1) | ((c0 | c1 | c2 | c3) & 1u);
                d[2 * x] = (uint8_t)(o >> 8);
                d[2 * x + 1] = (uint8_t)o;
            }
        }
        if (g_prvImg == NULL) {
            g_prvImg = (uint8_t *)malloc((size_t)PRV_W * 2 * PRV_H * 2 * 2);
        }
        if (g_prvImg != NULL) {
            memcpy(g_prvImg, g_prvBuf[k], (size_t)PRV_W * 2 * PRV_H * 2 * 2);
            g_prvImgOk = 1;
            g_prvStat[0]++;
        }
    }
    gx_hr_retarget(); /* volta para o alvo do jogo (a imagem de 64, na RAM) */
}

GxTex *gx_hr_prv_lookup(GxTex *tx, const GxTile *tl, const uint8_t *src, unsigned stride) {
    uint32_t off, row, col;
    GxTex *h;
    int x, y;
    if (!g_prvLive || g_prvTex == 0 || g_prvTexImg == NULL || src == NULL || tx == NULL || tx->px == NULL) {
        return NULL;
    }
    off = (uint32_t)(uintptr_t)src - g_prvTex;
    if (off >= (uint32_t)PRV_W * PRV_H * 2u) {
        if (tl->fmt == 0 && tl->siz == 2 && tx->w == PRV_W) {
            g_prvStat[5]++; /* (uma textura de 64 de largura em outro endereco: diagnostico) */
            g_prvLastOff = (uint32_t)(uintptr_t)src;
        }
        return NULL;
    }
    if (tl->fmt != 0 || tl->siz != 2 || (stride != 0 && stride != PRV_W * 2u) || (off & 1u)) {
        g_prvStat[2]++;
        return NULL;
    }
    row = off / (PRV_W * 2u);
    col = (off % (PRV_W * 2u)) / 2u;
    /* (a ultima faixa do menu e carregada com a altura das outras: as linhas depois da imagem sao lixo, ficam como
     * as da textura do jogo) */
    if (col + (uint32_t)tx->w > PRV_W || row >= PRV_H || tx->w <= 0 || tx->h <= 0) {
        g_prvStat[2]++;
        return NULL;
    }
    h = (GxTex *)calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }
    h->w = tx->w * 2;
    h->h = tx->h * 2;
    h->px = (uint32_t *)malloc((size_t)h->w * h->h * 4u);
    if (h->px == NULL) {
        free(h);
        return NULL;
    }
    for (y = 0; y < h->h; y++) {
        const uint8_t *sr = g_prvTexImg + ((size_t)(row * 2 + (uint32_t)y) * PRV_W * 2 + col * 2) * 2u;
        const uint32_t *na = tx->px + (size_t)(y / 2) * tx->w;
        uint32_t *d = h->px + (size_t)y * h->w;
        if (row * 2 + (uint32_t)y >= PRV_H * 2) {
            for (x = 0; x < h->w; x++) {
                d[x] = na[x / 2];
            }
            continue;
        }
        for (x = 0; x < h->w; x++) {
            const unsigned c = ((unsigned)sr[2 * x] << 8) | sr[2 * x + 1];
            const unsigned r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
            d[x] = ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) |
                   (na[x / 2] & 0xFF000000u); /* alfa: o da textura do jogo */
        }
    }
    h->clipw = tx->clipw * 2;
    h->cliph = tx->cliph * 2;
    h->key = tx->key;
    h->hdk = 2;
    h->hdTried = 1;
    g_prvStat[1]++;
    return h;
}

/* marcas 0x34..0x39 (gfx_rsp.c) */
void gx_hr_tag(uint32_t tag, uint32_t w0) {
    extern int gx_tmem_note;
    switch (tag & 15u) {
        case 4:
            pbg_save();
            break;
        case 5:
            pbg_begin();
            break;
        case 6:
            g_pbgSkip = 0;
            break;
        case 7:
            if (gx_hr_max > FB_W && !gx_capturing) {
                gx_batch_flush();
                g_prvArm = 1;
                g_prvColor = -1;
                g_prvRam[0] = g_prvRam[1] = NULL;
            }
            break;
        case 8:
            prv_end();
            break;
        case 9:
            g_prvStat[g_prvImgOk ? 3 : 4]++;
            if (g_prvImgOk) {
                if (g_prvTexImg == NULL) {
                    g_prvTexImg = (uint8_t *)malloc((size_t)PRV_W * 2 * PRV_H * 2 * 2);
                }
                if (g_prvTexImg != NULL) {
                    memcpy(g_prvTexImg, g_prvImg, (size_t)PRV_W * 2 * PRV_H * 2 * 2);
                    g_prvTex = 0x80000000u | (w0 & 0xFFFFFFu);
                    g_prvLive = 1;
                    gx_tmem_note = 1; /* (a cache de texturas precisa saber de onde veio cada carga) */
                }
            }
            break;
        default:
            break;
    }
}
