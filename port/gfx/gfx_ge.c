/*
 * gfx_ge.c -- o 2D do fim do quadro (HUD, logo da abertura) pelo acelerador 2D do aparelho (MI_GFX).
 *
 * O jogo marca o comeco do HUD (Interface_Draw) e do logo da abertura (EnMag_Draw) com um G_NOOP
 * (tools/oot-patch.py). Dali ate o fim da lista do quadro, gfx_rsp.c passa DUAS vezes pelos comandos:
 *   1. varredura: nada e desenhado; cada desenho vira uma "peca" com o retangulo que ocupa na tela e se
 *      o acelerador consegue fazer igual (retangulo de textura, sem z, combinador sem ruido, blender
 *      opaco ou de alfa: gx_bake_mode);
 *   2. desenho de verdade (o estado do RSP/RDP volta ao do comeco da marca): as pecas escolhidas na
 *      varredura nao sao desenhadas pelos nucleos; os texels delas (ja com o combinador aplicado, em ARGB
 *      com a cor multiplicada pelo alfa) vao para a memoria que o acelerador le, e a tela os desenha em
 *      640x480 por cima da imagem pronta, com a suavizacao da ampliacao dele.
 * A ordem do jogo e mantida: uma peca so vai para o acelerador se nenhuma peca dos nucleos desenhada
 * depois dela a cobre (a do acelerador ficaria por cima). As pecas pequenas ficam nos nucleos (cada
 * copia do acelerador custa uns 35 us de processador, mais que desenhar um coracao).
 *
 * Cada desenho (imagem do jogo ou intermediaria) usa uma "parte" da memoria (anel de GE_PARTS): a imagem
 * leva o numero da parte (gx_ge_attach) e a tela (video_miyoo.c) desenha as pecas dela quando a mostra.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gfx_priv.h"

int gx_ge_on = 1; /* opcao hud_acelerador */
int gx_ge_texto = 1; /* opcao texto_acelerador: a caixa de texto inteira como uma camada do acelerador */
/* opcao text_box_320: a camada da caixa de texto (caixa e letras) e montada em 320x240, como o resto da interface,
 * e o acelerador a amplia 2x com suavizacao na tela; 0 = montada em 640x480 (letras mais nitidas) */
int gx_texto_320 = 0;     /* em uso (gx_hr_sync_opts: a opcao, ou 1 com o filtro retro) */
int gx_texto_320_opt = 0; /* a opcao */
static int g_tlS = 1; /* escala da camada guardada: 1 = 640x480, 2 = 320x240 */
static size_t g_tlCap; /* pixels alocados em g_tl */
/* opcao logo_320: a logo da abertura e os textos dela (marca do escudo, 0x5458543C) ficam no acelerador 2D (texels da
 * textura original, 320x240, ampliados com a suavizacao dele) tambem enquanto aparecem/somem e com o PRESS START
 * piscando. Nessas horas so o alfa geral (cor primitiva) muda: os texels guardados com alfa 255 servem, e a copia para
 * a memoria do acelerador multiplica pelo alfa da vez (barato: a copia ja passa por cada texel). Antes, cada mudanca de
 * alfa mandava a peca de volta aos nucleos (na resolucao do 3D: a "oscilada") ou recalculava os texels (lento: lote 16).
 * Vale so quando o resultado e de fato proporcional ao alfa (conferido em 16 texels por peca); senao, como antes.
 * As chamas (mudam de desenho a cada quadro) continuam com os nucleos. */
int gx_logo_320 = 1;
/* opcao hud_alfa: o mesmo para o HUD e o resto do 2D (aparecendo e sumindo ao entrar em areas, cenas): a peca fica
 * no acelerador enquanto so o alfa muda, em vez de voltar aos nucleos */
int gx_ge_alfa = 1;
/* opcao hud_pequenas: as pecas pequenas (menos de GE_MIN_AREA) tambem pelo acelerador, se ficarem iguais por GE_STABLE
 * desenhos (as que mudam continuam nos nucleos); 0 = ficam nos nucleos como antes */
int gx_ge_pequenas = 1;
/* opcao hud_changing_on_accelerator: as pecas do HUD que mudam (o coracao pulsando, os numeros contando, os botoes
 * acendendo) tambem ficam no acelerador, com os texels recalculados quando mudam (pecas pequenas: barato), em vez de
 * irem para os nucleos enquanto mudam e voltarem depois (a peca "pulando" entre a nitidez de 640x480 e a do 3D).
 * Pecas grandes que mudam sempre (as chamas da logo) continuam com os nucleos. Vale tambem para a logo da abertura
 * (os textos com o brilho mudando de cor iam e voltavam: os "pulinhos" dela). 0 = como antes */
int gx_ge_mudando = 1;
#define GE_DYN_MAX 6144 /* texels (ou pixels, nos triangulos) de uma peca que muda, no maximo, para ficar no acelerador */
/* na logo da abertura: os textos com o brilho mudando de cor ("THE LEGEND OF", "OCARINA OF TIME", ~600-800 texels) ficam
 * no acelerador; as chamas (9 pecas de 4096 texels, mudando a cada desenho: recalcular custaria ~4 ms) continuam com
 * os nucleos, sempre (nunca alternam) */
#define GE_DYN_LOGO 2048
static int g_logoScan, g_logoPrev;
/* memoria que o acelerador le, uma por parte (video_miyoo.c; no PC, teste com --interp-dump). NULL: desligado */
uint8_t *(*gx_ge_mem)(int part, uint32_t *cap);

#define GE_MAXC 1024        /* pecas por desenho (da marca ao fim) */
#define GE_MAX_ITEMS 96     /* pecas pelo acelerador por imagem (48 era o teto na logo nova: o resto ia para os nucleos) */
#define GE_COPIES 160       /* copias de texels por imagem (faixas emendadas contam uma a uma) */
#define GE_MIN_AREA 1600    /* pixels (da tela de 640x480) para valer o acelerador */
#define GE_BAKE_N 96
#define GE_BAKE_BYTES (10u << 20) /* (a logo nova ampliada sozinha ocupa ~2,6 MB: com 3 MB as pecas dela eram
                                     * recalculadas de tempos em tempos e voltavam aos nucleos: a logo "oscilava") */

typedef struct {
    int16_t x0, y0, x1, y1; /* 640x480, [x0, x1) */
    uint8_t ok;
    uint32_t need; /* texels a calcular (0: ja prontos) */
    char why[40];
} GeCand;
static GeCand g_c[GE_MAXC];
static uint8_t g_take[GE_MAXC];
static uint8_t g_type[GE_MAXC]; /* 0 outro, 1 par de triangulos (retangulo), 2 triangulo sozinho */
static int g_nc, g_k, g_over;
static uint32_t g_need; /* (scan_ok -> cand) */
static uint32_t g_bakeBudget, g_histSeen;
#define GE_BAKE_BUDGET 64000u /* texels calculados por desenho, no maximo */
static int g_mode;   /* 0 nada; 1 varredura; 2 desenho com as escolhas */
static int g_inText; /* (varredura) dentro da caixa de texto */
/* Caixa de texto pelo acelerador: tudo o que o jogo desenha entre as marcas da caixa (fundo, letras, sombras,
 * icones) vira UMA camada: os nucleos calculam os texels de cada peca e as juntam numa imagem de 640x480 so da
 * regiao da caixa (cor * alfa), e o acelerador a cola por cima da imagem 3D, com a transparencia. A camada
 * fica guardada: enquanto o texto nao muda, nao e refeita; quando so aparecem letras novas no fim (o texto
 * sendo escrito), so elas sao juntadas. Se alguma peca da caixa nao der (triangulo, retangulo de cor...),
 * a caixa inteira fica com o caminho de antes (gfx_hires.c, texto_nitido). */
#define GE_GRP_MAX 768
static int g_grpFirst = -1, g_grpLast = -1, g_grpBad, g_grpTake, g_grpM;
static int g_grpRect[4];                  /* regiao da caixa (640x480) */
static int g_grpOrig[4];                  /* (a regiao marcada pelo jogo, antes de crescer) */
static uint64_t g_grpKeys[GE_GRP_MAX];    /* (varredura) chave de cada peca da caixa */
static int g_grpN, g_grpPrefix;
static uint64_t g_grpKey;
/* Camada da caixa refeita so com o texto (lote 75). Antes, qualquer peca guardada que mudasse refazia a camada
 * inteira (fundo da caixa ampliado com filtro + todas as letras: ~2 ms no PC, uns 8-10 no aparelho), e a setinha
 * de "continua" animada fazia isso quase todo desenho (log: "refeita 20" de 30): era o que derrubava a resolucao.
 *  - o fundo da caixa (uma das 4 primeiras pecas, com 1/5 ou mais da regiao) vai como peca comum do acelerador,
 *    nos texels originais, ampliado por ele (como o resto da interface em 320x240): fica fora da camada;
 *  - pecas pequenas no fim que mudam (a setinha, o icone piscando) viram a "cauda": pecas comuns do acelerador,
 *    desenhadas por cima da camada, recalculadas quando mudam. A camada guarda so o que vem antes delas;
 *  - a camada so com as letras: refeita quando o texto troca, e as letras novas entram sem refazer. */
static uint8_t g_grpBgF[GE_GRP_MAX];       /* (varredura) a peca e o fundo da caixa */
static int g_grpTail = 1 << 30;            /* pecas da caixa a partir daqui sao a cauda (neste desenho) */
static int g_tlTail = 1 << 30;             /* (guardado) onde a cauda comeca enquanto o texto nao muda */
static int g_itTail = -1;                  /* indice da 1a peca da cauda na lista da parte (a camada entra antes) */
#define GE_TAIL_MAX 6
#define GE_TAIL_AREA (56 * 56)
static uint32_t *g_tl;                    /* a camada guardada */
static int g_tlRect[4], g_tlN;
static uint64_t g_tlKeys[GE_GRP_MAX];
static unsigned g_grpWhy, g_grpWhyStat[11]; /* (diagnostico) motivos de recusa da caixa de texto */
static unsigned g_grpStat[4];             /* caixas pelo acelerador, refeitas, so letras novas, recusadas */
static int g_done;   /* a marca ja foi usada neste desenho */
static int g_pend, g_pendTake, g_pendType, g_pendOk; /* triangulo esperando o par (gx_ge_tri) */
static uint32_t g_pendNeed;
static void pend_flush(void);
static uint64_t mix(uint64_t h, uint64_t v);

typedef struct {
    GxGeItem it[GE_MAX_ITEMS];
    int n;
    uint32_t used, cap;
    uint8_t *mem;
    uint32_t dlo, dhi; /* o que foi escrito neste desenho (a tela limpa o cache disso) */
    /* o que esta na memoria: as pecas deste desenho (c) e do anterior nesta parte (p) */
    /* as copias feitas (uma por peca do jogo, mesmo as emendadas): chave e lugar, deste desenho (c) e do
     * anterior nesta parte (p). A mesma copia no mesmo lugar: ja esta na memoria */
    uint64_t ckey[GE_COPIES], pkey[GE_COPIES];
    uint32_t coff[GE_COPIES], poff[GE_COPIES];
    int cn, pn;
} GePart;
static GePart g_part[GX_GE_PARTS];
static int g_cur = -1, g_nextPart;

/* imagem -> parte com as pecas dela */
#define GE_ATT 24
static struct {
    const void *key;
    int part;
} g_att[GE_ATT];
static int g_attNext;

/* texels prontos (o HUD quase nao muda: so copia) */
typedef struct {
    uint64_t key;
    uint32_t *px;
    int sw, sh;
    uint32_t use, bytes;
} GeBake;
static GeBake g_bake[GE_BAKE_N];
static uint32_t g_bakeUse = 1, g_bakeBytes;

unsigned gx_ge_stat[6];
static uint64_t g_bakeTexels, g_copyBytes; /* diagnostico: pecas pelo acelerador, nos nucleos (nao da / pequenas / cobertas / limite), feitas */

void gx_ge_logo_mark(void) {
    if (g_mode == 1) {
        g_logoScan = 1;
    }
}

static int logo_on(void) {
    return gx_logo_320 && (g_logoScan || g_logoPrev);
}

void gx_ge_pass_begin(void) {
    pend_flush();
    g_mode = 0;
    g_done = 0;
    g_cur = -1;
}

/* marca do jogo: comeca a varredura? (gfx_rsp.c guarda o estado e passa de novo depois) */
int gx_ge_scan_begin(void) {
    GePart *p;
    if (g_done || !gx_ge_on || gx_ge_mem == NULL || gx_capturing || g_mode != 0) {
        return 0;
    }
    g_done = 1;
    p = &g_part[g_nextPart];
    if (p->mem == NULL) {
        p->mem = gx_ge_mem(g_nextPart, &p->cap);
        if (p->mem == NULL) {
            return 0;
        }
    }
    g_mode = 1;
    g_nc = 0;
    g_logoPrev = g_logoScan;
    g_logoScan = 0;
    g_bakeBudget = GE_BAKE_BUDGET;
    g_histSeen++;
    g_over = 0;
    g_inText = 0;
    g_grpFirst = g_grpLast = -1;
    g_grpBad = 0;
    g_grpN = 0;
    g_grpTake = 0;
    return 1;
}

static int overlap(const GeCand *a, const GeCand *b) {
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* fim da varredura: escolhe as pecas (do fim para o comeco: uma peca so vai se nenhuma dos nucleos depois
 * dela a cobre) e prepara o desenho de verdade */
void gx_ge_scan_end(int abort) {
    int k, j, n = 0;
    pend_flush();
    for (k = 0; k < g_nc; k++) {
        if (g_c[k].ok == 5) {
            g_c[k].ok = 0; /* (nao deveria sobrar) */
        }
    }
    memset(g_take, 0, sizeof g_take);
    if (abort || g_over) {
        g_mode = 0;
        return;
    }
    if (g_logoScan && gx_logo_320) {
        for (k = 0; k < g_nc; k++) {
            if (g_c[k].ok == 2) {
                g_c[k].ok = 1; /* (logo: as letras pequenas tambem, para os textos ficarem todos iguais) */
            }
        }
    }
    /* (pequenas tambem entram aqui: uma pequena por cima de uma grande deixa a grande ir para o acelerador;
     * depois, as pequenas que nao cobrem nenhuma do acelerador voltam para os nucleos) */
    g_grpTake = 0;
    for (k = g_nc - 1; k >= 0; k--) {
        const GeCand *c = &g_c[k];
        int ok = c->ok == 1 || c->ok == 2;
        if (g_grpFirst >= 0 && k >= g_grpFirst && k < g_grpLast) {
            /* caixa de texto: decidida inteira, na ultima peca dela */
            if (k == g_grpLast - 1) {
                GeCand gc;
                int valid = !g_grpBad && g_grpN == g_grpLast - g_grpFirst && n < GE_MAX_ITEMS;
                gc.x0 = (int16_t)g_grpRect[0];
                gc.y0 = (int16_t)g_grpRect[1];
                gc.x1 = (int16_t)g_grpRect[2];
                gc.y1 = (int16_t)g_grpRect[3];
                if (g_grpN != g_grpLast - g_grpFirst && !g_grpBad) {
                    g_grpWhy |= 1u << 10; /* contagem de pecas diferente */
                }
                for (j = g_grpFirst; j < g_grpLast && valid; j++) {
                    if (g_c[j].ok != 7) {
                        valid = 0;
                        g_grpWhy |= 1u << 9; /* peca que nao entrou na camada */
                    }
                }
                for (j = g_grpLast; j < g_nc && valid; j++) {
                    if (!g_take[j] && overlap(&gc, &g_c[j])) {
                        valid = 0; /* (os nucleos desenham algo por cima da caixa depois dela) */
                        g_grpWhy |= 1u << 8;
                    }
                }
                if (valid) {
                    for (j = g_grpFirst; j < g_grpLast; j++) {
                        g_take[j] = 1;
                    }
                    g_grpTake = 1;
                    n++;
                } else {
                    unsigned b;
                    g_grpStat[3]++;
                    for (b = 0; b < 11; b++) {
                        if (g_grpWhy & (1u << b)) {
                            g_grpWhyStat[b]++;
                        }
                    }
                }
            }
            continue;
        }
        if (!ok) {
            gx_ge_stat[c->ok == 2 ? 2 : c->ok >= 3 ? 4 : 1]++;
            continue;
        }
        for (j = k + 1; j < g_nc && ok; j++) {
            if (!g_take[j] && overlap(c, &g_c[j])) {
                ok = 0;
            }
        }
        if (!ok) {
            gx_ge_stat[3]++;
            continue;
        }
        if (c->need > g_bakeBudget && g_bakeBudget < GE_BAKE_BUDGET) {
            /* texels a calcular neste desenho: limite (fica com os nucleos ate a vez dela) */
            gx_ge_stat[4]++;
            continue;
        }
        if (n >= GE_MAX_ITEMS) {
            gx_ge_stat[4]++;
            continue;
        }
        g_take[k] = 1;
        g_bakeBudget = c->need > g_bakeBudget ? 0 : g_bakeBudget - c->need; /* (a 1a passa sempre) */
        n++;
    }
    for (k = 0; k < g_nc; k++) {
        if (g_take[k] && g_c[k].ok == 2) {
            int under = 0;
            for (j = 0; j < k && !under; j++) {
                if (g_take[j] && overlap(&g_c[k], &g_c[j])) {
                    under = 1;
                }
            }
            if (!under) {
                g_take[k] = 0;
                n--;
                gx_ge_stat[2]++;
            }
        }
    }
    {
        static int dbg = -1, shown;
        if (dbg < 0) {
            dbg = getenv("OOT_GE_LISTA") != NULL;
        }
        if (dbg && shown++ == 200) {
            for (k = 0; k < g_nc; k++) {
                fprintf(stderr, "[2d-lista] %3d ok=%d tipo=%d vai=%d (%d,%d)-(%d,%d) %s\n", k, g_c[k].ok, g_type[k], g_take[k], g_c[k].x0, g_c[k].y0, g_c[k].x1,
                        g_c[k].y1, g_c[k].why);
            }
        }
    }
    if (n == 0) {
        g_mode = 0;
        return;
    }
    if (g_grpTake) {
        /* o que da camada guardada continua valendo: as pecas do comeco iguais (texto sendo escrito) */
        int same = g_tl != NULL && memcmp(g_tlRect, g_grpRect, sizeof g_tlRect) == 0, lim;
        g_grpPrefix = 0;
        g_grpTail = 1 << 30;
        if (same) {
            while (g_grpPrefix < g_grpN && g_grpPrefix < g_tlN && g_grpKeys[g_grpPrefix] == g_tlKeys[g_grpPrefix]) {
                g_grpPrefix++;
            }
            if (g_grpPrefix < g_tlN) {
                /* alguma das guardadas mudou: se sao so pecas pequenas no fim, viram a cauda (a camada e refeita uma
                 * vez sem elas); senao, o texto mudou: refaz */
                const int P = g_grpPrefix;
                int small = g_grpN - P <= GE_TAIL_MAX && g_tlN - P <= GE_TAIL_MAX && P > 0;
                for (k = P; k < g_grpN && small; k++) {
                    const GeCand *c = &g_c[g_grpFirst + k];
                    small = !g_grpBgF[k] && (c->x1 - c->x0) * (c->y1 - c->y0) <= GE_TAIL_AREA;
                }
                g_tlTail = small ? P : 1 << 30;
                g_grpPrefix = 0;
            } else if (g_grpPrefix == g_tlN && g_tlTail <= g_tlN && g_grpN - g_tlN > GE_TAIL_MAX) {
                g_tlTail = 1 << 30; /* (cauda grande demais: era texto novo) */
                g_grpPrefix = 0;
            }
        } else {
            g_tlTail = 1 << 30;
        }
        if (g_tlTail < (1 << 30) && g_tlTail <= g_grpN) {
            g_grpTail = g_tlTail;
        }
        lim = g_grpN < g_grpTail ? g_grpN : g_grpTail;
        /* (a regiao entra na chave da copia: sem o fundo na camada, a camada vazia de uma caixa abrindo tinha a mesma
         * chave em tamanhos diferentes, e a copia antiga, de outra largura, era reaproveitada: letras cortadas/tortas
         * por um instante no fim da animacao) */
        g_grpKey = mix(0x9AE16A3B2F90404Full, (uint64_t)(uint32_t)(g_grpRect[0] | g_grpRect[1] << 16) << 32 |
                                                  (uint32_t)(g_grpRect[2] | g_grpRect[3] << 16));
        g_grpKey = mix(g_grpKey, (uint64_t)(gx_texto_320 ? 2 : 1));
        for (k = 0; k < lim; k++) {
            g_grpKey = mix(g_grpKey, g_grpKeys[k]);
        }
        g_itTail = -1;
    }
    g_mode = 2;
    g_k = 0;
    g_cur = g_nextPart;
    g_nextPart = (g_nextPart + 1) % GX_GE_PARTS;
    {
        GePart *p = &g_part[g_cur];
        memcpy(p->pkey, p->ckey, sizeof p->pkey);
        memcpy(p->poff, p->coff, sizeof p->poff);
        p->pn = p->cn;
        p->cn = 0;
        p->n = 0;
        p->used = 0;
        p->dlo = 0xFFFFFFFFu;
        p->dhi = 0;
    }
}

int gx_ge_scanning(void) {
    return g_mode == 1;
}

void gx_ge_text(int on, int x0, int y0, int x1, int y1) {
    if (g_mode != 1) {
        return;
    }
    pend_flush();
    if (on) {
        g_inText = 1;
        g_grpFirst = g_nc;
        g_grpLast = -1;
        g_grpBad = 0;
        g_grpN = 0;
        g_grpRect[0] = x0 * 2;
        g_grpRect[1] = y0 * 2;
        g_grpRect[2] = x1 * 2;
        g_grpRect[3] = y1 * 2;
        memcpy(g_grpOrig, g_grpRect, sizeof g_grpOrig);
        g_grpWhy = 0;
        if (x1 <= x0 || y1 <= y0 || !gx_ge_texto) {
            g_grpBad = 1;
            g_grpWhy |= 1u << 0;
        }
    } else if (g_inText) {
        g_inText = 0;
        g_grpLast = g_nc;
    }
}

static void cand(int x0, int y0, int x1, int y1, int ok) {
    GeCand *c;
    if (g_nc >= GE_MAXC) {
        g_over = 1;
        return;
    }
    c = &g_c[g_nc++];
    c->x0 = (int16_t)(x0 < -1000 ? -1000 : x0 > 3000 ? 3000 : x0);
    c->y0 = (int16_t)(y0 < -1000 ? -1000 : y0 > 3000 ? 3000 : y0);
    c->x1 = (int16_t)(x1 < -1000 ? -1000 : x1 > 3000 ? 3000 : x1);
    c->y1 = (int16_t)(y1 < -1000 ? -1000 : y1 > 3000 ? 3000 : y1);
    c->ok = (uint8_t)ok;
    g_type[g_nc - 1] = 0;
    c->need = ok ? g_need : 0;
    g_need = 0;
    snprintf(c->why, sizeof c->why, "cc=%08x/%08x om=%06x/%08x", gx.combW0, gx.combW1, gx.omH, gx.omL);
}

/* qualquer desenho que os nucleos fazem (triangulo, preenchimento...), em 640x480. 1 = nao desenhe */
static void pend_single(void);
static void pend_flush(void) {
    if (g_pend) {
        g_pend = 0;
        pend_single();
        g_pendTake = 0;
        g_pendType = 0;
    }
}

int gx_ge_other(int x0, int y0, int x1, int y1) {
    pend_flush();
    if (g_mode == 1) {
        if (g_inText) {
            g_grpBad = 1;
            g_grpWhy |= 1u << 1; /* preenchimento ou outro desenho que nao e retangulo de textura */
        }
        cand(x0, y0, x1, y1, 0);
        return 1;
    }
    if (g_mode == 2) {
        g_k++;
    }
    return 0;
}

/* (gx_ge_tri: depois do calculo dos texels) */

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

static GeBake *bake_get(uint64_t key, int sw, int sh, float s0, float t0, int mode, int hk) {
    int i, best = -1, y, x;
    uint32_t bu = 0xFFFFFFFFu, bytes = (uint32_t)sw * (uint32_t)sh * 4u;
    GeBake *b;
    for (i = 0; i < GE_BAKE_N; i++) {
        if (g_bake[i].px != NULL && g_bake[i].key == key && g_bake[i].sw == sw && g_bake[i].sh == sh) {
            g_bake[i].use = g_bakeUse++;
            return &g_bake[i];
        }
    }
    /* lugar: um vazio ou o usado ha mais tempo; e a memoria total limitada (tira os mais velhos) */
    for (;;) {
        int empty = -1;
        best = -1;
        bu = 0xFFFFFFFFu;
        for (i = 0; i < GE_BAKE_N; i++) {
            if (g_bake[i].px == NULL) {
                if (empty < 0) {
                    empty = i;
                }
            } else if (g_bake[i].use < bu) {
                bu = g_bake[i].use;
                best = i;
            }
        }
        if (empty >= 0 && g_bakeBytes + bytes <= GE_BAKE_BYTES) {
            best = empty;
            break;
        }
        if (best < 0) {
            return NULL;
        }
        g_bakeBytes -= g_bake[best].bytes;
        free(g_bake[best].px);
        g_bake[best].px = NULL;
    }
    b = &g_bake[best];
    b->px = (uint32_t *)malloc(bytes);
    if (b->px == NULL) {
        return NULL;
    }
    b->key = key;
    b->sw = sw;
    b->sh = sh;
    b->bytes = bytes;
    b->use = g_bakeUse++;
    g_bakeBytes += bytes;
    g_bakeTexels += (uint64_t)sw * (uint64_t)sh;
    {
        /* hk texels da textura nova por texel da original (gfx_hd.c) */
        const float st = 32.0f / (float)hk;
        for (y = 0; y < sh; y++) {
            const float T = t0 + ((float)y + 0.5f) * st;
            uint32_t *row = b->px + (size_t)y * (size_t)sw;
            for (x = 0; x < sw; x++) {
                row[x] = gx_bake_texel(s0 + ((float)x + 0.5f) * st, T, mode, hk, NULL);
            }
        }
    }
    return b;
}

/* o desenho atual pronto para calcular os texels: modo (gx_bake_mode), tamanho na grade dos texels (textura
 * nova: hk vezes maior) e a chave do conteudo. sw/shTex = texels da original. 0 = nao da */
typedef struct {
    int mode, sw, sh, hk;
    uint64_t key;  /* texels guardados */
    uint64_t ckey; /* copia na memoria do acelerador (key + alfa da vez) */
    int sa;        /* alfa da vez aplicado na copia (255: nada) */
} GePrep;

/* texels calculados com a cor primitiva de alfa 255 (sa != 255: a peca usa o alfa so como fator) */
static GeBake *bake_get_sa(const GePrep *r, float s0, float t0) {
    GeBake *b;
    const uint8_t a = gx.prim[3];
    if (r->sa != 255) {
        gx.prim[3] = 255;
    }
    b = bake_get(r->key, r->sw, r->sh, s0, t0, r->mode, r->hk);
    gx.prim[3] = a;
    return b;
}

/* copia uma linha de texels (cor ja vezes alfa) multiplicando tudo por sa/255 */
static void copy_row(uint32_t *d, const uint32_t *src, int n, int sa) {
    int x;
    if (sa == 255) {
        memcpy(d, src, (size_t)n * 4u);
        return;
    }
    for (x = 0; x < n; x++) {
        const uint32_t v = src[x];
        uint32_t lo = (v & 0x00FF00FFu) * (uint32_t)sa + 0x00800080u;
        uint32_t hi = ((v >> 8) & 0x00FF00FFu) * (uint32_t)sa + 0x00800080u;
        lo = ((lo + ((lo >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        hi = (hi + ((hi >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
        d[x] = lo | hi;
    }
}

/* a peca (estado atual) da o mesmo resultado com o alfa primitivo aplicado depois? 16 texels conferidos */
static int alpha_linear(int mode, int hk, float s0, float t0, float st, int sw, int sh) {
    const uint8_t a = gx.prim[3];
    int i, j, ok = 1;
    for (j = 0; j < 4 && ok; j++) {
        for (i = 0; i < 4 && ok; i++) {
            const float S = s0 + ((float)(i * (sw - 1)) / 3.0f + 0.5f) * st;
            const float T = t0 + ((float)(j * (sh - 1)) / 3.0f + 0.5f) * st;
            uint32_t full, real, sc;
            int ch;
            gx.prim[3] = 255;
            full = gx_bake_texel(S, T, mode, hk, NULL);
            gx.prim[3] = a;
            real = gx_bake_texel(S, T, mode, hk, NULL);
            copy_row(&sc, &full, 1, a);
            for (ch = 0; ch < 32; ch += 8) {
                const int dv = (int)((sc >> ch) & 255) - (int)((real >> ch) & 255);
                if (dv > 2 || dv < -2) {
                    ok = 0;
                }
            }
        }
    }
    gx.prim[3] = a;
    return ok;
}

static int prep(GePrep *r, int cyc, float s0, float t0, float swTex, float shTex) {
    int k2;
    uint64_t key;
    gx_tex_resolve();
    gx_pipe_begin();
    r->mode = gx_bake_mode();
    if (r->mode == 0) {
        return 0;
    }
    r->sw = (int)(swTex + 0.5f);
    r->sh = (int)(shTex + 0.5f);
    r->hk = gx_bake_hdk();
    if (r->hk > 1 && r->sw * r->hk <= 1022 && r->sh * r->hk <= 1022) {
        r->sw *= r->hk;
        r->sh *= r->hk;
    } else {
        r->hk = 1;
    }
    if (r->sw < 1 || r->sh < 1 || r->sw > 1022 || r->sh > 1022) {
        return 0;
    }
    key = 0xCBF29CE484222325ull;
    key = mix(key, (uint64_t)gx.combW0 << 32 | gx.combW1);
    key = mix(key, (uint64_t)gx.omL << 32 | gx.omH);
    r->sa = 255;
    if ((logo_on() || gx_ge_alfa) && gx.prim[3] != 255 && alpha_linear(r->mode, r->hk, s0, t0, 32.0f / (float)r->hk, r->sw, r->sh)) {
        r->sa = gx.prim[3]; /* (os texels com alfa 255; o alfa vai na copia) */
    }
    key = mix(key, (uint64_t)(gx.prim[0] | gx.prim[1] << 8 | gx.prim[2] << 16 | (uint32_t)(r->sa != 255 ? 255u : gx.prim[3]) << 24) << 32 |
                       (uint32_t)(gx.env[0] | gx.env[1] << 8 | gx.env[2] << 16 | (uint32_t)gx.env[3] << 24));
    key = mix(key, (uint64_t)gx.blendc[3] << 8 | gx.primLodFrac);
    for (k2 = 0; k2 < 2; k2++) {
        const GxTile *tl = &gx.tile[(gx.texTile + k2) & 7];
        const GxTex *tx = gx.rtex[k2];
        if (!gx_bake_uses(k2)) {
            continue;
        }
        key = mix(key, tx ? tx->key : 0);
        key = mix(key, (uint64_t)(tx && tx->hd ? tx->hd->hdk : 0)); /* (nao o endereco: muda quando a nova e remontada) */
        key = mix(key, (uint64_t)tl->uls | (uint64_t)tl->ult << 16 | (uint64_t)tl->lrs << 32 | (uint64_t)tl->lrt << 48);
        key = mix(key, (uint64_t)tl->cms | (uint64_t)tl->cmt << 4 | (uint64_t)tl->masks << 8 | (uint64_t)tl->maskt << 12 |
                           (uint64_t)tl->shifts << 16 | (uint64_t)tl->shiftt << 20);
    }
    key = mix(key, (uint64_t)(uint32_t)(int32_t)s0 << 32 | (uint32_t)(int32_t)t0);
    key = mix(key, (uint64_t)r->mode << 40 | (uint64_t)cyc << 32 | (uint64_t)r->hk << 48);
    r->key = key;
    r->ckey = r->sa != 255 ? mix(key, 0xA1FA000000000000ull | (uint64_t)r->sa) : key;
    /* a copia na memoria do acelerador tambem depende do TAMANHO: a mesma textura do mesmo ponto em larguras diferentes
     * da a mesma chave (o meio da barra de magia cresce de 8 em 8 texels ao encher). Sem isto, uma copia estreita de um
     * desenho antigo desta parte passava por "ja esta na memoria" e o MI_GFX a lia com a largura de linha nova: lixo em
     * faixas inclinadas na barra (sempre depois de carregar um estado sobre a agua) */
    r->ckey = mix(r->ckey, 0x512E000000000000ull | (uint64_t)(uint32_t)r->sw << 16 | (uint64_t)(uint32_t)r->sh);
    return 1;
}

static int bake_has(uint64_t key) {
    int i;
    for (i = 0; i < GE_BAKE_N; i++) {
        if (g_bake[i].px != NULL && g_bake[i].key == key) {
            return 1;
        }
    }
    return 0;
}

/* pecas que mudam a cada desenho (o fogo do logo rolando, o HUD aparecendo): calcular os texels toda vez
 * custa mais que os nucleos desenharem. Memoria por lugar na tela: quantas vezes seguidas mudou */
#define GE_HIST 128
static struct {
    int16_t x0, y0, x1, y1;
    uint8_t occ; /* (a n-esima peca neste mesmo retangulo no desenho: o fundo de um botao e o icone dele) */
    uint64_t key;
    uint8_t stable; /* desenhos seguidos com a peca igual */
    uint32_t seen;
} g_hist[GE_HIST];
/* uma peca so vai para o acelerador depois de ficar igual por GE_STABLE desenhos seguidos (~150 ms): numa
 * animacao (o logo aparecendo, um icone piscando) ela fica com os nucleos o tempo todo, em vez de alternar
 * entre os dois (a nitidez mudando a cada imagem: o piscar) e de calcular os texels a cada desenho */
#define GE_STABLE 6

static int dynamic_piece(int x0, int y0, int x1, int y1, uint64_t key) {
    int i, old = 0, occ = 0;
    for (i = 0; i < g_nc; i++) {
        if (g_c[i].x0 == x0 && g_c[i].y0 == y0 && g_c[i].x1 == x1 && g_c[i].y1 == y1) {
            occ++;
        }
    }
    for (i = 0; i < GE_HIST; i++) {
        if (g_hist[i].x0 == x0 && g_hist[i].y0 == y0 && g_hist[i].x1 == x1 && g_hist[i].y1 == y1 && g_hist[i].occ == occ &&
            g_hist[i].seen) {
            g_hist[i].seen = g_histSeen;
            if (g_hist[i].key != key) {
                g_hist[i].key = key;
                g_hist[i].stable = 0;
            } else if (g_hist[i].stable < 255) {
                g_hist[i].stable++;
            }
            return g_hist[i].stable < GE_STABLE;
        }
        if (g_hist[i].seen < g_hist[old].seen) {
            old = i;
        }
    }
    g_hist[old].x0 = (int16_t)x0;
    g_hist[old].y0 = (int16_t)y0;
    g_hist[old].x1 = (int16_t)x1;
    g_hist[old].y1 = (int16_t)y1;
    g_hist[old].occ = (uint8_t)occ;
    g_hist[old].key = key;
    g_hist[old].stable = 0;
    g_hist[old].seen = g_histSeen;
    return 1;
}

/* varredura: uma peca que da para o acelerador (r pronto) no retangulo (640x480) */
static int scan_ok(const GePrep *r, int x0, int y0, int x1, int y1) {
    int ok = 1;
    if ((x1 - x0) * (y1 - y0) < GE_MIN_AREA && !gx_ge_pequenas) {
        ok = 2;
    }
    if (ok == 1 && dynamic_piece(x0, y0, x1, y1, r->key) &&
        !(gx_ge_mudando && r->sw * r->sh <= (logo_on() ? GE_DYN_LOGO : GE_DYN_MAX))) {
        ok = 3; /* muda a cada desenho: os nucleos */
    }
    g_need = bake_has(r->key) ? 0u : (uint32_t)(r->sw * r->sh);
    return ok;
}

/* (copias) esta copia (chave, lugar) ja esta na memoria desde o ultimo desenho desta parte? anota */
static int copy_same(GePart *p, uint64_t key, uint32_t off) {
    const int c = p->cn;
    const int same = c < p->pn && p->pkey[c] == key && p->poff[c] == off;
    if (c < GE_COPIES) {
        p->ckey[c] = key;
        p->coff[c] = off;
        p->cn = c + 1;
    }
    return same && c < GE_COPIES;
}

static void dirty(GePart *p, uint32_t lo, uint32_t hi) {
    if (lo < p->dlo) {
        p->dlo = lo;
    }
    if (hi > p->dhi) {
        p->dhi = hi;
    }
}

/* desenho de verdade: os texels da peca (calculados ou da memoria) para a parte atual, e a peca na lista.
 * Faixas seguidas de uma mesma imagem (o logo vem em faixas de poucas linhas, por causa da TMEM) viram uma
 * peca so: menos chamadas do acelerador, e a suavizacao na emenda le a linha vizinha de verdade */
static int add_item(const GePrep *r, float s0, float t0, int x0, int y0, int x1, int y1) {
    GePart *p = &g_part[g_cur];
    GxGeItem *it;
    GeBake *b;
    uint32_t pw, ph, bytes, y;
    const int sw = r->sw, sh = r->sh;
    b = bake_get_sa(r, s0, t0);
    if (b == NULL) {
        return 0;
    }
    if (p->n > 0) {
        GxGeItem *pv = &p->it[p->n - 1];
        const uint32_t pvBytes = ((uint32_t)pv->pw * pv->ph * 4u + 63u) & ~63u;
        if (pv->dx == x0 && pv->dw == x1 - x0 && pv->dy + pv->dh == y0 && pv->sw == sw &&
            (int)pv->dh * sh == (y1 - y0) * (int)pv->sh && pv->off + pvBytes == p->used && pv->sh + sh <= 1022) {
            /* emenda embaixo da anterior: as linhas novas no lugar da borda de baixo dela */
            const uint32_t nsh = (uint32_t)pv->sh + (uint32_t)sh, nph = nsh + 2u;
            const uint32_t nbytes = ((uint32_t)pv->pw * nph * 4u + 63u) & ~63u;
            uint32_t *base = (uint32_t *)(p->mem + pv->off);
            const uint32_t row0 = (uint32_t)pv->sh + 1u; /* 1a linha nova */
            if (pv->off + nbytes > p->cap) {
                return 0;
            }
            pw = pv->pw;
            if (!copy_same(p, r->ckey, pv->off + row0 * pw * 4u)) {
                for (y = 0; y < (uint32_t)sh; y++) {
                    const uint32_t *src = b->px + (size_t)y * (size_t)sw;
                    uint32_t *d = base + (size_t)(row0 + y) * pw, x;
                    copy_row(d + 1, src, sw, r->sa);
                    d[0] = d[1];
                    for (x = (uint32_t)sw + 1u; x < pw; x++) {
                        d[x] = d[sw];
                    }
                }
                g_copyBytes += (uint64_t)pw * (uint64_t)sh * 4u;
                dirty(p, pv->off + row0 * pw * 4u, pv->off + (row0 + (uint32_t)sh) * pw * 4u);
            }
            /* borda de baixo nova (repete a ultima linha) */
            memcpy(base + (size_t)(nsh + 1u) * pw, base + (size_t)nsh * pw, (size_t)pw * 4u);
            dirty(p, pv->off + (nsh + 1u) * pw * 4u, pv->off + (nsh + 2u) * pw * 4u);
            pv->sh = (uint16_t)nsh;
            pv->ph = (uint16_t)nph;
            pv->dh = (int16_t)(pv->dh + (y1 - y0));
            p->used = pv->off + nbytes;
            gx_ge_stat[5]++;
            return 1;
        }
    }
    if (p->n >= GE_MAX_ITEMS) {
        return 0;
    }
    /* copia com borda de 1 texel repetindo a beirada (a suavizacao nas beiradas nao le vizinho) */
    pw = ((uint32_t)sw + 2u + 3u) & ~3u;
    ph = (uint32_t)sh + 2u;
    bytes = (pw * ph * 4u + 63u) & ~63u;
    if (p->used + bytes > p->cap) {
        return 0;
    }
    if (!copy_same(p, r->ckey, p->used)) {
        uint32_t *dst = (uint32_t *)(p->mem + p->used);
        for (y = 0; y < ph; y++) {
            const uint32_t sy = y == 0 ? 0 : (y > (uint32_t)sh ? (uint32_t)sh - 1u : y - 1u);
            const uint32_t *src = b->px + (size_t)sy * (size_t)sw;
            uint32_t *d = dst + (size_t)y * pw, x;
            copy_row(d + 1, src, sw, r->sa);
            d[0] = d[1];
            for (x = (uint32_t)sw + 1u; x < pw; x++) {
                d[x] = d[sw];
            }
        }
        g_copyBytes += (uint64_t)pw * ph * 4u;
        dirty(p, p->used, p->used + bytes);
    }
    it = &p->it[p->n++];
    it->off = p->used;
    it->pw = (uint16_t)pw;
    it->ph = (uint16_t)ph;
    it->sw = (uint16_t)sw;
    it->sh = (uint16_t)sh;
    it->dx = (int16_t)x0;
    it->dy = (int16_t)y0;
    it->dw = (int16_t)(x1 - x0);
    it->dh = (int16_t)(y1 - y0);
    p->used += bytes;
    gx_ge_stat[5]++;
    return 1;
}

/* uma peca da caixa de texto (texels prontos de r) por cima da camada, no retangulo (640x480) X0..X1, Y0..Y1:
 * ampliada com filtro (como o acelerador faria com ela sozinha) e juntada com o alfa (cor ja vezes alfa) */
static void grp_put(const GePrep *r, float s0, float t0, int X0, int Y0, int X1, int Y1) {
    const int S = g_tlS;
    const int W = (g_grpRect[2] - g_grpRect[0]) / S;
    int dw, dh;
    GeBake *b;
    int x, y;
    if (g_tl == NULL || X1 <= X0 || Y1 <= Y0) {
        return;
    }
    b = bake_get_sa(r, s0, t0);
    if (b == NULL) {
        return;
    }
    X0 = (X0 - g_grpRect[0]) / S + g_grpRect[0] / S; /* (em pixels da camada) */
    Y0 = (Y0 - g_grpRect[1]) / S + g_grpRect[1] / S;
    X1 = (X1 - g_grpRect[0]) / S + g_grpRect[0] / S;
    Y1 = (Y1 - g_grpRect[1]) / S + g_grpRect[1] / S;
    dw = X1 - X0;
    dh = Y1 - Y0;
    if (dw <= 0 || dh <= 0) {
        return;
    }
    for (y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)b->sh / (float)dh - 0.5f;
        int ya, yb, wy;
        uint32_t *d = g_tl + (size_t)(Y0 + y - g_grpRect[1] / S) * (size_t)W + (size_t)(X0 - g_grpRect[0] / S);
        if (fy < 0.0f) {
            fy = 0.0f;
        }
        ya = (int)fy;
        wy = (int)((fy - (float)ya) * 256.0f);
        if (ya >= b->sh - 1) {
            ya = b->sh - 1;
            wy = 0;
        }
        yb = ya + 1 < b->sh ? ya + 1 : ya;
        for (x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)b->sw / (float)dw - 0.5f;
            int xa, xb, wx, ch;
            uint32_t p00, p01, p10, p11, o = 0;
            unsigned sa, dv;
            if (fx < 0.0f) {
                fx = 0.0f;
            }
            xa = (int)fx;
            wx = (int)((fx - (float)xa) * 256.0f);
            if (xa >= b->sw - 1) {
                xa = b->sw - 1;
                wx = 0;
            }
            xb = xa + 1 < b->sw ? xa + 1 : xa;
            p00 = b->px[(size_t)ya * b->sw + xa];
            p01 = b->px[(size_t)ya * b->sw + xb];
            p10 = b->px[(size_t)yb * b->sw + xa];
            p11 = b->px[(size_t)yb * b->sw + xb];
            if ((p00 | p01 | p10 | p11) == 0) {
                continue;
            }
            for (ch = 0; ch < 32; ch += 8) {
                const int a0 = (int)((p00 >> ch) & 255) * (256 - wx) + (int)((p01 >> ch) & 255) * wx;
                const int a1 = (int)((p10 >> ch) & 255) * (256 - wx) + (int)((p11 >> ch) & 255) * wx;
                o |= (uint32_t)((a0 * (256 - wy) + a1 * wy + 32768) >> 16) << ch;
            }
            if (r->sa != 255) {
                copy_row(&o, &o, 1, r->sa); /* (o alfa da vez: texels guardados com alfa 255) */
            }
            sa = o >> 24;
            dv = d[x];
            if (sa >= 255 || dv == 0) {
                d[x] = o;
            } else {
                uint32_t res = 0;
                for (ch = 0; ch < 32; ch += 8) {
                    unsigned v = ((o >> ch) & 255u) + ((((dv >> ch) & 255u) * (256u - sa)) >> 8);
                    res |= (v > 255u ? 255u : v) << ch;
                }
                d[x] = res;
            }
        }
    }
}

/* marcas da caixa de texto no desenho de verdade: 1 = a caixa vai pela camada (gfx_hires.c nao a faz) */
int gx_ge_group(int on) {
    GePart *p;
    GxGeItem *it;
    int W, H;
    uint32_t pw, ph, bytes, y;
    if (g_mode != 2 || !g_grpTake || g_cur < 0) {
        return 0;
    }
    if (on && g_grpPrefix == 0) {
        g_tlS = gx_texto_320 ? 2 : 1; /* (so troca quando a camada e refeita) */
    }
    W = (g_grpRect[2] - g_grpRect[0]) / g_tlS;
    H = (g_grpRect[3] - g_grpRect[1]) / g_tlS;
    if (on) {
        if (g_grpPrefix == 0) {
            const size_t need = (size_t)W * H;
            if (g_tl == NULL || g_tlCap < need) {
                free(g_tl);
                g_tl = (uint32_t *)malloc(need * 4u);
                g_tlCap = g_tl != NULL ? need : 0; /* (pixels alocados; a regiao guardada e em 640x480) */
                memset(g_tlRect, 0, sizeof g_tlRect);
                g_tlN = 0;
                if (g_tl == NULL) {
                    g_grpTake = 0; /* (as pecas ja foram tiradas dos nucleos: a caixa some neste quadro) */
                    return 1;
                }
            }
            memset(g_tl, 0, need * 4u);
            g_grpStat[1]++;
        } else if (g_grpPrefix < (g_grpN < g_grpTail ? g_grpN : g_grpTail)) {
            g_grpStat[2]++;
        }
        g_grpM = 0;
        return 1;
    }
    /* fim: a camada guardada e copiada para a parte deste desenho */
    if (g_tl == NULL) {
        return 1;
    }
    memcpy(g_tlRect, g_grpRect, sizeof g_tlRect);
    {
        const int lim = g_grpN < g_grpTail ? g_grpN : g_grpTail;
        memcpy(g_tlKeys, g_grpKeys, (size_t)lim * sizeof g_tlKeys[0]);
        g_tlN = lim;
        g_grpPrefix = lim;
    }
    g_grpStat[0]++;
    p = &g_part[g_cur];
    if (p->n >= GE_MAX_ITEMS) {
        return 1;
    }
    {
        /* em 320x240 a camada e ampliada 2x aqui (com filtro), so quando o texto muda: o acelerador desenha 1:1, como
         * antes (ampliar no acelerador travou o aparelho com esta camada) */
        const int S = g_tlS, OW = W * S, OH = H * S;
        pw = ((uint32_t)OW + 2u + 3u) & ~3u;
        ph = (uint32_t)OH + 2u;
        bytes = (pw * ph * 4u + 63u) & ~63u;
        if (p->used + bytes > p->cap) {
            return 1;
        }
        if (!copy_same(p, g_grpKey, p->used)) {
            uint32_t *dst = (uint32_t *)(p->mem + p->used);
            memset(dst, 0, (size_t)pw * 4u);
            for (y = 0; y < (uint32_t)OH; y++) {
                uint32_t *d = dst + (size_t)(y + 1) * pw;
                d[0] = 0;
                if (S == 1) {
                    memcpy(d + 1, g_tl + (size_t)y * W, (size_t)W * 4u);
                } else {
                    /* 2x bilinear: amostras em (x+0,5)/2-0,5 -> pesos 1/4 e 3/4 */
                    int sy0 = ((int)y - 1) >> 1, sy1, x;
                    unsigned wy;
                    if ((int)y == 0) {
                        sy0 = 0;
                    }
                    sy1 = sy0 + 1 < H ? sy0 + 1 : sy0;
                    wy = ((int)y == 0 || (int)y == OH - 1) ? 0u : ((y & 1u) ? 64u : 192u);
                    {
                        const uint32_t *r0 = g_tl + (size_t)sy0 * W, *r1 = g_tl + (size_t)sy1 * W;
                        for (x = 0; x < OW; x++) {
                            int sx0 = (x - 1) >> 1, sx1, ch;
                            unsigned wx;
                            uint32_t o = 0;
                            if (x == 0) {
                                sx0 = 0;
                            }
                            sx1 = sx0 + 1 < W ? sx0 + 1 : sx0;
                            wx = (x == 0 || x == OW - 1) ? 0u : ((x & 1) ? 64u : 192u);
                            for (ch = 0; ch < 32; ch += 8) {
                                unsigned a = ((r0[sx0] >> ch) & 255u) * (256u - wx) + ((r0[sx1] >> ch) & 255u) * wx;
                                unsigned b = ((r1[sx0] >> ch) & 255u) * (256u - wx) + ((r1[sx1] >> ch) & 255u) * wx;
                                o |= ((a * (256u - wy) + b * wy + 32768u) >> 16) << ch;
                            }
                            d[1 + x] = o;
                        }
                    }
                }
                memset(d + 1 + OW, 0, (size_t)(pw - 1u - (uint32_t)OW) * 4u);
            }
            memset(dst + (size_t)(ph - 1u) * pw, 0, (size_t)pw * 4u);
            g_copyBytes += (uint64_t)pw * ph * 4u;
            dirty(p, p->used, p->used + bytes);
        }
        if (g_itTail >= 0 && g_itTail < p->n) {
            /* a camada entra antes das pecas da cauda (elas ficam por cima) */
            memmove(&p->it[g_itTail + 1], &p->it[g_itTail], (size_t)(p->n - g_itTail) * sizeof p->it[0]);
            it = &p->it[g_itTail];
            p->n++;
        } else {
            it = &p->it[p->n++];
        }
        g_itTail = -1;
        it->off = p->used;
        it->pw = (uint16_t)pw;
        it->ph = (uint16_t)ph;
        it->sw = (uint16_t)OW;
        it->sh = (uint16_t)OH;
        it->dx = (int16_t)g_grpRect[0];
        it->dy = (int16_t)g_grpRect[1];
        it->dw = (int16_t)OW;
        it->dh = (int16_t)OH;
    }
    p->used += bytes;
    gx_ge_stat[5]++;
    return 1;
}

/* um retangulo de textura (os parametros do comando). 1 = nao desenhe (varredura, ou foi para o acelerador) */
int gx_ge_texrect(uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, int flip) {
    const int lrx = (int)((w0 >> 12) & 0xFFF), lry = (int)(w0 & 0xFFF);
    const int tile = (int)((w1 >> 24) & 7);
    const int ulx = (int)((w1 >> 12) & 0xFFF), uly = (int)(w1 & 0xFFF);
    const float s0 = (float)(int16_t)(h1 >> 16), t0 = (float)(int16_t)(h1 & 0xFFFF);
    const float dsdx = (float)(int16_t)(h2 >> 16) / 1024.0f, dtdy = (float)(int16_t)(h2 & 0xFFFF) / 1024.0f;
    const int cyc = (int)((gx.omH >> GX_CYC_SHIFT) & 3);
    int x0, y0, x1, y1, ok;
    float swTex, shTex;
    GePrep r;
    if (g_mode == 0) {
        return 0;
    }
    pend_flush();
    /* os pixels que os nucleos cobririam em 320x240 */
    if (cyc >= GX_CYC_COPY) {
        x0 = ulx >> 2;
        y0 = uly >> 2;
        x1 = (lrx >> 2) + 1;
        y1 = (lry >> 2) + 1;
        swTex = (float)(x1 - x0) * dsdx * 0.25f;
        shTex = (float)(y1 - y0);
    } else {
        x0 = (ulx + 3) >> 2;
        y0 = (uly + 3) >> 2;
        x1 = (lrx + 3) >> 2;
        y1 = (lry + 3) >> 2;
        swTex = (float)(lrx - ulx) * 0.25f * dsdx;
        shTex = (float)(lry - uly) * 0.25f * dtdy;
    }
    if (g_mode == 2) {
        const int k = g_k++;
        if (k >= GE_MAXC || !g_take[k] || g_cur < 0) {
            return 0;
        }
        if (g_type[k] == 3) {
            /* peca da caixa de texto: vai para a camada (as que ja estao nela, guardada, nem sao calculadas) */
            const int m = g_grpM++;
            const int bg = m < GE_GRP_MAX && g_grpBgF[m], tail = m >= g_grpTail;
            if (m < g_grpPrefix && !bg) {
                return 1;
            }
            if (gx.texTile != tile) {
                gx.texTile = tile;
                gx.texDirty = 1;
            }
            if (bg || tail) {
                /* fundo da caixa (por baixo da camada: entra antes dela) ou cauda (por cima: a camada entra antes) */
                const int n0 = g_part[g_cur].n;
                if (prep(&r, cyc, s0, t0, swTex, shTex)) {
                    add_item(&r, s0, t0, x0 * 2, y0 * 2, x1 * 2, y1 * 2);
                }
                if (tail && g_itTail < 0) {
                    g_itTail = n0;
                }
                return 1;
            }
            if (prep(&r, cyc, s0, t0, swTex, shTex)) {
                grp_put(&r, s0, t0, x0 * 2, y0 * 2, x1 * 2, y1 * 2);
            }
            return 1;
        }
        if (gx.texTile != tile) {
            gx.texTile = tile;
            gx.texDirty = 1;
        }
        if (!prep(&r, cyc, s0, t0, swTex, shTex)) {
            return 0;
        }
        return add_item(&r, s0, t0, x0 * 2, y0 * 2, x1 * 2, y1 * 2);
    }
    if (g_inText) {
        /* varredura dentro da caixa de texto: peca da camada? */
        int okT = !(flip || dsdx <= 0.0f || dtdy <= 0.0f || cyc == GX_CYC_FILL || x1 <= x0 || y1 <= y0);
        if (!okT) {
            g_grpWhy |= 1u << 3; /* espelhado, escala negativa ou preenchimento */
        }
        if (okT && ((uint32_t)ulx < gx.scRaw[0] || (uint32_t)uly < gx.scRaw[1] || (uint32_t)lrx > gx.scRaw[2] ||
                    (uint32_t)lry > gx.scRaw[3])) {
            okT = 0;
            g_grpWhy |= 1u << 4; /* cortado pela tesoura */
        }
        if (okT && (x0 * 2 < g_grpRect[0] || y0 * 2 < g_grpRect[1] || x1 * 2 > g_grpRect[2] || y1 * 2 > g_grpRect[3])) {
            /* peca um pouco fora da regiao marcada (a seta de "continua", a caixa abrindo/fechando): a regiao cresce
             * para conte-la (ate 48 pixels de 640x480 alem da marcada, dentro da tela); mais longe, recusa */
            const int m = 48;
            if (x0 * 2 >= g_grpOrig[0] - m && y0 * 2 >= g_grpOrig[1] - m && x1 * 2 <= g_grpOrig[2] + m &&
                y1 * 2 <= g_grpOrig[3] + m && x0 >= 0 && y0 >= 0 && x1 <= 320 && y1 <= 240) {
                if (x0 * 2 < g_grpRect[0]) g_grpRect[0] = x0 * 2;
                if (y0 * 2 < g_grpRect[1]) g_grpRect[1] = y0 * 2;
                if (x1 * 2 > g_grpRect[2]) g_grpRect[2] = x1 * 2;
                if (y1 * 2 > g_grpRect[3]) g_grpRect[3] = y1 * 2;
            } else {
                okT = 0; /* (fora da regiao da caixa) */
                g_grpWhy |= 1u << 5;
            }
        }
        if (okT) {
            if (gx.texTile != tile) {
                gx.texTile = tile;
                gx.texDirty = 1;
            }
            okT = prep(&r, cyc, s0, t0, swTex, shTex);
            if (!okT) {
                g_grpWhy |= 1u << 6; /* combinador/formato que o acelerador nao reproduz */
            }
        }
        if (okT && g_grpN >= GE_GRP_MAX) {
            g_grpWhy |= 1u << 7; /* pecas demais */
        }
        if (!okT || g_grpN >= GE_GRP_MAX) {
            g_grpBad = 1;
        } else {
            const int ow = g_grpOrig[2] - g_grpOrig[0], oh = g_grpOrig[3] - g_grpOrig[1];
            const int bg = g_grpN < 4 && (x1 - x0) * 2 * (y1 - y0) * 2 * 5 >= ow * oh;
            g_grpBgF[g_grpN] = (uint8_t)bg;
            /* (o fundo nao conta na chave da camada: ele nao esta nela) */
            g_grpKeys[g_grpN++] = bg ? 0 : mix(r.ckey, (uint64_t)(uint32_t)(x0 | y0 << 12) << 32 | (uint32_t)(x1 | y1 << 12));
        }
        g_need = 0;
        cand(x0 * 2, y0 * 2, x1 * 2, y1 * 2, okT ? 7 : 0);
        if (!g_over) {
            g_type[g_nc - 1] = 3;
        }
        return 1;
    }
    /* varredura: da para o acelerador? */
    ok = 1;
    if (g_inText || flip || dsdx <= 0.0f || dtdy <= 0.0f || cyc == GX_CYC_FILL || x1 <= x0 || y1 <= y0) {
        ok = 0;
    }
    /* inteiro dentro da tesoura (o acelerador nao corta) e da tela */
    if (ok && ((uint32_t)ulx < gx.scRaw[0] || (uint32_t)uly < gx.scRaw[1] || (uint32_t)lrx > gx.scRaw[2] ||
               (uint32_t)lry > gx.scRaw[3] || x0 < 0 || y0 < 0 || x1 > 320 || y1 > 240)) {
        ok = 0;
    }
    if (ok) {
        if (gx.texTile != tile) {
            gx.texTile = tile;
            gx.texDirty = 1;
        }
        ok = prep(&r, cyc, s0, t0, swTex, shTex) ? scan_ok(&r, x0 * 2, y0 * 2, x1 * 2, y1 * 2) : 0;
    }
    cand(x0 * 2, y0 * 2, x1 * 2, y1 * 2, ok);
    return 1;
}

/* Triangulos: dois seguidos que formam um retangulo alinhado com a tela, com a textura esticada so na
 * horizontal (S so muda com x, T so com y), w e cor iguais nos 4 cantos (o coracao grande, o botao A e a
 * palavra dele no HUD) sao um retangulo de textura como os outros. g_pend: o 1o triangulo de um par
 * (varredura: a peca g_nc - 1; desenho: g_pendTake = a peca foi para o acelerador, o 2o tambem nao e
 * desenhado) */
static GxVtx g_pv[3];

/* o retangulo de 2 triangulos (6 vertices) e a textura nele; 0 = nao e */
static int quad_of(const GxVtx *v, float *X0, float *Y0, float *X1, float *Y1, float *s0, float *t0, float *swT,
                   float *shT) {
    float xs[2], ys[2];
    int nx = 0, ny = 0, i, j;
    const float e = 0.02f * gx.rs;
    for (i = 0; i < 6; i++) {
        for (j = 0; j < nx && fabsf(v[i].sx - xs[j]) > e; j++) {
        }
        if (j == nx) {
            if (nx == 2) {
                return 0;
            }
            xs[nx++] = v[i].sx;
        }
        for (j = 0; j < ny && fabsf(v[i].sy - ys[j]) > e; j++) {
        }
        if (j == ny) {
            if (ny == 2) {
                return 0;
            }
            ys[ny++] = v[i].sy;
        }
        if (fabsf(v[i].w - v[0].w) > 1e-3f * fabsf(v[0].w) || v[i].r != v[0].r || v[i].g != v[0].g || v[i].b != v[0].b ||
            v[i].a != v[0].a || v[i].w <= 0.0f) {
            return 0;
        }
    }
    if (nx != 2 || ny != 2) {
        return 0;
    }
    if (xs[0] > xs[1]) {
        const float t = xs[0];
        xs[0] = xs[1];
        xs[1] = t;
    }
    if (ys[0] > ys[1]) {
        const float t = ys[0];
        ys[0] = ys[1];
        ys[1] = t;
    }
    {
        /* S de cada coluna e T de cada linha (iguais nos vertices da mesma coluna / linha) */
        float sc[2] = { -1e30f, -1e30f }, tr[2] = { -1e30f, -1e30f };
        for (i = 0; i < 6; i++) {
            const int cx = fabsf(v[i].sx - xs[0]) <= e ? 0 : 1, cy = fabsf(v[i].sy - ys[0]) <= e ? 0 : 1;
            if (sc[cx] == -1e30f) {
                sc[cx] = v[i].s;
            } else if (fabsf(sc[cx] - v[i].s) > 0.5f) {
                return 0;
            }
            if (tr[cy] == -1e30f) {
                tr[cy] = v[i].t;
            } else if (fabsf(tr[cy] - v[i].t) > 0.5f) {
                return 0;
            }
        }
        if (sc[1] <= sc[0] || tr[1] <= tr[0]) {
            return 0; /* (espelhada: os nucleos) */
        }
        *X0 = xs[0] / gx.rs;
        *X1 = xs[1] / gx.rs;
        *Y0 = ys[0] / gx.rs;
        *Y1 = ys[1] / gx.rs;
        *s0 = sc[0];
        *t0 = tr[0];
        *swT = (sc[1] - sc[0]) / 32.0f;
        *shT = (tr[1] - tr[0]) / 32.0f;
    }
    return 1;
}

static int bbox_tri(const GxVtx *a, const GxVtx *b, const GxVtx *c, int *X0, int *Y0, int *X1, int *Y1) {
    const float k = 2.0f / (gx.rs > 0.0f ? gx.rs : 1.0f);
    float x0 = a->sx < b->sx ? a->sx : b->sx, x1 = a->sx > b->sx ? a->sx : b->sx;
    float y0 = a->sy < b->sy ? a->sy : b->sy, y1 = a->sy > b->sy ? a->sy : b->sy;
    x0 = c->sx < x0 ? c->sx : x0;
    x1 = c->sx > x1 ? c->sx : x1;
    y0 = c->sy < y0 ? c->sy : y0;
    y1 = c->sy > y1 ? c->sy : y1;
    *X0 = (int)(x0 * k) - 2;
    *Y0 = (int)(y0 * k) - 2;
    *X1 = (int)(x1 * k) + 3;
    *Y1 = (int)(y1 * k) + 3;
    return 1;
}

/* Triangulo sozinho (a seta do mini mapa, o botao A girando...): calculado pixel a pixel em 640x480 (centro
 * de cada pixel, cor e textura interpoladas), sem textura em perspectiva (w diferente nos vertices com
 * textura: os nucleos). Cabe numa peca do tamanho do retangulo dele, 1:1 */
static uint32_t g_stateSig; /* (estado do 1o triangulo de um par) */

static uint32_t state_sig(void) {
    uint64_t h = 0x84222325ull;
    h = mix(h, (uint64_t)gx.combW0 << 32 | gx.combW1);
    h = mix(h, (uint64_t)gx.omL << 32 | gx.omH);
    h = mix(h, (uint64_t)(gx.prim[0] | gx.prim[1] << 8 | gx.prim[2] << 16 | (uint32_t)gx.prim[3] << 24) << 32 |
                   (uint32_t)(gx.env[0] | gx.env[1] << 8 | gx.env[2] << 16 | (uint32_t)gx.env[3] << 24));
    h = mix(h, (uint64_t)gx.texTile << 32 | gx.tmemGen);
    return (uint32_t)(h ^ (h >> 32));
}

typedef struct {
    float x[3], y[3]; /* 640x480 */
    float s[3], t[3];
    float c[3][4];
    int X0, Y0, W, H;
} GeTri;

static int tri_prep(GePrep *r, GeTri *g, const GxVtx *v) {
    const float k = 2.0f / (gx.rs > 0.0f ? gx.rs : 1.0f);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, area;
    int i;
    uint64_t key;
    gx_tex_resolve();
    gx_pipe_begin();
    r->mode = gx_bake_mode();
    if (r->mode == 0) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (v[i].w <= 0.0f) {
            return 0;
        }
        if ((gx_bake_uses(0) || gx_bake_uses(1)) && fabsf(v[i].w - v[0].w) > 1e-3f * fabsf(v[0].w)) {
            return 0; /* (textura em perspectiva) */
        }
        g->x[i] = v[i].sx * k;
        g->y[i] = v[i].sy * k;
        g->s[i] = v[i].s;
        g->t[i] = v[i].t;
        g->c[i][0] = v[i].r;
        g->c[i][1] = v[i].g;
        g->c[i][2] = v[i].b;
        g->c[i][3] = v[i].a;
        x0 = g->x[i] < x0 ? g->x[i] : x0;
        x1 = g->x[i] > x1 ? g->x[i] : x1;
        y0 = g->y[i] < y0 ? g->y[i] : y0;
        y1 = g->y[i] > y1 ? g->y[i] : y1;
    }
    area = (g->x[1] - g->x[0]) * (g->y[2] - g->y[0]) - (g->x[2] - g->x[0]) * (g->y[1] - g->y[0]);
    if (fabsf(area) < 1.0f || x0 < 0.0f || y0 < 0.0f || x1 > 640.0f || y1 > 480.0f) {
        return 0;
    }
    g->X0 = (int)floorf(x0);
    g->Y0 = (int)floorf(y0);
    g->W = (int)ceilf(x1) - g->X0;
    g->H = (int)ceilf(y1) - g->Y0;
    if (g->W < 1 || g->H < 1 || g->W > 640 || g->H > 480) {
        return 0;
    }
    r->sw = g->W;
    r->sh = g->H;
    r->hk = 1;
    key = (uint64_t)state_sig() << 32 | 0x7A1u;
    for (i = 0; i < 3; i++) {
        key = mix(key, (uint64_t)(uint32_t)(int32_t)(g->x[i] * 16.0f) << 32 | (uint32_t)(int32_t)(g->y[i] * 16.0f));
        key = mix(key, (uint64_t)(uint32_t)(int32_t)g->s[i] << 32 | (uint32_t)(int32_t)g->t[i]);
        key = mix(key, (uint64_t)(uint32_t)(g->c[i][0] + 256.0f * g->c[i][1] + 65536.0f * g->c[i][2]) << 8 | (uint64_t)g->c[i][3]);
    }
    key = mix(key, (uint64_t)r->mode);
    r->key = key;
    r->ckey = key;
    r->sa = 255;
    return 1;
}

/* os pixels do triangulo (dentro: o centro do pixel), cor ja multiplicada pelo alfa; fora: 0 */
static GeBake *bake_tri(const GePrep *r, const GeTri *g) {
    int i, best, x, y;
    uint32_t bu, bytes = (uint32_t)r->sw * (uint32_t)r->sh * 4u;
    const float A = (g->x[1] - g->x[0]) * (g->y[2] - g->y[0]) - (g->x[2] - g->x[0]) * (g->y[1] - g->y[0]);
    GeBake *b;
    for (i = 0; i < GE_BAKE_N; i++) {
        if (g_bake[i].px != NULL && g_bake[i].key == r->key && g_bake[i].sw == r->sw && g_bake[i].sh == r->sh) {
            g_bake[i].use = g_bakeUse++;
            return &g_bake[i];
        }
    }
    for (;;) {
        int empty = -1;
        best = -1;
        bu = 0xFFFFFFFFu;
        for (i = 0; i < GE_BAKE_N; i++) {
            if (g_bake[i].px == NULL) {
                if (empty < 0) {
                    empty = i;
                }
            } else if (g_bake[i].use < bu) {
                bu = g_bake[i].use;
                best = i;
            }
        }
        if (empty >= 0 && g_bakeBytes + bytes <= GE_BAKE_BYTES) {
            best = empty;
            break;
        }
        if (best < 0) {
            return NULL;
        }
        g_bakeBytes -= g_bake[best].bytes;
        free(g_bake[best].px);
        g_bake[best].px = NULL;
    }
    b = &g_bake[best];
    b->px = (uint32_t *)calloc(1, bytes);
    if (b->px == NULL) {
        return NULL;
    }
    b->key = r->key;
    b->sw = r->sw;
    b->sh = r->sh;
    b->bytes = bytes;
    b->use = g_bakeUse++;
    g_bakeBytes += bytes;
    g_bakeTexels += (uint64_t)r->sw * (uint64_t)r->sh;
    for (y = 0; y < r->sh; y++) {
        const float py = (float)(g->Y0 + y) + 0.5f;
        for (x = 0; x < r->sw; x++) {
            const float px = (float)(g->X0 + x) + 0.5f;
            /* coordenadas baricentricas */
            const float l1 = ((px - g->x[0]) * (g->y[2] - g->y[0]) - (g->x[2] - g->x[0]) * (py - g->y[0])) / A;
            const float l2 = ((g->x[1] - g->x[0]) * (py - g->y[0]) - (px - g->x[0]) * (g->y[1] - g->y[0])) / A;
            const float l0 = 1.0f - l1 - l2;
            int sh[4], ch;
            if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) {
                continue;
            }
            for (ch = 0; ch < 4; ch++) {
                const float v = l0 * g->c[0][ch] + l1 * g->c[1][ch] + l2 * g->c[2][ch];
                sh[ch] = v < 0.0f ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f);
            }
            b->px[(size_t)y * (size_t)r->sw + (size_t)x] =
                gx_bake_texel(l0 * g->s[0] + l1 * g->s[1] + l2 * g->s[2], l0 * g->t[0] + l1 * g->t[1] + l2 * g->t[2], r->mode, 1, sh);
        }
    }
    return b;
}

static int add_tri(const GePrep *r, const GeTri *g) {
    GeBake *b = bake_tri(r, g);
    GePart *p = &g_part[g_cur];
    GxGeItem *it;
    uint32_t pw, ph, bytes, y;
    if (b == NULL || p->n >= GE_MAX_ITEMS) {
        return 0;
    }
    pw = ((uint32_t)r->sw + 2u + 3u) & ~3u;
    ph = (uint32_t)r->sh + 2u;
    bytes = (pw * ph * 4u + 63u) & ~63u;
    if (p->used + bytes > p->cap) {
        return 0;
    }
    if (!copy_same(p, r->key, p->used)) {
        uint32_t *dst = (uint32_t *)(p->mem + p->used);
        /* borda transparente (fora do triangulo ja e 0) */
        memset(dst, 0, (size_t)pw * ph * 4u);
        for (y = 0; y < (uint32_t)r->sh; y++) {
            memcpy(dst + (size_t)(y + 1) * pw + 1, b->px + (size_t)y * (size_t)r->sw, (size_t)r->sw * 4u);
        }
        g_copyBytes += (uint64_t)pw * ph * 4u;
        dirty(p, p->used, p->used + bytes);
    }
    it = &p->it[p->n++];
    it->off = p->used;
    it->pw = (uint16_t)pw;
    it->ph = (uint16_t)ph;
    it->sw = (uint16_t)r->sw;
    it->sh = (uint16_t)r->sh;
    it->dx = (int16_t)g->X0;
    it->dy = (int16_t)g->Y0;
    it->dw = (int16_t)r->sw;
    it->dh = (int16_t)r->sh;
    p->used += bytes;
    gx_ge_stat[5]++;
    return 1;
}


/* varredura: o triangulo pendente (peca g_nc - 1) fica sozinho */
static void pend_single(void) {
    if (g_mode == 1 && g_nc > 0 && g_c[g_nc - 1].ok == 5) {
        g_c[g_nc - 1].ok = g_pendOk;
        g_c[g_nc - 1].need = g_pendOk ? g_pendNeed : 0;
        g_type[g_nc - 1] = 2;
    }
}

int gx_ge_tri(const GxVtx *a, const GxVtx *b, const GxVtx *c) {
    GxVtx tv[3];
    GePrep r;
    GeTri g;
    int X0, Y0, X1, Y1;
    if (g_mode == 0) {
        return 0;
    }
    tv[0] = *a;
    tv[1] = *b;
    tv[2] = *c;
    if (g_pend) {
        /* 2o triangulo de um par? */
        GxVtx v[6];
        float x0, y0, x1, y1, s0, t0, swT, shT;
        const int sameState = g_stateSig == state_sig();
        v[0] = g_pv[0];
        v[1] = g_pv[1];
        v[2] = g_pv[2];
        v[3] = *a;
        v[4] = *b;
        v[5] = *c;
        g_pend = 0;
        if (g_mode == 2) {
            const int t = g_pendType;
            g_pendType = 0;
            if (t == 1) {
                /* o par (decidido na varredura) */
                if (g_pendTake && quad_of(v, &x0, &y0, &x1, &y1, &s0, &t0, &swT, &shT)) {
                    const int ix0 = (int)(x0 * 2.0f + 0.5f), iy0 = (int)(y0 * 2.0f + 0.5f);
                    const int ix1 = (int)(x1 * 2.0f + 0.5f), iy1 = (int)(y1 * 2.0f + 0.5f);
                    g_pendTake = 0;
                    if (prep(&r, (int)((gx.omH >> GX_CYC_SHIFT) & 3), s0, t0, swT, shT) &&
                        add_item(&r, s0, t0, ix0, iy0, ix1, iy1)) {
                        return 1;
                    }
                }
                g_pendTake = 0;
                return 0;
            }
            g_pendTake = 0;
            /* (o anterior era sozinho: este e uma peca nova, abaixo) */
        } else if (sameState && quad_of(v, &x0, &y0, &x1, &y1, &s0, &t0, &swT, &shT)) {
            const int ix0 = (int)(x0 * 2.0f + 0.5f), iy0 = (int)(y0 * 2.0f + 0.5f);
            const int ix1 = (int)(x1 * 2.0f + 0.5f), iy1 = (int)(y1 * 2.0f + 0.5f);
            GeCand *cd = &g_c[g_nc - 1];
            int ok = 0;
            g_need = 0;
            if (!g_inText && cd->ok == 5 && ix0 >= 0 && iy0 >= 0 && ix1 <= 640 && iy1 <= 480 && ix1 > ix0 && iy1 > iy0 &&
                prep(&r, (int)((gx.omH >> GX_CYC_SHIFT) & 3), s0, t0, swT, shT)) {
                ok = scan_ok(&r, ix0, iy0, ix1, iy1);
            }
            cd->x0 = (int16_t)(ix0 < cd->x0 ? ix0 : cd->x0);
            cd->y0 = (int16_t)(iy0 < cd->y0 ? iy0 : cd->y0);
            cd->x1 = (int16_t)(ix1 > cd->x1 ? ix1 : cd->x1);
            cd->y1 = (int16_t)(iy1 > cd->y1 ? iy1 : cd->y1);
            cd->ok = (uint8_t)ok;
            cd->need = ok ? g_need : 0;
            g_need = 0;
            g_type[g_nc - 1] = 1;
            return 1;
        } else {
            pend_single();
        }
    }
    bbox_tri(a, b, c, &X0, &Y0, &X1, &Y1);
    if (g_mode == 1 && g_inText) {
        g_grpBad = 1; /* (triangulo na caixa de texto) */
        g_grpWhy |= 1u << 2;
    }
    if (g_mode == 1) {
        /* pode ser o 1o de um par: ok = 5 ate ver o proximo; o resultado como triangulo sozinho fica guardado */
        int ok = 0;
        g_need = 0;
        if (!g_inText && tri_prep(&r, &g, tv)) {
            ok = scan_ok(&r, g.X0, g.Y0, g.X0 + g.W, g.Y0 + g.H);
        }
        g_pendOk = ok;
        g_pendNeed = g_need;
        g_need = 0;
        cand(X0, Y0, X1, Y1, 5);
        if (g_nc > 0) {
            g_type[g_nc - 1] = 2;
        }
        g_pv[0] = *a;
        g_pv[1] = *b;
        g_pv[2] = *c;
        g_stateSig = state_sig();
        g_pend = 1;
        return 1;
    }
    {
        const int k = g_k++;
        const int take = k < GE_MAXC && g_take[k];
        const int t = k < GE_MAXC ? g_type[k] : 0;
        if (t == 1) {
            /* 1o de um par: espera o 2o */
            g_pv[0] = *a;
            g_pv[1] = *b;
            g_pv[2] = *c;
            g_pend = 1;
            g_pendType = 1;
            g_pendTake = take;
            return take;
        }
        if (take && tri_prep(&r, &g, tv) && add_tri(&r, &g)) {
            return 1;
        }
        return 0;
    }
}

/* fim de um desenho: as pecas vao com a imagem 'key' (o framebuffer do jogo ou a imagem intermediaria) */
void gx_ge_attach(const void *key) {
    int i, part = (g_cur >= 0 && g_part[g_cur].n > 0) ? g_cur : -1;
    g_mode = 0;
    for (i = 0; i < GE_ATT; i++) {
        if (g_att[i].key == key) {
            __atomic_store_n(&g_att[i].part, part, __ATOMIC_RELEASE);
            return;
        }
    }
    if (part < 0) {
        return;
    }
    i = g_attNext;
    g_attNext = (g_attNext + 1) % GE_ATT;
    __atomic_store_n(&g_att[i].part, -1, __ATOMIC_RELEASE);
    g_att[i].key = key;
    __atomic_store_n(&g_att[i].part, part, __ATOMIC_RELEASE);
}

/* as pecas da imagem 'key' (tela); devolve quantas e a parte */
int gx_ge_items(const void *key, const GxGeItem **items, int *part) {
    int i;
    for (i = 0; i < GE_ATT; i++) {
        if (g_att[i].key == key) {
            const int pt = __atomic_load_n(&g_att[i].part, __ATOMIC_ACQUIRE);
            if (pt < 0) {
                return 0;
            }
            *items = g_part[pt].it;
            *part = pt;
            return g_part[pt].n;
        }
    }
    return 0;
}

/* memoria da parte e o trecho escrito no ultimo desenho dela ([lo, hi); lo >= hi: nada novo) */
const uint8_t *gx_ge_part_mem(int part, uint32_t *lo, uint32_t *hi) {
    *lo = g_part[part].dlo;
    *hi = g_part[part].dhi;
    return g_part[part].mem;
}

void gx_ge_print_stats(void) {
    if (gx_ge_stat[0] + gx_ge_stat[1] + gx_ge_stat[5]) {
        fprintf(stderr, "[2d] pecas pelo acelerador %u | nos nucleos: nao da %u, pequenas %u, cobertas depois %u, mudando/limite %u | "
                        "texels calculados %llu, copiados %llu KB\n",
                gx_ge_stat[5], gx_ge_stat[1], gx_ge_stat[2], gx_ge_stat[3], gx_ge_stat[4], (unsigned long long)g_bakeTexels,
                (unsigned long long)(g_copyBytes >> 10));
        g_bakeTexels = g_copyBytes = 0;
    }
    if (g_grpStat[0] + g_grpStat[3]) {
        fprintf(stderr, "[2d] caixa de texto pelo acelerador: %u desenhos (refeita %u, so letras novas %u), recusada %u\n",
                g_grpStat[0], g_grpStat[1], g_grpStat[2], g_grpStat[3]);
        memset(g_grpStat, 0, sizeof g_grpStat);
        fprintf(stderr, "[2d] caixa recusada, motivos: desligada %u, preenchimento/outro desenho %u, triangulo %u, espelhado %u, "
                        "tesoura %u, fora da regiao %u, combinador %u, pecas demais %u, desenho por cima depois %u, "
                        "peca fora %u, contagem %u\n",
                g_grpWhyStat[0], g_grpWhyStat[1], g_grpWhyStat[2], g_grpWhyStat[3], g_grpWhyStat[4], g_grpWhyStat[5],
                g_grpWhyStat[6], g_grpWhyStat[7], g_grpWhyStat[8], g_grpWhyStat[9], g_grpWhyStat[10]);
        memset(g_grpWhyStat, 0, sizeof g_grpWhyStat);
    }
    memset(gx_ge_stat, 0, sizeof gx_ge_stat);
}
