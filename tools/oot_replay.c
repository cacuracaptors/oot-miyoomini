/*
 * oot_replay.c -- reproduz um quadro capturado (--capture do oot_headless) fora do jogo.
 *
 *   oot_replay QUADRO.cap [--out quadro.png] [--repeat N] [--fast] [--compare]
 *
 *   --fast     usa o caminho rapido dos spans
 *   --compare  renderiza com a referencia e com o rapido e compara cor e profundidade byte a byte
 *   --threads N  desenha com N threads (1 ou 2) no caminho rapido
 *   --prof ARQ amostra o contador de programa 2000 vezes por segundo durante as repeticoes e grava as
 *              amostras em ARQ (texto); tools/oot-prof-report.sh transforma em tabela por funcao/linha
 *
 * Mapeia a memoria em 0x80000000, restaura as paginas e o estado do RDP e renderiza a display
 * list do quadro. --repeat N mede o tempo medio (restaurando a memoria a cada vez).
 * Serve para medir o renderizador (no PC e no aparelho) e comparar versoes pixel a pixel.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <pthread.h>
#include <sched.h>

#include "gfx_priv.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000 /* cabecalhos antigos (kernels antigos tratam como dica) */
#endif

#define BASE 0x80000000u
#define SPAN (16u << 20)

typedef struct {
    uint32_t addr;
    uint8_t *data;
} Page;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- amostragem do contador de programa (--prof) ---- */
extern char __executable_start[], etext[];
static uintptr_t *g_samp;
static volatile int g_ns;
static int g_nmax;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = (ucontext_t *)ctx;
    uintptr_t pc = 0, lr = 0;
    (void)sig;
    (void)si;
#if defined(__arm__)
    pc = u->uc_mcontext.arm_pc;
    lr = u->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
    pc = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
    pc = (uintptr_t)u->uc_mcontext.gregs[REG_EIP];
#else
    (void)u;
#endif
    if (g_ns < g_nmax) {
        g_samp[2 * g_ns] = pc;
        g_samp[2 * g_ns + 1] = lr;
        g_ns++;
    }
}

static timer_t g_timer;

static int prof_start(void) {
    struct sigaction sa;
    struct sigevent ev;
    struct itimerspec it;
    g_nmax = 1 << 20;
    g_samp = (uintptr_t *)malloc(sizeof(uintptr_t) * 2u * (size_t)g_nmax);
    if (g_samp == NULL) {
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGPROF;
    if (timer_create(CLOCK_MONOTONIC, &ev, &g_timer) != 0) {
        return -1;
    }
    memset(&it, 0, sizeof it);
    it.it_interval.tv_nsec = 500000; /* 2000 amostras por segundo */
    it.it_value.tv_nsec = 500000;
    return timer_settime(g_timer, 0, &it, NULL);
}

static void prof_stop(const char *path) {
    struct itimerspec it;
    FILE *f;
    int i;
    memset(&it, 0, sizeof it);
    timer_settime(g_timer, 0, &it, NULL);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "nao consegui gravar %s\n", path);
        return;
    }
    /* inicio/fim do codigo do executavel neste processo (para descontar PIE e separar as bibliotecas) */
    fprintf(f, "base %lx %lx\n", (unsigned long)(uintptr_t)__executable_start, (unsigned long)(uintptr_t)etext);
    for (i = 0; i < g_ns; i++) {
        fprintf(f, "%lx %lx\n", (unsigned long)g_samp[2 * i], (unsigned long)g_samp[2 * i + 1]);
    }
    fclose(f);
    printf("perfil: %d amostras em %s\n", g_ns, path);
}

/* frequencia e governador da CPU (para saber se a medida foi em velocidade maxima) */
static void print_cpu(void) {
    char gov[64] = "?";
    long cur = 0, max = 0;
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (f) {
        if (fscanf(f, "%63s", gov) != 1) {
            strcpy(gov, "?");
        }
        fclose(f);
    }
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) {
        if (fscanf(f, "%ld", &cur) != 1) {
            cur = 0;
        }
        fclose(f);
    }
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "r");
    if (f) {
        if (fscanf(f, "%ld", &max) != 1) {
            max = 0;
        }
        fclose(f);
    }
    printf("cpu: governador=%s freq_atual=%ld MHz freq_max=%ld MHz\n", gov, cur / 1000, max / 1000);
}

/* --pmu: contadores do processador (ciclos, instrucoes, faltas de cache...) somados nas 2 threads */
#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define PMU_N 7
static const struct { uint32_t type; uint64_t cfg; const char *name; } k_pmu[PMU_N] = {
    { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "ciclos" },
    { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "instrucoes" },
    { PERF_TYPE_RAW, 0x03, "falta_L1d" },
    { PERF_TYPE_RAW, 0x17, "falta_L2" },
    { PERF_TYPE_RAW, 0x01, "falta_L1i" },
    { PERF_TYPE_RAW, 0x05, "falta_TLBd" },
    { PERF_TYPE_RAW, 0x10, "desvio_errado" },
};
static int g_pmu[PMU_N];
static void pmu_open(void) {
    int i;
    for (i = 0; i < PMU_N; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = k_pmu[i].type;
        a.config = k_pmu[i].cfg;
        a.disabled = 1;
        a.inherit = 1;
        a.exclude_kernel = 1;
        g_pmu[i] = (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
        if (g_pmu[i] < 0) {
            fprintf(stderr, "pmu: %s indisponivel\n", k_pmu[i].name);
        }
    }
    for (i = 0; i < PMU_N; i++) {
        if (g_pmu[i] >= 0) {
            ioctl(g_pmu[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(g_pmu[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    }
}
static void pmu_close(int repeat) {
    int i;
    uint64_t v[PMU_N];
    printf("pmu por quadro:");
    for (i = 0; i < PMU_N; i++) {
        v[i] = 0;
        if (g_pmu[i] >= 0) {
            ioctl(g_pmu[i], PERF_EVENT_IOC_DISABLE, 0);
            if (read(g_pmu[i], &v[i], 8) != 8) {
                v[i] = 0;
            }
            close(g_pmu[i]);
        }
        printf(" %s=%.0f", k_pmu[i].name, (double)v[i] / repeat);
    }
    if (v[0]) {
        printf(" | ipc=%.2f", (double)v[1] / (double)v[0]);
    }
    printf("\n");
}
#endif

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* --hd-teste K: cada textura ganha uma "nova" feita dela mesma, ampliada K vezes com o alinhamento de centros
 * (texel j da nova no ponto j/K - (K-1)/(2K) da original): testa a escala e o deslocamento das coordenadas
 * do caminho rapido para as texturas novas (gfx_hd.c). A imagem deve sair quase igual a normal. */
static int g_hdtest, g_hdtest_bias;
GxTex *gx_hd_lookup(GxTex *tx, const GxTile *tl, const uint8_t *src, unsigned stride) {
    GxTex *h;
    int K = g_hdtest, W, H, i, j, c;
    (void)tl;
    (void)src;
    (void)stride;
    if (K < 2 || tx->w * tx->h > 256 * 256) {
        return NULL;
    }
    W = tx->w * K;
    H = tx->h * K;
    h = (GxTex *)calloc(1, sizeof *h);
    h->px = (uint32_t *)malloc((size_t)W * H * 4);
    for (j = 0; j < H; j++) {
        float v = ((float)j + 0.5f) / K - 0.5f + g_hdtest_bias * 0.25f;
        int y0 = (int)floorf(v);
        float fy = v - y0;
        for (i = 0; i < W; i++) {
            float u = ((float)i + 0.5f) / K - 0.5f + g_hdtest_bias * 0.25f;
            int x0 = (int)floorf(u);
            float fx = u - x0;
            uint32_t out = 0;
            for (c = 0; c < 4; c++) {
                int xa = ((x0 % tx->w) + tx->w) % tx->w, xb = (((x0 + 1) % tx->w) + tx->w) % tx->w;
                int ya = ((y0 % tx->h) + tx->h) % tx->h, yb = (((y0 + 1) % tx->h) + tx->h) % tx->h;
                float p00 = (float)((tx->px[ya * tx->w + xa] >> (8 * c)) & 255), p10 = (float)((tx->px[ya * tx->w + xb] >> (8 * c)) & 255);
                float p01 = (float)((tx->px[yb * tx->w + xa] >> (8 * c)) & 255), p11 = (float)((tx->px[yb * tx->w + xb] >> (8 * c)) & 255);
                float vv = (p00 * (1 - fx) + p10 * fx) * (1 - fy) + (p01 * (1 - fx) + p11 * fx) * fy;
                out |= (uint32_t)(int)(vv + 0.5f) << (8 * c);
            }
            h->px[j * W + i] = out;
        }
    }
    h->w = W;
    h->h = H;
    h->clipw = tx->clipw * K;
    h->cliph = tx->cliph * K;
    h->key = tx->key;
    h->hdk = K;
    return h;
}
void gx_hd_free(GxTex *h) {
    free(h->px);
    free(h->pp);
    free(h->pf);
    free(h);
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL, *prof = NULL;
    int repeat = 1, i, r, fast = 0, compare = 0, nprobe = 0, probeXY[GX_PROBE_MAX][2], each = 0, pmu = 0;
    FILE *f;
    uint8_t hdr[24];
    uint32_t dl, ramEnd, stSize, npages;
    GxCapState st;
    Page *pages;
    void *mem;
    double best = 1e30, total = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fast")) {
            fast = 1;
        } else if (!strcmp(argv[i], "--compare")) {
            compare = 1;
        } else if (!strcmp(argv[i], "--antes")) {
            gx_kern = 0;    /* sem os kernels do combinador e sem o fundo adiado (para comparar) */
            gx_bgdefer = 0;
        } else if (!strcmp(argv[i], "--filtro-rapido")) {
            gx_tex_prefilter = 1; /* 2x2 */
        } else if (!strcmp(argv[i], "--filtro-rapido8")) {
            gx_tex_prefilter = 3; /* 8x8 */
        } else if (!strcmp(argv[i], "--filtro-rapido4")) {
            gx_tex_prefilter = 2; /* 4x4 */
        } else if (!strcmp(argv[i], "--ordenar")) {
            gx_sort_opaque = 1;
        } else if (!strcmp(argv[i], "--ordenar-lote") && i + 1 < argc) {
            extern int gx_sort_chunk;
            gx_sort_opaque = 1;
            gx_sort_chunk = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--sem-pld")) {
            extern int gx_pld;
            gx_pld = 0;
        } else if (!strcmp(argv[i], "--sem-kern")) {
            gx_kern = 0;
        } else if (!strcmp(argv[i], "--hd-teste") && i + 1 < argc) {
            extern int gx_hd_on;
            g_hdtest = atoi(argv[++i]);
            gx_hd_on = 1; /* anota a origem da TMEM, como no jogo com pacote */
        } else if (!strcmp(argv[i], "--filtro-novas")) {
            extern int gx_hd_filtro;
            gx_hd_filtro = 1;
        } else if (!strcmp(argv[i], "--filtro-perto") && i + 1 < argc) {
            extern int gx_hd_perto;
            gx_hd_perto = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--resolucao") && i + 1 < argc) {
            gx_hr_max = gx_hr_max_opt = atoi(argv[++i]);
            gx_hr_fixo = 1;
        } else if (!strcmp(argv[i], "--clip-cor-antigo")) {
            extern int gx_clip_cor;
            gx_clip_cor = 0;
        } else if (!strcmp(argv[i], "--hd-teste-erro")) {
            g_hdtest_bias = 1; /* desloca a nova 1/4 de texel: para ver que o teste percebe */
        } else if (!strcmp(argv[i], "--sem-roubo")) {
            extern int gx_steal;
            gx_steal = 0; /* a principal so espera a auxiliar no fim (como antes) */
        } else if (!strcmp(argv[i], "--com-meia")) {
            extern int gx_tex_meia;
            gx_tex_meia = 1; /* (teste do lote 116) */
        } else if (!strcmp(argv[i], "--sem-meia")) {
            extern int gx_tex_meia;
            gx_tex_meia = 0; /* texturas ampliadas amostradas em todo pixel (como antes do lote 116) */
        } else if (!strcmp(argv[i], "--dobra-comb") && i + 1 < argc) {
            extern int gx_dobra_comb;
            gx_dobra_comb = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--sem-dobra")) {
            extern int gx_cc_fold;
            gx_cc_fold = 0; /* combinador sem simplificar as constantes 0/255 (como antes do lote 115) */
        } else if (!strcmp(argv[i], "--sem-corte")) {
            extern int gx_corte;
            gx_corte = 0; /* desenha tambem a parte vazia das texturas transparentes (como antes do lote 125) */
        } else if (!strcmp(argv[i], "--sem-mistura")) {
            extern int gx_ceu_mistura;
            gx_ceu_mistura = 0; /* ceu com as 2 texturas por pixel (como antes do lote 121) */
        } else if (!strcmp(argv[i], "--com-aprox")) {
            extern int gx_cc_aprox;
            gx_cc_aprox = 1;
        } else if (!strcmp(argv[i], "--sem-memo")) {
            extern int gx_fmemo;
            gx_fmemo = 0; /* caminho fundido sem a memoria de programas montados */
        } else if (!strcmp(argv[i], "--sem-aprox")) {
            extern int gx_cc_aprox;
            gx_cc_aprox = 0; /* mistura com 255 como o N64 (as duas texturas) */
        } else if (!strcmp(argv[i], "--com-alfa-zero")) {
            extern int gx_a0_skip;
            gx_a0_skip = 1; /* (teste do lote 113) */
        } else if (!strcmp(argv[i], "--sem-alfa-zero")) {
            extern int gx_a0_skip;
            gx_a0_skip = 0; /* pixels de alfa 0 tambem misturados e gravados (como antes) */
        } else if (!strcmp(argv[i], "--sem-neblina-zero")) {
            extern int gx_fog_skip;
            gx_fog_skip = 0; /* trechos sem neblina tambem pela mistura da neblina (como antes) */
        } else if (!strcmp(argv[i], "--sem-spec")) {
            extern int gx_spanspec;
            gx_spanspec = 0; /* desenho dos trechos numa funcao so (como antes) */
        } else if (!strcmp(argv[i], "--sem-fundo-misto")) {
            extern int gx_bgmix;
            gx_bgmix = 0;
        } else if (!strcmp(argv[i], "--sem-mistura-desliza")) {
            extern int gx_mistura_desliza;
            gx_mistura_desliza = 0; /* texturas que deslizam misturadas por pixel (como antes do lote 183) */
        } else if (!strcmp(argv[i], "--sem-fundo")) {
            gx_bgdefer = 0;
        } else if (!strcmp(argv[i], "--nofused")) {
            gx_fused = 0; /* desliga o caminho fundido (para comparar) */
        } else if (!strcmp(argv[i], "--exact")) {
            gx_fast_exact = 1; /* caminho rapido sem as aproximacoes (igual a referencia bit a bit) */
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            gx_threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--probe") && i + 1 < argc) {
            /* --probe X,Y: mostra quem escreveu por ultimo naquele pixel (e como) */
            if (nprobe < GX_PROBE_MAX && sscanf(argv[++i], "%d,%d", &probeXY[nprobe][0], &probeXY[nprobe][1]) == 2) {
                nprobe++;
            }
        } else if (!strcmp(argv[i], "--pin")) {
            /* thread principal no nucleo 0, auxiliar no 1 */
            extern int gx_helper_cpu;
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(0, &cs);
            sched_setaffinity(0, sizeof cs, &cs);
            gx_helper_cpu = 1;
        } else if (!strcmp(argv[i], "--spin") && i + 1 < argc) {
            extern int gx_helper_spin_us;
            gx_helper_spin_us = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--rt") && i + 1 < argc) {
            /* desenho (e a auxiliar, que segue) em tempo real com esta prioridade, como no jogo */
            struct sched_param sp;
            memset(&sp, 0, sizeof sp);
            sp.sched_priority = atoi(argv[++i]);
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
                fprintf(stderr, "o Linux recusou o tempo real\n");
            }
        } else if (!strcmp(argv[i], "--pmu")) {
            pmu = 1;
        } else if (!strcmp(argv[i], "--each")) {
            each = 1;
        } else if (!strcmp(argv[i], "--prof") && i + 1 < argc) {
            prof = argv[++i];
        } else if (argv[i][0] != '-') {
            in = argv[i];
        } else {
            fprintf(stderr, "uso: %s QUADRO.cap [--out quadro.png] [--repeat N] [--fast] [--compare] [--exact] [--nofused] [--threads N] [--prof ARQ]\n", argv[0]);
            return 2;
        }
    }
    if (in == NULL) {
        fprintf(stderr, "uso: %s QUADRO.cap [--out quadro.png] [--repeat N] [--fast] [--compare] [--exact] [--nofused] [--threads N] [--prof ARQ]\n", argv[0]);
        return 2;
    }
    f = fopen(in, "rb");
    if (f == NULL || fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "OOTCAP1", 8) != 0) {
        fprintf(stderr, "arquivo de captura invalido: %s\n", in);
        return 1;
    }
    dl = rd32(hdr + 8);
    ramEnd = rd32(hdr + 12);
    stSize = rd32(hdr + 16);
    npages = rd32(hdr + 20);
    if (stSize != sizeof st || fread(&st, 1, sizeof st, f) != sizeof st) {
        fprintf(stderr, "captura de outra versao (estado %u bytes, esperado %u)\n", stSize, (unsigned)sizeof st);
        return 1;
    }
    mem = mmap((void *)(uintptr_t)BASE, SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
               -1, 0);
    if (mem == MAP_FAILED || (uintptr_t)mem != BASE) {
        fprintf(stderr, "nao consegui mapear a memoria em 0x80000000\n");
        return 1;
    }
    pages = (Page *)calloc(npages, sizeof *pages);
    for (i = 0; i < (int)npages; i++) {
        uint8_t a[4];
        if (fread(a, 1, 4, f) != 4) {
            fprintf(stderr, "captura truncada\n");
            return 1;
        }
        pages[i].addr = rd32(a);
        pages[i].data = (uint8_t *)malloc(4096);
        if (fread(pages[i].data, 1, 4096, f) != 4096 || pages[i].addr < BASE || pages[i].addr >= BASE + SPAN) {
            fprintf(stderr, "captura truncada ou pagina fora do lugar\n");
            return 1;
        }
    }
    fclose(f);
    gx_ram_end = ramEnd;
    if (gx_hr_max > 320) {
        /* resolucao maior: um desenho no tamanho do jogo descobre qual e a tela (o ultimo alvo) */
        const int keep = gx_hr_max;
        for (i = 0; i < (int)npages; i++) {
            memcpy((void *)(uintptr_t)pages[i].addr, pages[i].data, 4096);
        }
        gx_cap_load(&st);
        gx_hr_max = gx_hr_max_opt = 320;
        gx_hr_pass_begin();
        gx_render(dl, NULL);
        gx_hr_pass_end(0);
        gx_hr_max = gx_hr_max_opt = keep;
        gx_hr_screen(gx.cimgRaw);
    }
    if (compare) {
        print_cpu();
        /* referencia e rapido no mesmo quadro: cor e profundidade tem que ser identicas */
        static uint8_t refC[640 * 480 * 2], refZ[640 * 480 * 2];
        uint8_t *cimg, *zimg;
        double tr, tf;
        int dc = 0, dz = 0, first = -1, cw, chh;
        for (i = 0; i < (int)npages; i++) {
            memcpy((void *)(uintptr_t)pages[i].addr, pages[i].data, 4096);
        }
        gx_cap_load(&st);
        gx_fast = 0;
        gx_hr_pass_begin();
        tr = now_ms();
        gx_render(dl, NULL);
        tr = now_ms() - tr;
        cimg = gx.cimg.addr;
        zimg = gx.zimg;
        cw = gx.cimg.width;
        chh = gx.hrActive ? gx.hrH : 240;
        memcpy(refC, cimg, (size_t)cw * chh * 2u);
        if (zimg) {
            memcpy(refZ, zimg, (size_t)cw * chh * 2u);
        }
        for (i = 0; i < (int)npages; i++) {
            memcpy((void *)(uintptr_t)pages[i].addr, pages[i].data, 4096);
        }
        gx_cap_load(&st);
        gx_fast = 1;
        gx_hr_pass_begin();
        tf = now_ms();
        gx_render(dl, NULL);
        tf = now_ms() - tf;
        for (i = 0; i < cw * chh; i++) {
            if (memcmp(refC + i * 2, cimg + i * 2, 2) != 0) {
                if (first < 0) {
                    first = i;
                }
                dc++;
            }
            if (zimg && memcmp(refZ + i * 2, zimg + i * 2, 2) != 0) {
                dz++;
            }
        }
        printf("%s: comparacao referencia x rapido: pixels de cor diferentes=%d, profundidade diferentes=%d", in, dc, dz);
        if (first >= 0) {
            printf(" (primeiro em x=%d y=%d: ref %02x%02x rapido %02x%02x)", first % cw, first / cw, refC[first * 2],
                   refC[first * 2 + 1], cimg[first * 2], cimg[first * 2 + 1]);
        }
        printf("\ntempo: referencia %.2f ms, rapido %.2f ms\n", tr, tf);
        return dc || dz ? 3 : 0;
    }
    gx_fast = fast;
    print_cpu();
    if (prof != NULL && prof_start() != 0) {
        fprintf(stderr, "nao consegui ligar o temporizador do perfil\n");
        prof = NULL;
    }
    if (pmu) {
        /* 1 quadro de aquecimento fora da conta (cache de texturas, thread auxiliar) */
        for (i = 0; i < (int)npages; i++) {
            memcpy((void *)(uintptr_t)pages[i].addr, pages[i].data, 4096);
        }
        gx_cap_load(&st);
        gx_render(dl, NULL);
#ifdef __linux__
        pmu_open();
#endif
    }
    {
        extern uint64_t gx_raster_ns, gx_helper_ns, gx_wait_ns;
        gx_raster_ns = gx_helper_ns = gx_wait_ns = 0;
        { extern uint64_t gx_steal_ns; gx_steal_ns = 0; }
    }
    for (r = 0; r < repeat; r++) {
        double t0, dt;
        for (i = 0; i < (int)npages; i++) {
            memcpy((void *)(uintptr_t)pages[i].addr, pages[i].data, 4096);
        }
        gx_cap_load(&st);
        gx_hr_pass_begin();
        gx.nProbe = 0;
        for (i = 0; i < nprobe; i++) {
            memset(&gx.probe[i], 0, sizeof gx.probe[0]);
            gx.probe[i].x = probeXY[i][0];
            gx.probe[i].y = probeXY[i][1];
            gx.nProbe++;
        }
        t0 = now_ms();
        gx_render(dl, NULL);
        dt = now_ms() - t0;
        if (gx_hr_max > 320 && r + 1 == repeat) {
            gx_hr_pass_end(dt); /* (so no ultimo: a tela do N64 recebe a reducao) */
        }
        if (each) {
            printf("%.1f ", dt);
        }
        total += dt;
        if (dt < best) {
            best = dt;
        }
    }
    if (prof != NULL) {
        prof_stop(prof);
    }
    if (each) {
        printf("\n");
    }
#ifdef __linux__
    if (pmu) {
        pmu_close(repeat);
    }
#endif
    printf("%s: dl=%08x paginas=%u | cmds=%u tris=%u desenhados=%u rects=%u cargas_tex=%u ruins=%u\n", in, dl, npages,
           gx.nCmd, gx.nTri, gx.nTriDrawn, gx.nRect, gx.nTexLoad, gx.nBad);
    printf("tempo: melhor %.2f ms, media %.2f ms (%d vezes)\n", best, total / repeat, repeat);
    {
        /* divisao do trabalho entre as threads (media por quadro) */
        extern uint64_t gx_raster_ns, gx_helper_ns, gx_wait_ns;
        extern uint64_t gx_steal_ns;
        printf("threads: principal desenhando no fim %.2f ms, auxiliar ocupada %.2f ms, principal esperando %.2f ms"
               " (das quais pegando linhas da auxiliar %.2f ms)\n",
               gx_raster_ns / 1e6 / repeat, gx_helper_ns / 1e6 / repeat, gx_wait_ns / 1e6 / repeat, gx_steal_ns / 1e6 / repeat);
    }
    if (nprobe && gx.cimg.addr != NULL) {
        gx_probe_print(gx.cimg.addr);
    }
    if (out != NULL && gx.cimg.addr != NULL) {
        const uint16_t *hi;
        int hw, hh;
        if (gx_hr_image(gx.cimgRaw, &hi, &hw, &hh)) {
            if (gx_dump_png((const uint8_t *)hi, hw, hh, out) == 0) {
                printf("imagem: %s (%dx%d)\n", out, hw, hh);
            }
        } else if (gx_dump_png(gx.cimg.addr, 320, 240, out) == 0) {
            printf("imagem: %s (cimg=%p)\n", out, (void *)gx.cimg.addr);
        }
        {
            extern int gx_hr_text_of(const void *fbRaw, const uint8_t **buf, int rect[4]);
            const uint8_t *tb;
            int tr[4];
            if (gx_hr_text_of(gx.cimgRaw, &tb, tr)) {
                char p2[300];
                snprintf(p2, sizeof p2, "%s.texto.png", out);
                gx_dump_png(tb, 640, 480, p2);
                printf("texto: %s (%d,%d)-(%d,%d)\n", p2, tr[0], tr[1], tr[2], tr[3]);
            }
        }
    }
    return 0;
}
