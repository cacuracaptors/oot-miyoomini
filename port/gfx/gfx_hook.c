/*
 * gfx_hook.c -- liga o renderizador ao "hardware" do port (ganchos do os_hw) e
 * oferece diagnosticos: gravar quadros em PPM, estatisticas e histograma de comandos.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx_priv.h"
#include "../perf_trace.h"
#include "os_hw.h"
#include "../frame_interp.h"

static struct {
    const char *prefix;
    int every, max, count, seen;
} g_dump;

static struct {
    const char *prefix;
    uint32_t at[16];
    int n;
} g_cap;

void gfx_capture_config(const char *prefix, const char *list) {
    const char *p = list;
    g_cap.prefix = prefix;
    g_cap.n = 0;
    while (p && *p && g_cap.n < 16) {
        char *e;
        unsigned long v = strtoul(p, &e, 10);
        if (e == p) {
            break;
        }
        g_cap.at[g_cap.n++] = (uint32_t)v;
        p = (*e == ',') ? e + 1 : e;
    }
}

static struct {
    uint64_t tasks, cmds, tris, trisDrawn, rects, texLoads, bad, ucode, culled, clipRej, verts;
    uint64_t hist[256];
} g_tot;

void gfx_dump_config(const char *prefix, int every, int max) {
    g_dump.prefix = prefix;
    g_dump.every = every < 1 ? 1 : every;
    g_dump.max = max;
}

static void hook_vi_present(const void *fb, const OSViMode *mode, int blank) {
    (void)mode;
    static int first = 1;
    if (first) {
        first = 0;
        fprintf(stderr, "[gfx] primeiro present: fb=%p blank=%d\n", fb, blank);
    }
    if (fb != NULL && !blank) {
        gx_hr_screen(fb); /* este framebuffer vai para a tela: pode ser desenhado em resolucao maior */
    }
    if (g_dump.prefix == NULL || fb == NULL) {
        return;
    }
    g_dump.seen++;
    if ((g_dump.seen % g_dump.every) == 0 && g_dump.count < g_dump.max) {
        char path[512];
        snprintf(path, sizeof path, "%s_%04d.png", g_dump.prefix, g_dump.count++);
        if (gx_dump_png((const uint8_t *)fb, 320, 240, path) != 0) {
            fprintf(stderr, "[gfx] nao consegui gravar %s\n", path);
        } else {
            fprintf(stderr, "[gfx] gravou %s (fb=%p blank=%d)\n", path, fb, blank);
            gx_probe_print(fb);
            gx_seg_print();
        }
    }
}

/* Captura pedida por uma tecla (aparelho: SELECT+Y): o proximo quadro e gravado como
 * captura_NN.cap (para o oot_replay refazer e depurar) e captura_NN.png (como ficou). */
static volatile int g_capReq;
void gfx_request_capture(void) {
    __atomic_store_n(&g_capReq, 1, __ATOMIC_RELEASE);
}

/* Captura automatica dos quadros lentos (opcoes.txt captura_lenta = N ms; 0 = desligada):
 * quando um quadro demora mais que N ms para desenhar, o seguinte (quase igual) e gravado como
 * lento_NN.cap/.png, para medir no PC exatamente as cenas que pesam. No maximo 8 por vez que o
 * jogo abre, com pelo menos 3 s entre elas (gravar atrasa aquele quadro). */
int gx_slowcap_ms = 0;
int gx_slowcap_after = 0; /* opcao captura_inicio: segundos de jogo antes de gravar quadros lentos */
static int g_slowReq, g_slowCount;
static uint64_t g_slowLastNs;

static uint64_t hook_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int capture_on_request(OSTask *t) {
    char path[64], png[64];
    const char *pre = "captura";
    int n;
    FILE *f;
    if (g_slowReq) {
        g_slowReq = 0;
        pre = "lento";
    } else if (!__atomic_exchange_n(&g_capReq, 0, __ATOMIC_ACQ_REL)) {
        return 0;
    }
    for (n = 0; n < 100; n++) {
        snprintf(path, sizeof path, "%s_%02d.cap", pre, n);
        f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        fclose(f);
    }
    snprintf(png, sizeof png, "%s_%02d.png", pre, n);
    if (gx_capture_render((uint32_t)(uintptr_t)t->t.data_ptr, path) == 0) {
        fprintf(stderr, "[gfx] captura gravada: %s\n", path);
        if (gx.cimg.addr != NULL) {
            gx_dump_png((const uint8_t *)gx.cimg.addr, 320, 240, png);
        }
    } else {
        fprintf(stderr, "[gfx] a captura falhou (%s)\n", path);
    }
    return 1;
}

/* ------------------------------------------------------------------ quadros intermediarios
 * A logica do jogo faz 20 quadros por segundo (um a cada 3 vsyncs). Com quadros_extras ligado a tela
 * mostra 30 (um a cada 2 vsyncs): em cada 2 quadros da logica sao desenhados 3, nas posicoes 1/3, 1 e 2/3
 * do caminho entre o quadro anterior e o atual (como o Ship of Harkinian):
 *   tipo A: 1/3 (entra na tela quando o jogo troca de quadro) e o quadro do jogo (2 vsyncs depois)
 *   tipo B: so 2/3 (1 vsync depois da troca); o quadro do jogo desse tick nao aparece
 * O quadro do jogo aparece 2 vsyncs mais tarde que sem a opcao (33 ms).
 *
 * Desenho adiado (extra_adiado = 1): no tipo A so o 1/3 e desenhado antes de o jogo saber que a tarefa
 * acabou; o quadro do jogo (que so aparece 2 vsyncs depois da troca) e desenhado logo em seguida
 * (gfx_post), no tempo em que o desenho ficaria parado esperando a troca de quadro. Assim cada tick tem
 * so 1 desenho no caminho do jogo e cabem desenhos de ate ~31 ms (sem adiar: 2 por tick, ~23 ms).
 * O jogo espera esse desenho acabar antes de montar o quadro seguinte naquela memoria (Port_GfxBarrier,
 * chamado no graph.c). A tela so mostra a imagem adiada quando ela fica pronta (gx_interp_notready).
 *
 * Se mesmo assim nao couber, os dois ticks mostram so o quadro do jogo, 2 vsyncs depois da troca (20 fps
 * regulares, com o mesmo atraso do modo de 30: a troca entre os modos nao da tranco).
 *
 * Ritmo: a imagem "na troca" (vs 0) nunca saia na hora: quando o jogo troca de quadro, a thread da tela ja
 * passou do ponto de escolher a imagem daquele vsync, e ela entrava 1 vsync depois. As imagens ficavam
 * 1, 2 e 3 vsyncs na tela (medido no aparelho), em vez de 2, 2 e 2. Agora tudo entra FI_VS vsyncs mais
 * tarde (A: 1/3 em 1, jogo em 3; B: 2/3 em 2), sempre a tempo: 2, 2 e 2. */
#define FI_VS (1 + port_opt_folga)
/* opcoes.txt atraso_extra: vsyncs a mais antes de cada imagem entrar na tela (folga para picos; o controle
 * responde esse tanto mais tarde) */
int port_opt_folga = 1;
void (*gx_interp_sink)(const void *fb, const uint16_t *const *img, const int *vs, int n);
int port_opt_extra_adiado = 1;
/* opcoes.txt quadros_degraus: entre 30 e 20 fps, degraus intermediarios. Um tick "A" mostra 2 imagens
 * (1/3 e o quadro do jogo), um tick "B" mostra 1 (2/3). AB AB... = 30 fps; so B = 20 fps; A seguido de n
 * ticks B = 60*(n+2)/(3*(n+1)) fps (n=2: 26,7; n=3: 25; n=4: 24). Cada imagem continua entrando no vsync
 * da hora que ela retrata (1/3, 2/3, 1 do tick), entao o movimento fica certo mesmo com imagens de 2 e 3
 * vsyncs. O n sai do tempo medio de um desenho: n+2 desenhos em (n+1) ticks de 50 ms. */
int port_opt_degraus = 1;
static int g_dgN = 1;   /* ticks B depois de cada A */
static int g_dgB = 1;   /* ticks B desde o ultimo A */
static int g_dgLastA;   /* o tick anterior foi A */
volatile const uint16_t *gx_interp_notready; /* imagem adiada ainda sendo desenhada (a tela espera) */
static FiRepl *g_repl;
static uint16_t *g_ibuf[6];
static int g_ibufW[6], g_ibufH[6]; /* tamanho da imagem em cada um (resolucao maior: ate 640x480) */
static int g_ibufNext, g_itick, g_fall;
static double g_passMs = 15.0; /* media do tempo de um desenho */
static double g_lastMs = 15.0; /* o ultimo desenho */
static struct {
    int pending;
    uint32_t dl;
    uint16_t *dst;
    unsigned nSet, nStart, nDone; /* diagnostico */
} g_def;
/* espera do jogo (Port_GfxBarrier): fila do proprio jogo, para a thread que espera ceder a vez as outras
 * threads do jogo (som, escalonador) em vez de segurar tudo parado */
static OSMesgQueue g_barQ;
static OSMesg g_barBuf[8];
static int g_barInit, g_barWaiting;
/* estatistica para o log (a cada ~2 s): desenhos, tempo, quantos ticks A ficaram so com 1 desenho (20 fps) */
static struct {
    uint64_t t0;
    int n, nA, nAfall, h[6];
    double sum, max, build, cpu, helper, wait;
} g_ist;

static uint64_t thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}


/* ------------------------------------------------------------------ registro quadro a quadro
 * Opcao registro_quadros = 1: cada desenho, cada tick do jogo, cada espera do jogo e cada imagem que vai
 * para a tela viram uma linha "[q]" no log.txt (guardadas num anel e escritas junto com as estatisticas,
 * fora das threads de tempo real). Os tempos sao em ms desde o primeiro registro. */
int gx_trace = 0;
typedef struct {
    uint64_t t;
    float a, b, c, d, e;
    int i0, i1;
    char k;
} TrRec;
#define TR_N 4096u
static TrRec g_tr[TR_N];
static unsigned g_trW, g_trR;
static uint64_t g_trT0;

void gfx_trace(char k, int i0, int i1, float a, float b, float c, float d, float e) {
    unsigned w;
    TrRec *r;
    if (!gx_trace) {
        return;
    }
    w = __atomic_fetch_add(&g_trW, 1u, __ATOMIC_ACQ_REL);
    r = &g_tr[w % TR_N];
    r->t = hook_now_ns();
    r->k = k;
    r->i0 = i0;
    r->i1 = i1;
    r->a = a;
    r->b = b;
    r->c = c;
    r->d = d;
    r->e = e;
}

static void gfx_trace_flush(void) {
    const unsigned w = __atomic_load_n(&g_trW, __ATOMIC_ACQUIRE);
    if (w - g_trR > TR_N) {
        fprintf(stderr, "[q] (perdidos %u registros)\n", w - g_trR - TR_N);
        g_trR = w - TR_N;
    }
    for (; g_trR != w; g_trR++) {
        const TrRec *r = &g_tr[g_trR % TR_N];
        double t;
        if (g_trT0 == 0) {
            g_trT0 = r->t;
        }
        t = (double)(int64_t)(r->t - g_trT0) / 1e6;
        switch (r->k) {
            case 'P':
                /* desenho: tipo (0 sem interpolar, 1 = 1/3, 2 = 2/3, 3 = quadro do jogo, 4 = adiado, 5 = 20 fps) */
                fprintf(stderr, "[q] %.1f P%d %.1f cpu=%.1f aux=%.1f esp=%.1f som=%.1f tri=%d\n", t, r->i0, r->a, r->b, r->c,
                        r->d, r->e, r->i1);
                break;
            case 'T':
                fprintf(stderr, "[q] %.1f T tick=%d 20fps=%d\n", t, r->i0, r->i1);
                break;
            case 'B':
                fprintf(stderr, "[q] %.1f B espera=%.1f\n", t, r->a);
                break;
            case 'X':
                /* do desenho que vem a seguir no registro: texturas decodificadas, copias com borda, filtros
                 * pre-calculados (quantas e ms) e esvaziamentos da fila */
                fprintf(stderr, "[q] %.1f X dec=%d/%.2f borda=%d/%.2f filtro=%d/%.2f esvazia=%d\n", t, r->i0 & 0xFFFF,
                        r->a, r->i0 >> 16, r->b, r->i1 & 0xFFFF, r->c, r->i1 >> 16);
                break;
            case 'V':
                /* imagem na tela: vsyncs desde a anterior, adiada atrasada, tempo da copia */
                fprintf(stderr, "[q] %.1f V gap=%d atrasada=%d copia=%.1f\n", t, r->i0, r->i1, r->a);
                break;
            default:
                break;
        }
    }
}

static void interp_stat(double ms) {
    static const double lim[5] = { 18.0, 20.0, 23.0, 26.0, 30.0 };
    int k = 0;
    uint64_t now = hook_now_ns();
    while (k < 5 && ms > lim[k]) {
        k++;
    }
    g_ist.h[k]++;
    g_ist.n++;
    g_ist.sum += ms;
    if (ms > g_ist.max) {
        g_ist.max = ms;
    }
    if (g_ist.t0 == 0) {
        g_ist.t0 = now;
    } else if (now - g_ist.t0 >= 2000000000ull) {
        fprintf(stderr,
                "[quadros extras] %d desenhos, media %.1f ms, pior %.1f ms | ate 18: %d, 18-20: %d, 20-23: %d, 23-26: %d, "
                "26-30: %d, mais de 30: %d | ticks A sem o extra (20 fps): %d de %d | por desenho: matrizes %.2f ms, "
                "cpu da thread %.1f ms, auxiliar %.1f ms, esperando a auxiliar %.1f ms\n",
                g_ist.n, g_ist.sum / g_ist.n, g_ist.max, g_ist.h[0], g_ist.h[1], g_ist.h[2], g_ist.h[3], g_ist.h[4],
                g_ist.h[5], g_ist.nAfall, g_ist.nA, g_ist.build / g_ist.n, g_ist.cpu / g_ist.n, g_ist.helper / g_ist.n,
                g_ist.wait / g_ist.n);
        memset(&g_ist, 0, sizeof g_ist);
        g_ist.t0 = now;
    }
}

static const float *repl_lookup(const void *mtx) {
    return port_fi_lookup(g_repl, mtx);
}

static const void *repl_vtx_lookup(const void *src, unsigned bytes) {
    return port_fi_vtx_lookup(g_repl, src, bytes);
}

static int repl_tile_lookup(const void *cmd, uint32_t *w0, uint32_t *w1) {
    return port_fi_tile_lookup(g_repl, cmd, w0, w1);
}

static const uint32_t *repl_cmd_lookup(const void *cmd) {
    return port_fi_cmd_lookup(g_repl, cmd);
}

/* tamanho de uma imagem intermediaria (a tela pergunta); 0 = nao e uma delas */
int gx_img_size(const void *img, int *w, int *h) {
    int i;
    for (i = 0; i < 6; i++) {
        if (g_ibuf[i] != NULL && (const void *)g_ibuf[i] == img) {
            *w = g_ibufW[i] ? g_ibufW[i] : 320;
            *h = g_ibufH[i] ? g_ibufH[i] : 240;
            return 1;
        }
    }
    return 0;
}

/* caixa de texto em 640x480 de cada imagem (gx_hr_text): copiada junto com a imagem */
static uint8_t *g_itxt[6];
static int g_itxtR[6][4], g_itxtOn[6];

static void ibuf_set_text(const uint16_t *b, const void *fbRaw) {
    int i;
    for (i = 0; i < 6; i++) {
        if (g_ibuf[i] == b) {
            const uint8_t *tb;
            int r[4], y;
            g_itxtOn[i] = 0;
            if (!gx_hr_text_of(fbRaw, &tb, r)) {
                return;
            }
            if (g_itxt[i] == NULL) {
                g_itxt[i] = (uint8_t *)malloc(640u * 480u * 2u);
                if (g_itxt[i] == NULL) {
                    return;
                }
            }
            for (y = r[1]; y < r[3]; y++) {
                const size_t o = ((size_t)y * 640u + (size_t)r[0]) * 2u;
                memcpy(g_itxt[i] + o, tb + o, (size_t)(r[2] - r[0]) * 2u);
            }
            memcpy(g_itxtR[i], r, sizeof r);
            g_itxtOn[i] = 1;
            return;
        }
    }
}

/* a caixa de texto (640x480, bytes do N64) que vai por cima da imagem img; 0 = nenhuma */
int gx_img_text(const void *img, const uint8_t **buf, int rect[4]) {
    int i;
    for (i = 0; i < 6; i++) {
        if (g_ibuf[i] != NULL && (const void *)g_ibuf[i] == img) {
            if (!g_itxtOn[i]) {
                return 0;
            }
            *buf = g_itxt[i];
            memcpy(rect, g_itxtR[i], sizeof g_itxtR[i]);
            return 1;
        }
    }
    return 0;
}

static void ibuf_set_size(const uint16_t *b, int w, int h) {
    int i;
    for (i = 0; i < 6; i++) {
        if (g_ibuf[i] == b) {
            g_ibufW[i] = w;
            g_ibufH[i] = h;
        }
    }
}

static uint16_t *ibuf_take(void) {
    extern void gx_hr_release(const void *p);
    uint16_t *dst = g_ibuf[g_ibufNext];
    g_ibufNext = (g_ibufNext + 1) % 6;
    gx_hr_release(dst); /* (se uma tela usava esta imagem como a maior dela) */
    return dst;
}

/* desenha o display list (com as matrizes interpoladas para 'step' se step < 1) e copia a imagem para dst
 * (NULL = proximo buffer livre) */
void *(*gx_img_alloc)(size_t bytes); /* (video_miyoo.c: memoria do chip 2D para as imagens da fila) */

static const uint16_t *interp_pass_to(uint16_t *dst, uint32_t dl, volatile int *yield, int cs, int ps, float step,
                                      const void **fbOut) {
    extern uint64_t gx_helper_ns, gx_wait_ns;
    uint64_t t0 = hook_now_ns(), c0 = thread_cpu_ns(), h0 = gx_helper_ns, w0 = gx_wait_ns;
    const uint64_t a0 = os_hw_stats()->audio_ns;
    const int defd = dst != NULL;
    if (dst == NULL) {
        dst = ibuf_take();
    }
    {
        extern void gx_hr_target(void *p);
        gx_hr_target(dst); /* (desenho direto nesta imagem: gfx_hires.c) */
    }
    if (step < 1.0f) {
        port_fi_build(cs, ps, step, g_repl);
        gx_mtx_lookup = repl_lookup;
        gx_vtx_lookup = repl_vtx_lookup;
        gx_tile_lookup = repl_tile_lookup;
        gx_cmd_lookup = repl_cmd_lookup;
    }
    g_ist.build += (double)(hook_now_ns() - t0) / 1e6;
    memset(gx_stN, 0, sizeof gx_stN);
    memset(gx_stNs, 0, sizeof gx_stNs);
    gx_hr_pass_begin(); /* tamanho deste desenho (resolucao dinamica) */
    gx_render(dl, yield);
    {
        extern void gx_hr_target_done(const void *fbRaw);
        gx_hr_target_done(gx.cimgRaw);
    }
    /* texturas novas e esvaziamentos da fila neste desenho (o do fim do desenho conta 1) */
    gfx_trace('X', (int)(gx_stN[0] | (gx_stN[1] << 16)), (int)(gx_stN[2] | (gx_stN[3] << 16)),
              (float)(gx_stNs[0] / 1e6), (float)(gx_stNs[1] / 1e6), (float)(gx_stNs[2] / 1e6), 0, 0);
    gx_mtx_lookup = NULL;
    gx_vtx_lookup = NULL;
    gx_tile_lookup = NULL;
    gx_cmd_lookup = NULL;
    *fbOut = gx.cimgRaw;
    {
        const double ms = (double)(hook_now_ns() - t0) / 1e6;
        const uint16_t *hi;
        int hw, hh;
        { extern int gx_hr_interm; gx_hr_interm = step < 1.0f; } /* (z reduzido so no quadro de verdade) */
        gx_hr_pass_end(ms); /* a tela do N64 recebe a imagem reduzida; a maior fica para mostrar */
        gx_ge_attach(dst); /* o 2D pelo acelerador desta imagem (gfx_ge.c) */
        if (gx_hr_image(gx.cimgRaw, &hi, &hw, &hh)) {
            extern uint64_t gx_hr_copyNs;
            extern unsigned gx_hr_copyN;
            if (hi != dst) {
                const uint64_t tc = hook_now_ns();
                memcpy(dst, hi, (size_t)hw * hh * 2u);
                gx_hr_copyNs += hook_now_ns() - tc; /* (medida: a copia da imagem maior para a fila da tela) */
                gx_hr_copyN++;
            }
            ibuf_set_size(dst, hw, hh);
            ibuf_set_text(dst, gx.cimgRaw);
        } else if (gx.cimgRaw != NULL && gx.cimgRawW == 320) {
            if (gx_hr_le) {
                /* (as imagens da fila ficam todas na ordem do processador: opcao cor_direta) */
                const uint16_t *s16 = (const uint16_t *)gx.cimgRaw;
                int k;
                for (k = 0; k < 320 * 240; k++) {
                    dst[k] = __builtin_bswap16(s16[k]);
                }
            } else {
                memcpy(dst, gx.cimgRaw, 320 * 240 * 2);
            }
            ibuf_set_size(dst, 320, 240);
            ibuf_set_text(dst, gx.cimgRaw); /* (caixa de texto nitida tambem com o desenho em 320) */
        } else {
            memset(dst, 0, 320 * 240 * 2);
            ibuf_set_size(dst, 320, 240);
            ibuf_set_text(dst, NULL);
        }
        if (ms < 62.0) {
            /* (um desenho isolado muito longo - carregando cena, texturas novas chegando - nao e o peso da
             * cena: sem isto ele jogava a media acima de 31 ms e a tela caia para 20-27 fps por um tempo) */
            g_passMs = g_passMs * 0.75 + ms * 0.25;
            g_lastMs = ms;
        }
        g_ist.cpu += (double)(thread_cpu_ns() - c0) / 1e6;
        g_ist.helper += (double)(gx_helper_ns - h0) / 1e6;
        g_ist.wait += (double)(gx_wait_ns - w0) / 1e6;
        interp_stat(ms);
        PT_EV(PT_FRAME, step < 0.4f ? 1 : step < 0.7f ? 2 : defd ? 4 : g_fall ? 5 : 3);
        gfx_trace('P', step < 0.4f ? 1 : step < 0.7f ? 2 : defd ? 4 : g_fall ? 5 : 3, (int)gx.nTriDrawn, (float)ms,
                  (float)((double)(thread_cpu_ns() - c0) / 1e6), (float)((double)(gx_helper_ns - h0) / 1e6),
                  (float)((double)(gx_wait_ns - w0) / 1e6), (float)((double)(os_hw_stats()->audio_ns - a0) / 1e6));
    }
    return dst;
}

static const uint16_t *interp_pass(uint32_t dl, volatile int *yield, int cs, int ps, float step, const void **fbOut) {
    return interp_pass_to(NULL, dl, yield, cs, ps, step, fbOut);
}

/* thread do desenho, logo depois de o jogo receber o "acabou": desenha a imagem adiada */
static void hook_gfx_post(void) {
    const void *fb;
    if (!__atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
        return;
    }
    g_def.nStart++;
    interp_pass_to(g_def.dst, g_def.dl, NULL, 0, 0, 1.0f, &fb);
    __atomic_store_n(&gx_interp_notready, NULL, __ATOMIC_RELEASE);
    g_def.nDone++;
    __atomic_store_n(&g_def.pending, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_barWaiting, __ATOMIC_SEQ_CST)) {
        os_async_send(&g_barQ, (OSMesg)1); /* acorda o jogo */
    }
}

/* thread do jogo (graph.c), antes de montar um quadro na memoria 'pool': se o desenho adiado ainda le
 * aquela memoria (o quadro de 2 atras), espera. O do quadro anterior (outra memoria) nao precisa: no N64
 * o RSP tambem desenha o quadro anterior enquanto o jogo monta o seguinte. */
void Port_GfxBarrier(const void *pool, unsigned size) {
    OSTimer timer;
    OSMesg m;
    const uintptr_t a = (uintptr_t)pool;
    if (!__atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
        return;
    }
    if ((uintptr_t)g_def.dl - a >= (uintptr_t)size) {
        return; /* o adiado e de outra memoria */
    }
    if (!g_barInit) {
        osCreateMesgQueue(&g_barQ, g_barBuf, 8);
        g_barInit = 1;
    }
    while (osRecvMesg(&g_barQ, &m, OS_MESG_NOBLOCK) == 0) {
        /* avisos velhos */
    }
    __atomic_store_n(&g_barWaiting, 1, __ATOMIC_SEQ_CST);
    const uint64_t tb = hook_now_ns();
    /* rede de seguranca: 150 ms (o desenho adiado leva ~20-40 ms) */
    osSetTimer(&timer, (OSTime)7031250, 0, &g_barQ, (OSMesg)2);
    while (__atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
        osRecvMesg(&g_barQ, &m, OS_MESG_BLOCK);
        if (m == (OSMesg)2 && __atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
            static int warned;
            if (warned++ < 5) {
                fprintf(stderr, "[gfx] o desenho adiado nao terminou em 150 ms (marcados %u, comecados %u, terminados %u)\n",
                        g_def.nSet, g_def.nStart, g_def.nDone);
            }
            break;
        }
    }
    osStopTimer(&timer);
    __atomic_store_n(&g_barWaiting, 0, __ATOMIC_SEQ_CST);
    gfx_trace('B', 0, 0, (float)((double)(hook_now_ns() - tb) / 1e6), 0, 0, 0, 0);
}

/* teste no PC (--interp-dump PREFIXO,DESDE,QUANTOS): grava as imagens dos quadros intermediarios */
static struct {
    const char *prefix;
    int from, max, n;
} g_idump;

static void interp_dump_sink(const void *fb, const uint16_t *const *img, const int *vs, int n) {
    int k;
    (void)fb;
    for (k = 0; k < n; k++) {
        char path[512];
        int w = 320, h = 240, r[4];
        const uint8_t *tb;
        if (gx.taskNo < (uint32_t)g_idump.from || g_idump.n >= g_idump.max) {
            return;
        }
        snprintf(path, sizeof path, "%s_%05u_%d_vs%d.png", g_idump.prefix, gx.taskNo, k, vs[k]);
        g_idump.n++;
        gx_img_size(img[k], &w, &h);
        if (w == 320 && !gx_img_text(img[k], &tb, r)) {
            gx_dump_png((const uint8_t *)img[k], 320, 240, path);
        } else {
            /* como a tela mostra: a imagem ampliada para 640x480 (vizinho) e a caixa de texto por cima */
            static uint16_t *big;
            int x, y;
            if (big == NULL && (big = (uint16_t *)malloc(640 * 480 * 2)) == NULL) {
                return;
            }
            for (y = 0; y < 480; y++) {
                for (x = 0; x < 640; x++) {
                    big[y * 640 + x] = img[k][(y * h / 480) * w + x * w / 640];
                }
            }
            if (gx_img_text(img[k], &tb, r)) {
                for (y = r[1]; y < r[3]; y++) {
                    memcpy(big + y * 640 + r[0], tb + ((size_t)y * 640 + (size_t)r[0]) * 2u, (size_t)(r[2] - r[0]) * 2u);
                }
            }
            gx_dump_png((const uint8_t *)big, 640, 480, path);
        }
    }
}

void gfx_interp_dump_config(const char *spec) {
    static char pre[400];
    int from = 0, max = 30;
    const char *c = strchr(spec, ',');
    size_t l = c != NULL ? (size_t)(c - spec) : strlen(spec);
    if (l >= sizeof pre) {
        l = sizeof pre - 1;
    }
    memcpy(pre, spec, l);
    pre[l] = 0;
    if (c != NULL) {
        sscanf(c + 1, "%d,%d", &from, &max);
    }
    g_idump.prefix = pre;
    g_idump.from = from;
    g_idump.max = max;
    gx_interp_sink = interp_dump_sink;
}

static int interp_render(OSTask *t, volatile int *yield) {
    const void *tag = t->t.data_ptr, *fb = NULL;
    uint32_t dl = (uint32_t)(uintptr_t)tag;
    const uint16_t *img[2];
    int vs[2], cs, ps;
    if (!port_fi_enabled || gx_interp_sink == NULL) {
        return 0;
    }
    if (!port_fi_pair(tag, &cs, &ps)) {
        return 0;
    }
    if (g_repl == NULL) {
        int i;
        g_repl = (FiRepl *)calloc(1, sizeof *g_repl);
        for (i = 0; i < 6; i++) {
            {
                /* memoria que o chip 2D da tela le direto (video_miyoo.c), se houver; senao a comum */
                extern void *(*gx_img_alloc)(size_t bytes);
                extern size_t gx_hr_img_bytes(void); /* gfx_hires.c: 640x480, ou 752x560 na tela do Mini v4 / Flip */
                extern int gx_hr_max_opt, gx_hr_pan560, gx_retro;
                const size_t nb = gx_hr_max_opt > 320 || (gx_hr_pan560 && gx_retro) ? gx_hr_img_bytes() : 320 * 240 * 2 + 64;
                g_ibuf[i] = gx_img_alloc != NULL ? (uint16_t *)gx_img_alloc(nb) : NULL;
                if (g_ibuf[i] == NULL) {
                    g_ibuf[i] = (uint16_t *)malloc(nb); /* (+64: como buf_alloc) */
                }
            }
            if (g_ibuf[i] == NULL) {
                g_repl = NULL;
            }
        }
        if (g_repl == NULL) {
            port_fi_enabled = 0;
            return 0;
        }
    }
    if (port_opt_degraus && gx_interp_sink != interp_dump_sink && port_opt_extra_adiado) {
        /* degraus: escolhe n pelo tempo medio do desenho (com folga, como o limite de 31 ms dos 30 fps) */
        static const double lim[5] = { 31.0, 35.0, 37.5, 39.0, 40.0 }; /* n = 1..5 */
        const double P = g_passMs;
        int n = 1, isA;
        while (n <= 5 && P > lim[n - 1]) {
            n++;
        }
        /* n = 6: so ticks B (20 fps). Sobe na hora; desce com folga de 2 ms abaixo do limite do degrau
         * novo, ou quando o ultimo desenho ja e bem mais leve (a media demora a cair) */
        if (n > g_dgN) {
            g_dgN = n;
        } else if (n < g_dgN && (P < lim[n - 1] - 2.0 || g_lastMs < lim[n - 1] - 6.0)) {
            g_dgN = n;
        }
        isA = !g_dgLastA && g_dgN <= 5 && g_dgB >= g_dgN;
        g_ist.nA++;
        g_ist.nAfall += !isA;
        gfx_trace('T', isA, g_dgN, 0, 0, 0, 0, 0);
        g_dgLastA = isA;
        if (isA) {
            uint16_t *d;
            g_dgB = 0;
            img[0] = interp_pass(dl, yield, cs, ps, 1.0f / 3.0f, &fb);
            d = ibuf_take();
            g_def.dl = dl;
            g_def.dst = d;
            g_def.nSet++;
            __atomic_store_n(&g_def.pending, 1, __ATOMIC_SEQ_CST);
            __atomic_store_n(&gx_interp_notready, d, __ATOMIC_RELEASE);
            img[1] = d;
            vs[0] = 0 + FI_VS;
            vs[1] = 2 + FI_VS;
            gx_interp_sink(fb, img, vs, 2);
            return 1;
        }
        g_dgB++;
        img[0] = interp_pass(dl, yield, cs, ps, 2.0f / 3.0f, &fb);
        vs[0] = 1 + FI_VS;
        gx_interp_sink(fb, img, vs, 1);
        return 1;
    }
    g_itick ^= 1;
    {
        /* adiando, cada tick tem 1 desenho no caminho do jogo (o adiado cabe na espera da troca) */
        const int defer = port_opt_extra_adiado && gx_interp_sink != interp_dump_sink;
        const double lim = defer ? 31.0 : 23.0;
        if (g_itick) {
            /* a decisao vale para o par A+B (os dois ticks no mesmo modo) */
            if (!g_fall && g_passMs > lim) {
                g_fall = 1;
            } else if (g_fall && (g_passMs < lim - 2.0 || g_lastMs < lim - 6.0)) {
                /* (volta tambem quando o ultimo desenho ja e bem mais leve: a media demora ~4 desenhos para cair) */
                g_fall = 0;
            }
            g_ist.nA++;
            g_ist.nAfall += g_fall;
        }
        if (g_fall) {
            /* sem tempo: so o quadro do jogo, 2 vsyncs depois da troca (20 fps regulares) */
            img[0] = interp_pass(dl, yield, cs, ps, 1.0f, &fb);
            vs[0] = 2 + FI_VS;
            gx_interp_sink(fb, img, vs, 1);
            return 1;
        }
        if (g_itick) {
            /* tipo A: 1/3 agora; o quadro do jogo agora ou adiado */
            img[0] = interp_pass(dl, yield, cs, ps, 1.0f / 3.0f, &fb);
            if (defer) {
                uint16_t *d = ibuf_take();
                g_def.dl = dl;
                g_def.dst = d;
                g_def.nSet++;
                __atomic_store_n(&g_def.pending, 1, __ATOMIC_SEQ_CST);
                __atomic_store_n(&gx_interp_notready, d, __ATOMIC_RELEASE);
                img[1] = d;
            } else {
                img[1] = interp_pass(dl, yield, cs, ps, 1.0f, &fb);
            }
            vs[0] = 0 + FI_VS;
            vs[1] = 2 + FI_VS;
            gx_interp_sink(fb, img, vs, 2);
            return 1;
        }
    }
    img[0] = interp_pass(dl, yield, cs, ps, 2.0f / 3.0f, &fb);
    vs[0] = 1 + FI_VS;
    if (gx_interp_sink == interp_dump_sink) {
        /* teste no PC: tambem o quadro do jogo deste tick (vs 9), para conferir as posicoes */
        img[1] = interp_pass(dl, yield, cs, ps, 1.0f, &fb);
        vs[1] = 9;
        gx_interp_sink(fb, img, vs, 2);
        return 1;
    }
    gx_interp_sink(fb, img, vs, 1);
    return 1;
}

/* Tarefas graficas que o jogo mandou (graph.c) e que ja terminaram de desenhar aqui. */
static volatile unsigned g_gfxSent, g_gfxDone;

void Port_GfxSent(void) {
    __atomic_add_fetch(&g_gfxSent, 1, __ATOMIC_RELEASE);
}

/* Thread do jogo: espera TODO o desenho pendente acabar (tarefas mandadas, desenho adiado). O jogo chama
 * antes de escrever por cima de memoria que o ultimo quadro ainda desenha: ao abrir o menu de pausa ele
 * carrega os icones e o Link do menu em cima da memoria dos objetos da cena, e ao fechar recarrega os
 * objetos em cima dos icones. No N64 o RCP ja tinha acabado aquele quadro; aqui o desenho anda atras
 * (quadros intermediarios, desenho adiado, resolucao maior) e lia lixo: a piscada amarela e preta e os
 * "LOADBLOCK invalido" do log. */
void Port_GfxIdle(void) {
    OSTimer timer;
    OSMesg m;
    const uint64_t t0 = hook_now_ns();
    int waited = 0;
    if (getenv("OOT_SEM_ESPERA") != NULL) {
        return; /* teste: como era antes */
    }
    /* espera pelas filas do sistema do jogo (nunca dormindo direto): o escalonador do jogo precisa rodar
     * para entregar a tarefa ao desenho */
    if (!g_barInit) {
        osCreateMesgQueue(&g_barQ, g_barBuf, 8);
        g_barInit = 1;
    }
    for (;;) {
        unsigned sent = __atomic_load_n(&g_gfxSent, __ATOMIC_ACQUIRE);
        unsigned done = __atomic_load_n(&g_gfxDone, __ATOMIC_ACQUIRE);
        if ((int)(done - sent) >= 0 && !__atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
            break;
        }
        if (hook_now_ns() - t0 > 300000000ull) {
            fprintf(stderr, "[gfx] Port_GfxIdle: o desenho nao terminou em 300 ms (mandadas %u, feitas %u)\n", sent, done);
            break;
        }
        waited = 1;
        while (osRecvMesg(&g_barQ, &m, OS_MESG_NOBLOCK) == 0) {
            /* avisos velhos */
        }
        __atomic_store_n(&g_barWaiting, 1, __ATOMIC_SEQ_CST);
        /* (pode ter terminado entre a conferencia e o aviso ligado) */
        if ((int)(__atomic_load_n(&g_gfxDone, __ATOMIC_ACQUIRE) - sent) >= 0 &&
            !__atomic_load_n(&g_def.pending, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&g_barWaiting, 0, __ATOMIC_SEQ_CST);
            break;
        }
        osSetTimer(&timer, (OSTime)937500, 0, &g_barQ, (OSMesg)2); /* acorda a cada 20 ms para conferir */
        osRecvMesg(&g_barQ, &m, OS_MESG_BLOCK);
        osStopTimer(&timer);
        __atomic_store_n(&g_barWaiting, 0, __ATOMIC_SEQ_CST);
    }
    if (waited) {
        fprintf(stderr, "[gfx] o jogo esperou o desenho pendente: %.1f ms\n", (double)(hook_now_ns() - t0) / 1e6);
    }
}

static int hook_gfx_task(OSTask *t, int resume, volatile int *yield) {
    int i;
    (void)resume;
    if (capture_on_request(t)) {
        /* (ja desenhou) */
    } else {
        int k, cap = 0;
        uint32_t next = gx.taskNo + 1; /* gx_render incrementa */
        for (k = 0; k < g_cap.n; k++) {
            if (g_cap.at[k] == next) {
                cap = 1;
            }
        }
        if (cap && g_cap.prefix != NULL) {
            char path[512];
            snprintf(path, sizeof path, "%s_%05u.cap", g_cap.prefix, next);
            gx_capture_render((uint32_t)(uintptr_t)t->t.data_ptr, path);
        } else {
            uint64_t t0 = hook_now_ns(), dt;
            if (!port_opt_degraus) {
                gfx_trace('T', g_itick ^ 1, g_fall, 0, 0, 0, 0, 0);
            }
            if (!interp_render(t, yield)) {
                const uint64_t a0 = os_hw_stats()->audio_ns;
                gx_hr_pass_begin();
                gx_render((uint32_t)(uintptr_t)t->t.data_ptr, yield);
                { extern int gx_hr_interm; gx_hr_interm = 0; }
                gx_hr_pass_end((double)(hook_now_ns() - t0) / 1e6);
                gx_ge_attach(gx.cimgRaw); /* o 2D pelo acelerador desta imagem (gfx_ge.c) */
                PT_EV(PT_FRAME, 0);
                gfx_trace('P', 0, (int)gx.nTriDrawn, (float)((double)(hook_now_ns() - t0) / 1e6), 0, 0, 0,
                          (float)((double)(os_hw_stats()->audio_ns - a0) / 1e6));
                if (gx_interp_sink != NULL) {
                    gx_interp_sink(NULL, NULL, NULL, 0); /* quadro normal: esquece imagens intermediarias velhas */
                }
            }
            dt = hook_now_ns() - t0;
            static uint64_t firstNs;
            if (firstNs == 0) {
                firstNs = t0;
            }
            if (gx_slowcap_ms > 0 && g_slowCount < 8 && dt > (uint64_t)gx_slowcap_ms * 1000000ull &&
                t0 - firstNs >= (uint64_t)gx_slowcap_after * 1000000000ull &&
                t0 - g_slowLastNs > 3000000000ull) {
                g_slowReq = 1;
                g_slowCount++;
                g_slowLastNs = t0;
                fprintf(stderr, "[gfx] quadro lento: %.1f ms (o proximo sera gravado)\n", (double)dt / 1e6);
            }
        }
    }
    __atomic_add_fetch(&g_gfxDone, 1, __ATOMIC_RELEASE); /* (Port_GfxIdle) */
    if (__atomic_load_n(&g_barWaiting, __ATOMIC_SEQ_CST)) {
        os_async_send(&g_barQ, (OSMesg)1); /* acorda o jogo, se ele espera o desenho acabar */
    }
    g_tot.tasks++;
    g_tot.cmds += gx.nCmd;
    g_tot.tris += gx.nTri;
    g_tot.trisDrawn += gx.nTriDrawn;
    g_tot.culled += gx.nCull;
    g_tot.clipRej += gx.nClipRej;
    g_tot.verts += gx.nVtx;
    g_tot.rects += gx.nRect;
    g_tot.texLoads += gx.nTexLoad;
    g_tot.bad += gx.nBad;
    g_tot.ucode += (uint64_t)gx.ucodeSwitch;
    for (i = 0; i < 256; i++) {
        g_tot.hist[i] += gx.opHist[i];
    }
    return 1;
}

void gfx_install(void) {
    port_hooks.gfx_task = hook_gfx_task;
    port_hooks.gfx_post = hook_gfx_post;
    port_hooks.vi_present = hook_vi_present;
}

void gfx_print_stats(const char *tag) {
    gfx_trace_flush();
    gx_hr_print_stats();
    fprintf(stderr, "[%s gfx] tarefas=%llu cmds=%llu tris=%llu (desenhados %llu, fora da tela %llu, face %llu) vertices=%llu rects=%llu "
            "cargas_tex=%llu ruins=%llu ucode=%llu\n",
            tag, (unsigned long long)g_tot.tasks, (unsigned long long)g_tot.cmds, (unsigned long long)g_tot.tris,
            (unsigned long long)g_tot.trisDrawn, (unsigned long long)g_tot.clipRej, (unsigned long long)g_tot.culled,
            (unsigned long long)g_tot.verts, (unsigned long long)g_tot.rects,
            (unsigned long long)g_tot.texLoads, (unsigned long long)g_tot.bad, (unsigned long long)g_tot.ucode);
}

void gfx_print_histogram(void) {
    int i;
    fprintf(stderr, "[gfx] histograma de comandos (opcode: quantidade):\n");
    for (i = 0; i < 256; i++) {
        if (g_tot.hist[i] != 0) {
            fprintf(stderr, "  %02X: %llu\n", i, (unsigned long long)g_tot.hist[i]);
        }
    }
}
