/*
 * port_main.c -- programa principal do port.
 *
 * No PC (teste "sem tela"):
 *   oot_headless --rom ROM.z64 [--cart oot.cart] [--swapmap oot_swapmap.bin] [--sram oot.sav] [--seconds N] [--log]
 *                [--wav som.wav]
 *   Roda o jogo de verdade (bootproc) sem imagem nem controles, por N segundos. Serve para
 *   ver ate onde o jogo chega, achar travadas/quedas e (com --wav) gravar o som.
 *
 * No Miyoo Mini Plus (TARGET_MIYOO): o jogo com tela, botoes e som (port/miyoo/). Roda ate
 *   o MENU ser apertado. Na pasta do jogo: oot.cart (ou a ROM, para criar o cartucho na
 *   primeira vez), oot_swapmap.bin e saves/. Opcoes extras: -perf -mudo -semgfx.
 *
 * Se o cartucho virtual nao existir (ou for de versao antiga), cria a partir da ROM.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>

/* mensagens das telas do port: ingles, ou portugues com language=1 (antes de ler o options.txt: ingles) */
extern int port_opt_idioma;
#define TR(en, pt) (port_opt_idioma == 1 ? (pt) : (en))

#include "perf_trace.h"
#include "cart_build.h"
#ifdef TARGET_MIYOO
#include "miyoo/miyoo.h"
#endif
#include "gfx/gfx_priv.h"
#include "os_hw.h"

/* gfx_hook.c */
void gfx_install(void);
void gfx_dump_config(const char* prefix, int every, int max);
void gfx_interp_dump_config(const char* spec);
void gfx_capture_config(const char* prefix, const char* list);
void gfx_print_stats(const char* tag);
void gfx_print_histogram(void);

extern void bootproc(void);
void audio_out_install(const char* wav_path, void (*sink)(const int16_t*, uint32_t, uint32_t));
void jpeg_hle_install(void); /* jpeg_hle.c: fundos pre-renderizados (JPEG) */
void port_options_load(const char* path); /* port_options.c: opcoes.txt */
void port_lang_init(void);                /* lang.c: pacote de idioma (opcao idioma) */
extern void port_stubs_init(void);
extern char _end[]; /* fim do executavel (o linker define); o BSS do jogo (z-buffer etc.) termina aqui */
extern size_t gDmaMgrDmaBuffSize;

/* --warp: entra direto num lugar (src/code/title_setup.c, ajustado pelo oot-patch.py) */
int port_warp_entrance = -1, port_warp_age = 1 /* crianca */, port_warp_cs = 0;

static int g_seconds = 10;
static int g_perf;
static int g_trace_n, g_trace_after = -1;

/* ---- controle "roteirizado" (so para testes sem tela): --script "8:START:0.3,12:A+DUP:0.2,15:RIGHT:1.5"
 *      cada item = segundo_de_inicio:botoes_separados_por_+:duracao.  RIGHT/LEFT/UP/DOWN mexem o analogico. */
typedef struct {
    double t, dur;
    unsigned short btn;
    int sx, sy;
} ScriptEv;
static ScriptEv g_ev[256];
static int g_nev;
static struct timespec g_t0;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - g_t0.tv_sec) + (double)(ts.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void script_pads(OSContPad pads[4]) {
    double t = now_s();
    int i;
    memset(pads, 0, 4 * sizeof(OSContPad));
    for (i = 0; i < g_nev; i++) {
        if (t >= g_ev[i].t && t < g_ev[i].t + g_ev[i].dur) {
            pads[0].button |= g_ev[i].btn;
            if (g_ev[i].sx) {
                pads[0].stick_x = (s8)g_ev[i].sx;
            }
            if (g_ev[i].sy) {
                pads[0].stick_y = (s8)g_ev[i].sy;
            }
        }
    }
}

static int parse_script(const char* text) {
    static const struct {
        const char* n;
        unsigned short b;
        int sx, sy;
    } names[] = {
        { "A", PORT_BTN_A, 0, 0 },         { "B", PORT_BTN_B, 0, 0 },           { "Z", PORT_BTN_Z, 0, 0 },
        { "START", PORT_BTN_START, 0, 0 }, { "DUP", PORT_BTN_DUP, 0, 0 },       { "DDOWN", PORT_BTN_DDOWN, 0, 0 },
        { "DLEFT", PORT_BTN_DLEFT, 0, 0 }, { "DRIGHT", PORT_BTN_DRIGHT, 0, 0 }, { "L", PORT_BTN_L, 0, 0 },
        { "R", PORT_BTN_R, 0, 0 },         { "CUP", PORT_BTN_CUP, 0, 0 },       { "CDOWN", PORT_BTN_CDOWN, 0, 0 },
        { "CLEFT", PORT_BTN_CLEFT, 0, 0 }, { "CRIGHT", PORT_BTN_CRIGHT, 0, 0 }, { "RIGHT", 0, 70, 0 },
        { "LEFT", 0, -70, 0 },             { "UP", 0, 0, 70 },                  { "DOWN", 0, 0, -70 },
    };
    char buf[4096], *item, *save1 = NULL;
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (item = strtok_r(buf, ",", &save1); item != NULL && g_nev < 256; item = strtok_r(NULL, ",", &save1)) {
        char* c1 = strchr(item, ':');
        char* c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        char *btns, *tok, *save2 = NULL;
        ScriptEv ev;
        if (c1 == NULL || c2 == NULL) {
            fprintf(stderr, "roteiro invalido perto de '%s' (use segundo:BOTOES:duracao)\n", item);
            return -1;
        }
        *c1 = 0;
        *c2 = 0;
        memset(&ev, 0, sizeof ev);
        ev.t = atof(item);
        ev.dur = atof(c2 + 1);
        btns = c1 + 1;
        for (tok = strtok_r(btns, "+", &save2); tok != NULL; tok = strtok_r(NULL, "+", &save2)) {
            size_t k;
            int found = 0;
            for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                if (!strcmp(tok, names[k].n)) {
                    ev.btn |= names[k].b;
                    if (names[k].sx) {
                        ev.sx = names[k].sx;
                    }
                    if (names[k].sy) {
                        ev.sy = names[k].sy;
                    }
                    found = 1;
                }
            }
            if (!found) {
                fprintf(stderr, "botao desconhecido no roteiro: '%s'\n", tok);
                return -1;
            }
        }
        g_ev[g_nev++] = ev;
    }
    return 0;
}

/* Traduz enderecos em nomes de funcao usando o addr2line (so para diagnostico). */
static void symbolize(void** bt, int n) {
    char exe[512], cmd[4096];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    int i, len;
    FILE* p;
    char line[512];
    if (l <= 0) {
        return;
    }
    exe[l] = 0;
    len = snprintf(cmd, sizeof(cmd), "addr2line -f -C -p -e '%s'", exe);
    for (i = 0; i < n && len < (int)sizeof(cmd) - 32; i++) {
        len += snprintf(cmd + len, sizeof(cmd) - len, " %p", bt[i]);
    }
    p = popen(cmd, "r");
    if (p == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), p) != NULL) {
        if (strstr(line, "?? ") == NULL) { /* so as linhas do executavel */
            dprintf(2, "    %s", line);
        }
    }
    pclose(p);
}

static void on_fault(int sig, siginfo_t* si, void* uc) {
    void* bt[40];
    int n;
    (void)uc;
    dprintf(2, "\n*** FALHA: sinal %d, endereco %p ***\n", sig, si->si_addr);
#if defined(__arm__)
    {
        /* no aparelho o backtrace() nao funciona: registradores e a pilha crua, para traduzir no PC com
         * arm-linux-gnueabihf-addr2line -f -e oot_headless ENDERECOS */
        const ucontext_t* u = (const ucontext_t*)uc;
        const unsigned long* sp = (const unsigned long*)u->uc_mcontext.arm_sp;
        extern char __executable_start[], etext[];
        int k, shown = 0;
        dprintf(2, "pc=%08lx lr=%08lx sp=%08lx\nprovaveis retornos na pilha:", u->uc_mcontext.arm_pc,
                u->uc_mcontext.arm_lr, u->uc_mcontext.arm_sp);
        for (k = 0; k < 512 && shown < 24; k++) {
            unsigned long v = sp[k];
            if (v >= (unsigned long)__executable_start && v < (unsigned long)etext) {
                dprintf(2, " %08lx", v);
                shown++;
            }
        }
        dprintf(2, "\n");
    }
#endif
    n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, 2);
    symbolize(bt, n);
    dprintf(2, "--- threads do jogo ---\n");
    os_dump_threads(0);
#ifdef TARGET_MIYOO
    {
        char msg[160];
        snprintf(msg, sizeof msg, TR("The game crashed (signal %d).\nDetails in log.txt.\n\nClosing in 8 seconds...", "O jogo travou (sinal %d).\nDetalhes em log.txt.\n\nSaindo em 8 segundos..."), sig);
        void port_ss_release(void);
        port_ss_release();
        miyoo_rumble_off();
        miyoo_audio_shutdown();
        miyoo_video_message(msg, 0xFF6060);
        miyoo_battery_release();
        sleep(8);
        miyoo_video_shutdown();
    }
#endif
    _exit(139);
}

/* SIGUSR2: o diagnostico de travada manda este sinal a cada thread do jogo */
static void on_dump(int sig) {
    void* bt[30];
    int n = backtrace(bt, 30);
    (void)sig;
    dprintf(2, "    onde esta parada:\n");
    symbolize(bt, n);
}

static void install_handlers(void) {
    static char altstack[65536];
    stack_t ss;
    struct sigaction sa;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    signal(SIGUSR2, on_dump);
}

#ifdef TARGET_MIYOO
/* SIGTERM (desligar no OnionOS): port/savestate.c salva o estado e fecha no proximo quadro */
static void on_term(int sig) {
    int port_ss_take_sigterm(void);
    void port_ss_release(void);
    if (port_ss_take_sigterm()) {
        return;
    }
    port_ss_release();
    miyoo_rumble_off();
    miyoo_battery_release();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void audio_hle_report(char* buf, int size); /* audio/audio_hle.c */
static void print_stats(const char* tag) {
    const OsHwStats* s = os_hw_stats();
    char ab[700];
    audio_hle_report(ab, sizeof ab);
    fprintf(stderr, "[%s som] %s\n", tag, ab);
    fprintf(stderr,
            "[%s] retraces=%u presents=%u dma=%u (%llu KB) gfx=%u (yields %u) audio=%u jpeg=%u outras=%u "
            "gfx_ms=%llu\n",
            tag, s->retraces, s->presents, s->dma_reads, (unsigned long long)(s->dma_bytes / 1024), s->gfx_tasks,
            s->gfx_yields, s->audio_tasks, s->jpeg_tasks, s->other_tasks, (unsigned long long)(s->gfx_ns / 1000000));
    {
        /* para onde vai o tempo: valores do ultimo intervalo, em ms por segundo */
        extern uint64_t os_idle_ns, gx_raster_ns, gx_helper_ns, gx_wait_ns, os_max_run_ns;
        extern int os_max_run_tid;
        extern uint32_t os_msg_drops;
        static uint64_t p[8], pa;
        uint64_t v[8];
        struct timespec ts;
        double dt;
        int i;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        v[0] = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        v[1] = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        v[2] = os_idle_ns;
        v[3] = s->gfx_ns;
        v[4] = s->gfx_cpu_ns;
        v[5] = gx_raster_ns;
        v[6] = gx_helper_ns;
        v[7] = gx_wait_ns;
        if (p[0] != 0) {
            dt = (double)(v[0] - p[0]) / 1e9;
#define MS(k) ((double)(v[k] - p[k]) / 1e6 / dt)
            fprintf(stderr,
                    "[%s cpu] ms por segundo: processo %.0f (de 2000, 2 nucleos) | jogo ocupado %.0f | desenho %.0f "
                    "(cpu %.0f; rasterizacao %.0f, ajudante %.0f, esperando o ajudante %.0f; resto = vertices e comandos) "
                    "| som %.0f | maior trecho do jogo sem trocar %.1f ms (thread %d) | mensagens perdidas %u\n",
                    tag, MS(1), 1000.0 - MS(2), MS(3), MS(4), MS(5), MS(6), MS(7),
                    (double)(s->audio_ns - pa) / 1e6 / dt, (double)os_max_run_ns / 1e6, os_max_run_tid, os_msg_drops);
            os_max_run_ns = 0;
            os_msg_drops = 0;
            {
                /* troca de vez entre as threads do jogo e filas que perderam mensagens (nome pelo simbolo) */
                char buf[600];
                int len = snprintf(buf, sizeof buf, "[%s troca] demora para a thread rodar: media %.2f ms, pior %.1f ms (%u trocas) | perdas:",
                                   tag, os_wake_n ? (double)os_wake_sum_ns / os_wake_n / 1e6 : 0.0,
                                   (double)os_wake_max_ns / 1e6, os_wake_n);
                int q;
                for (q = 0; q < 8 && os_drop_mq[q] != NULL && len < (int)sizeof buf - 80; q++) {
                    Dl_info di;
                    const OSMesgQueue* mq = (const OSMesgQueue*)os_drop_mq[q];
                    if (dladdr(os_drop_mq[q], &di) && di.dli_sname != NULL) {
                        len += snprintf(buf + len, sizeof buf - len, " %s+%ld(tam %d)=%u", di.dli_sname,
                                        (long)((char*)os_drop_mq[q] - (char*)di.dli_saddr), (int)mq->msgCount, os_drop_cnt[q]);
                    } else {
                        len += snprintf(buf + len, sizeof buf - len, " %p(tam %d)=%u", os_drop_mq[q], (int)mq->msgCount,
                                        os_drop_cnt[q]);
                    }
                    os_drop_cnt[q] = 0;
                }
                fprintf(stderr, "%s\n", buf);
                os_wake_max_ns = os_wake_sum_ns = 0;
                os_wake_n = 0;
            }
#undef MS
        }
        pa = s->audio_ns;
        for (i = 0; i < 8; i++) {
            p[i] = v[i];
        }
    }
}

/* clock da CPU medido: um laco de 1 instrucao por ciclo (subs+bne) durante uns 20 ms */
/* MHz do PLL da CPU (SigmaStar SSD202, como o "cpuclock" do OnionOS sem argumentos); 0 se nao der para ler */
static int pll_mhz(void) {
#ifdef TARGET_MIYOO
    static volatile uint16_t* p16;
    uint32_t lpf;
    unsigned pd;
    if (p16 == NULL) {
        int fd = open("/dev/mem", O_RDONLY | O_SYNC);
        void* m;
        if (fd < 0) {
            return 0;
        }
        m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1F206000);
        close(fd);
        if (m == MAP_FAILED) {
            return 0;
        }
        p16 = (volatile uint16_t*)m;
    }
    lpf = (uint32_t)p16[0x2A4] | ((uint32_t)p16[0x2A6] << 16);
    pd = (p16[0x232] & 0x0F) + 1;
    if (lpf == 0) {
        return 0;
    }
    return (int)((432000000ull * 524288ull / lpf * 2 / pd * 16) / 1000000ull);
#else
    return 0;
#endif
}

static int cpu_mhz(void) {
#if defined(__arm__)
    struct timespec a, b;
    unsigned n = 20000000u;
    double s;
    clock_gettime(CLOCK_MONOTONIC, &a);
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b\n" : "+r"(n) : : "cc");
    clock_gettime(CLOCK_MONOTONIC, &b);
    s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
    return s > 0 ? (int)(20.0 / s + 0.5) : 0;
#else
    return 0;
#endif
}

static void* stats_thread(void* arg) {
    int t, lastRetraces = -1, stalled = 0;
    (void)arg;
    for (t = 1; g_seconds == 0 || t <= g_seconds; t++) {
        sleep(1);
        if (g_seconds == 0 && (t % 10) != 0 && !g_perf) {
            continue; /* jogando: estatisticas a cada 10 s no log */
        }
        if (g_trace_after >= 0 && t == g_trace_after) {
            gx.shownV = gx.shownM = gx.shownT = gx.shownP = 0;
            gx.traceArmed = 1;
            gx.traceN = g_trace_n;
        }
        print_stats("t");
        gfx_print_stats("t");
        if ((t % 10) == 0) {
            /* guarda do clock (opcao clock_guard): o cpuclock do OnionOS programa o PLL da CPU direto; se algo do
             * sistema mexer no cpufreq depois, o PLL volta para 1200. Le o PLL e, se caiu, pede o clock de novo */
            extern int port_opt_clock, port_opt_clock_guard;
            int pll = pll_mhz();
            if (port_opt_clock_guard && pll > 0 && port_opt_clock >= 400 && port_opt_clock <= 1800 &&
                pll < port_opt_clock - 60) {
                char cmd[96];
                int rc;
                snprintf(cmd, sizeof cmd, "/mnt/SDCARD/.tmp_update/bin/cpuclock %d >/dev/null 2>&1", port_opt_clock);
                rc = system(cmd);
                fprintf(stderr, "[clock] estava em %d MHz: pedi %d de novo (%d); PLL agora %d MHz\n", pll, port_opt_clock,
                        rc, pll_mhz());
            }
        }
        if ((int)os_hw_stats()->retraces == lastRetraces) {
            ++stalled;
            if (stalled == 3 || stalled == 10) {
                fprintf(stderr, "[AVISO] o video parou de avancar ha %d s (jogo travado ou esperando algo)\n", stalled);
                os_dump_threads(SIGUSR2);
            }
        } else {
            stalled = 0;
        }
        lastRetraces = (int)os_hw_stats()->retraces;
    }
    print_stats("fim");
    gfx_print_stats("fim");
    gfx_print_histogram();
    fprintf(stderr, "Fim do teste (%d s).\n", g_seconds);
    _exit(0);
    return NULL;
}

static void cart_progress(void* ud, const char* stage, int pct) {
    (void)ud;
    static int last = -1;
    if (pct == last) {
        return;
    }
    last = pct;
    fprintf(stderr, "\r  %-34s %3d%%", stage, pct);
    if (pct >= 100) {
        fputc('\n', stderr);
    }
#ifdef TARGET_MIYOO
    {
        char msg[256];
        snprintf(msg, sizeof msg, TR("First run: building the cartridge\nfrom the ROM (a few minutes)\n\n%s  %d%%", "Primeira vez: criando o cartucho\na partir da ROM (alguns minutos)\n\n%s  %d%%"), stage, pct);
        miyoo_video_message(msg, 0xFFFFFF);
    }
#endif
}

/* Para de vez com uma mensagem (na tela do aparelho tambem). */
static int fatal(const char* msg) {
    fprintf(stderr, "ERRO: %s\n", msg);
#ifdef TARGET_MIYOO
    {
        char buf[600];
        snprintf(buf, sizeof buf, TR("%s\n\nClosing in 15 seconds...", "%s\n\nSaindo em 15 segundos..."), msg);
        miyoo_video_message(buf, 0xFF6060);
        sleep(15);
        miyoo_video_shutdown();
        miyoo_battery_release();
    }
#endif
    return 1;
}

#ifdef TARGET_MIYOO
void miyoo_quit(int code) {
    static volatile int quitting;
    if (__atomic_exchange_n(&quitting, 1, __ATOMIC_SEQ_CST)) {
        return;
    }
    pt_dump(); /* (opcao perf_trace: o que estiver no anel) */
    {
        void port_ss_release(void);
        port_ss_release(); /* (o script que faz o OnionOS esperar o jogo salvar) */
    }
    miyoo_rumble_off();
    miyoo_audio_shutdown();
    miyoo_video_shutdown();
    miyoo_battery_release();
    fprintf(stderr, "Fim.\n");
    _exit(code);
}

/* menu de opcoes: reinicia o jogo para valer uma opcao que so e lida ao abrir (o estado ja foi salvo; o jogo continua
 * dele). O mesmo programa, com os mesmos argumentos, no lugar deste (o OnionOS nem percebe). */
static char** g_argv;
static char g_exePath[512]; /* o caminho do programa, lido ao abrir (depois de uma atualizacao, /proc/self/exe aponta
                             * para o programa antigo, que foi trocado: update_miyoo.c) */

void miyoo_restart(void) {
    pt_dump();
    {
        void port_ss_release(void);
        port_ss_release();
    }
    miyoo_rumble_off();
    miyoo_audio_shutdown();
    miyoo_video_shutdown();
    miyoo_battery_release();
    fprintf(stderr, "Reiniciando...\n");
    fflush(stderr);
    if (g_argv != NULL) {
        sigset_t none;
        sigemptyset(&none);
        pthread_sigmask(SIG_SETMASK, &none, NULL); /* (o programa novo herdaria os sinais bloqueados desta thread) */
        /* O programa novo tambem herda a classe desta thread (a do jogo: tempo real, SCHED_FIFO), o nice e os
         * nucleos permitidos. A thread principal dele cria as outras, e as que nao dizem a sua classe ficariam em
         * tempo real (o carregador de texturas, nice 19, passaria na frente do desenho): de volta ao comum. */
        {
            struct sched_param sp;
            unsigned long all = ~0ul;
            memset(&sp, 0, sizeof sp);
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
            setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 0);
            syscall(SYS_sched_setaffinity, 0, sizeof all, &all);
        }
        {
            extern int port_update_installed __attribute__((weak));
            if (&port_update_installed != NULL && port_update_installed && g_exePath[0]) {
                fprintf(stderr, "abrindo a versao nova: %s\n", g_exePath);
                fflush(stderr);
                execv(g_exePath, g_argv);
                perror("execv (versao nova)");
            }
        }
        execv("/proc/self/exe", g_argv);
        perror("execv");
    }
    _exit(0);
}

/* sem --rom: procura a ROM do jogador na pasta do jogo */
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#define ROM_TMP "oot_rom_temp.z64"
static const char* find_rom(void) {
    /* a americana (a europeia tambem pode estar na pasta: lang_pal.c tira dela o alemao e o frances); um .zip com a
     * americana dentro tambem serve (extraida para ROM_TMP, apagada depois de criar o cartucho) */
    int port_rom_is_usa(const char* path);
    int port_rom_zip_extract_usa(const char* zip, const char* out);
    static char name[512];
    DIR* d = opendir(".");
    struct dirent* e;
    int pass;
    if (d == NULL) {
        return NULL;
    }
    for (pass = 0; pass < 3; pass++) {
        rewinddir(d);
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            const int isRom = n > 4 && (!strcasecmp(e->d_name + n - 4, ".z64") || !strcasecmp(e->d_name + n - 4, ".n64") ||
                                        !strcasecmp(e->d_name + n - 4, ".v64"));
            const int isZip = n > 4 && !strcasecmp(e->d_name + n - 4, ".zip");
            if (pass == 0 && isRom && port_rom_is_usa(e->d_name)) {
                snprintf(name, sizeof name, "%s", e->d_name);
                closedir(d);
                return name;
            }
            if (pass == 1 && isZip) {
                miyoo_video_message(TR("First run: reading the ROM\nfrom the .zip...", "Primeira vez: lendo a ROM\ndo .zip..."), 0xFFFFFF);
                if (port_rom_zip_extract_usa(e->d_name, ROM_TMP) == 0) {
                    fprintf(stderr, "ROM americana extraida de %s\n", e->d_name);
                    snprintf(name, sizeof name, "%s", ROM_TMP);
                    closedir(d);
                    return name;
                }
            }
            if (pass == 2 && isRom) { /* (nao e a americana: o cart_build explica o problema na tela) */
                snprintf(name, sizeof name, "%s", e->d_name);
                closedir(d);
                return name;
            }
        }
    }
    closedir(d);
    return NULL;
}

/* idiomas da ROM europeia (lang_pal.c): mensagem na tela enquanto monta */
static void lang_progress(void* ud, const char* stage, int pct) {
    char msg[256];
    (void)ud;
    snprintf(msg, sizeof msg,
             TR("European ROM found: extracting\nGerman and French (once)\n\n%s  %d%%",
                "ROM europeia achada: extraindo\nalemão e francês (só uma vez)\n\n%s  %d%%"),
             stage, pct);
    miyoo_video_message(msg, 0xFFFFFF);
}
#endif

int main(int argc, char** argv) {
    const char* rom = NULL;
    const char* cart = "oot.cart";
    const char* sram = "oot.sav";
    const char* swapmap = "oot_swapmap.bin";
    pthread_t th;
    const char* dumpPrefix = NULL;
    const char* interpDump = NULL;
    int dumpEvery = 30, dumpMax = 20;
    const char* capPrefix = NULL;
    const char* capList = NULL;
    const char* wav = NULL;
    int mute = 0, cpuGame = -1, cpuGfx = -1, cpuAudio = -1;
    int i;

    gx.traceSeg = -1;
    gx_fast = 1; /* caminho rapido dos spans (identico a referencia bit a bit); --gfx-ref desliga */
#ifdef TARGET_MIYOO
    g_seconds = 0;   /* joga ate o MENU */
    gx_threads = 2;  /* os dois nucleos desenham */
    sram = "saves/oot.sav";
    mkdir("saves", 0755);
    g_argv = argv;
    {
        const ssize_t n = readlink("/proc/self/exe", g_exePath, sizeof g_exePath - 1);
        g_exePath[n > 0 ? n : 0] = 0;
    }
#endif
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == 0) {
            continue; /* (o lancador pode passar Arguments vazio) */
        }
#ifdef TARGET_MIYOO
        if (argv[i][0] == '-' && argv[i][1] != '-' && strcmp(argv[i], "-mudo") && strcmp(argv[i], "-perf") &&
            strcmp(argv[i], "-semgfx")) {
            fprintf(stderr, "opcao desconhecida ignorada: %s\n", argv[i]);
            continue;
        }
#endif
        if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = argv[++i];
        } else if (!strcmp(argv[i], "--mudo") || !strcmp(argv[i], "-mudo")) {
            mute = 1;
        } else if (!strcmp(argv[i], "--perf") || !strcmp(argv[i], "-perf")) {
            g_perf = 1;
#ifdef TARGET_MIYOO
            miyoo_opt_perf = 1;
#endif
        } else if (!strcmp(argv[i], "--semgfx") || !strcmp(argv[i], "-semgfx")) {
#ifdef TARGET_MIYOO
            miyoo_opt_nogfx = 1;
#endif
        } else if (!strcmp(argv[i], "--cpus") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d", &cpuGame, &cpuGfx, &cpuAudio) != 3) {
                fprintf(stderr, "--cpus espera JOGO,DESENHO,SOM (numeros de nucleo, -1 = livre)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--warp") && i + 1 < argc) {
            port_warp_entrance = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--adulto")) {
            port_warp_age = 0;
        } else if (!strcmp(argv[i], "--cena") && i + 1 < argc) {
            port_warp_cs = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--rom") && i + 1 < argc) {
            rom = argv[++i];
        } else if (!strcmp(argv[i], "--cart") && i + 1 < argc) {
            cart = argv[++i];
        } else if (!strcmp(argv[i], "--swapmap") && i + 1 < argc) {
            swapmap = argv[++i];
        } else if (!strcmp(argv[i], "--sram") && i + 1 < argc) {
            sram = argv[++i];
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            g_seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--log")) {
            os_debug_log = 1;
        } else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) {
            dumpPrefix = argv[++i];
        } else if (!strcmp(argv[i], "--interp-dump") && i + 1 < argc) {
            interpDump = argv[++i]; /* PREFIXO,DESDE_O_QUADRO,QUANTAS: imagens dos quadros intermediarios */
        } else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) {
            dumpEvery = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dump-max") && i + 1 < argc) {
            dumpMax = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            if (parse_script(argv[++i]) != 0) {
                return 2;
            }
        } else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            g_trace_n = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--capture") && i + 1 < argc) {
            capPrefix = argv[++i];
        } else if (!strcmp(argv[i], "--capture-at") && i + 1 < argc) {
            capList = argv[++i];
        } else if (!strcmp(argv[i], "--trace-seg") && i + 1 < argc) {
            gx.traceSeg = (int)strtol(argv[++i], NULL, 16);
        } else if (!strcmp(argv[i], "--trace-after") && i + 1 < argc) {
            g_trace_after = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--probe") && i + 1 < argc) {
            int px, py;
            i++;
            if (sscanf(argv[i], "%d,%d", &px, &py) == 2 && gx.nProbe < GX_PROBE_MAX) {
                memset(&gx.probe[gx.nProbe], 0, sizeof gx.probe[0]);
                gx.probe[gx.nProbe].x = px;
                gx.probe[gx.nProbe].y = py;
                gx.nProbe++;
            } else {
                fprintf(stderr, "--probe espera X,Y (ate %d sondas)\n", GX_PROBE_MAX);
                return 2;
            }
        } else if (!strcmp(argv[i], "--vp-flip")) {
            gx.vpFlip = 1;
        } else if (!strcmp(argv[i], "--cull-flip")) {
            gx.cullFlip = 1;
        } else if (!strcmp(argv[i], "--gfx-ref")) {
            gx_fast = 0;
        } else if (!strcmp(argv[i], "--gfx-threads") && i + 1 < argc) {
            gx_threads = atoi(argv[++i]);
        } else {
            fprintf(stderr, "uso: %s --rom ROM.z64 [--cart oot.cart] [--swapmap oot_swapmap.bin] [--sram oot.sav] [--seconds N] [--log]\n"
                            "          [--dump-frames PREFIXO [--dump-every N] [--dump-max N]] [--cull-flip] [--vp-flip] [--gfx-ref] [--gfx-threads N] [--probe X,Y]... [--trace N [--trace-after SEGUNDO] [--trace-seg HEX]]\n"
                            "          [--capture PREFIXO --capture-at TAREFA1,TAREFA2,...]\n"
                            "          [--script \"segundo:BOTOES:duracao,...\"] [--wav SOM.wav] [--mudo] [--perf] [--cpus J,D,S]\n"
                            "          [--warp ENTRADA [--adulto] [--cena N]]\n", argv[0]);
            return 2;
        }
    }

    if (g_trace_after < 0) {
        gx.traceN = g_trace_n;
        gx.traceArmed = g_trace_n > 0;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    install_handlers();
#ifdef TARGET_MIYOO
    miyoo_video_init(); /* sem mensagem de "carregando": o jogo abre rapido e ela so piscava na tela */
#endif

    port_options_load("options.txt"); /* (antes do cartucho: as mensagens das telas saem na lingua escolhida) */
    if (access(swapmap, R_OK) != 0) {
        char msg[300];
        snprintf(msg, sizeof msg, TR("The file %s is missing\nfrom the game folder.", "Falta o arquivo %s\nna pasta do jogo."), swapmap);
        return fatal(msg);
    }
    if (!cart_is_valid(cart, swapmap)) {
        char err[512] = "";
#ifdef TARGET_MIYOO
        if (rom == NULL) {
            rom = find_rom();
        }
#endif
        if (rom == NULL) {
            char msg[300];
            snprintf(msg, sizeof msg,
                     TR("Found neither the cartridge (%s) nor the ROM.\nCopy the Ocarina of Time ROM (USA 1.1)\n"
                        "(.z64, .n64, .v64 or .zip) to the game folder.",
                        "Não achei o cartucho (%s) nem a ROM.\nCopie a ROM do Ocarina of Time (USA 1.1)\n"
                        "(.z64, .n64, .v64 ou .zip) para a pasta do jogo."),
                     cart);
            return fatal(msg);
        }
        fprintf(stderr, "Primeira execucao: criando o cartucho virtual a partir de %s...\n", rom);
        if (cart_build(rom, cart, swapmap, cart_progress, NULL, err, sizeof(err)) != 0) {
#ifdef TARGET_MIYOO
            unlink(ROM_TMP);
#endif
            return fatal(err);
        }
#ifdef TARGET_MIYOO
        unlink(ROM_TMP);
#endif
    }
#ifdef TARGET_MIYOO
    {
        /* ROM europeia na pasta: monta idiomas/de.oot e fr.oot uma vez (lang_pal.c) */
        int port_lang_pal_prepare(const char* cartPath, CartProgress cb, void* ud);
        port_lang_pal_prepare(cart, lang_progress, NULL);
    }
#endif

    {
        /* executavel publicado: os dados do jogo saem do cartucho (port/romdata.c, receita oot_dados.rcp) */
        int port_romdata_apply(const char* cartPath, const char* rcpPath, char* err, size_t errsz);
        char err[400] = "";
        if (port_romdata_apply(cart, "oot_dados.rcp", err, sizeof err) != 0) {
            return fatal(err);
        }
    }

    os_hw_set_cpus(cpuGame, cpuGfx, cpuAudio);
    os_platform_init();
    os_hw_set_mem_size(0x400000);
    if (os_hw_ram_map() != 0) {
        return fatal(TR("Could not map the N64 RAM at 0x80000000.", "Não consegui mapear a RAM do N64 em 0x80000000."));
    }
    port_stubs_init();
    pt_init(); /* (opcao perf_trace) */
#ifdef TARGET_MIYOO
    {
        /* overclock: troca o clock uma vez, ao abrir (trocar com o jogo rodando derrubava o MK64) */
        extern int port_opt_clock;
        if (port_opt_clock >= 400 && port_opt_clock <= 1800) {
            char cmd[96];
            int rc;
            snprintf(cmd, sizeof cmd, "/mnt/SDCARD/.tmp_update/bin/cpuclock %d 2>&1", port_opt_clock);
            fprintf(stderr, "[clock] pedindo %d MHz: %s\n", port_opt_clock, cmd);
            rc = system(cmd);
            fprintf(stderr, "[clock] cpuclock terminou (%d); medido agora: %d MHz, PLL %d MHz\n", rc, cpu_mhz(), pll_mhz());
        } else {
            fprintf(stderr, "[clock] sem troca; medido agora: %d MHz\n", cpu_mhz());
        }
    }
#endif
    {
        /* as 2 threads do desenho cada uma no seu nucleo: soltas, o Linux as vezes poe as duas no mesmo
         * nucleo e o quadro atrasa (medido no aparelho: picos de 40-65 ms num quadro de 39 ms) */
        extern int port_opt_nucleos_fixos, gx_helper_cpu;
        if (port_opt_nucleos_fixos && gx_threads >= 2) {
            if (cpuGfx < 0) {
                cpuGfx = 0;
                os_hw_set_cpus(cpuGame, cpuGfx, cpuAudio);
            }
            gx_helper_cpu = cpuGfx == 0 ? 1 : 0;
        }
    }
    gfx_install();
    jpeg_hle_install();
#ifdef TARGET_MIYOO
    audio_out_install(wav, mute ? NULL : miyoo_audio_push);
#else
    (void)mute;
    audio_out_install(wav, NULL);
#endif
    /* o z-buffer (gZBuffer) e outros buffers ficam no BSS do executavel, acima da RAM de 4 MB:
     * o renderizador precisa aceitar enderecos ate o fim do executavel */
    if ((uint32_t)(uintptr_t)_end > gx_ram_end) {
        gx_ram_end = (uint32_t)(uintptr_t)_end;
    }
    {
        /* dados do jogo em secoes proprias (estado rapido, oot-build.sh): o ligador pode po-las depois do _end */
        extern char __stop_game_data[], __stop_game_bss[];
        if ((uint32_t)(uintptr_t)__stop_game_data > gx_ram_end) {
            gx_ram_end = (uint32_t)(uintptr_t)__stop_game_data;
        }
        if ((uint32_t)(uintptr_t)__stop_game_bss > gx_ram_end) {
            gx_ram_end = (uint32_t)(uintptr_t)__stop_game_bss;
        }
    }
    gfx_dump_config(dumpPrefix, dumpEvery, dumpMax);
    if (interpDump != NULL) {
        gfx_interp_dump_config(interpDump);
    }
    if (capPrefix != NULL && capList != NULL) {
        gfx_capture_config(capPrefix, capList);
    }
    if (g_nev > 0) {
        clock_gettime(CLOCK_MONOTONIC, &g_t0);
        port_hooks.read_pads = script_pads;
    }
    if (os_hw_rom_open(cart) != 0) {
        char msg[300];
        snprintf(msg, sizeof msg, TR("Could not open the cartridge %s", "Não consegui abrir o cartucho %s"), cart);
        return fatal(msg);
    }
    port_lang_init(); /* depois do cartucho e das opcoes: traducao por cima das leituras da ROM */
    {
        extern void gx_hd_init(const char* path);
        gx_hd_init("texturas_hd.pak"); /* texturas novas (opcional) */
    }
    if (os_hw_sram_open(sram) != 0) {
        fprintf(stderr, "AVISO: nao consegui abrir o save %s (o jogo nao vai salvar)\n", sram);
    }

    /* leituras da ROM em pedacos grandes: menos trocas de thread */
    gDmaMgrDmaBuffSize = 0x80000;

    pthread_create(&th, NULL, stats_thread, NULL);
#ifdef TARGET_MIYOO
    miyoo_battery_init();
    miyoo_rumble_init();
    {
        /* estados: Game Switcher, desligar e continuar (port/savestate.c). O SIGTERM do desligamento do OnionOS
         * vira um pedido ao jogo (salva e fecha no proximo quadro); antes do jogo rodar, fecha na hora. */
        void port_ss_init(const char* argv0);
        struct sigaction st;
        port_ss_init(argc > 0 ? argv[0] : "oot");
        memset(&st, 0, sizeof st);
        st.sa_handler = on_term;
        st.sa_flags = SA_RESTART; /* (um estado sendo gravado quando ele chega nao e cortado) */
        sigemptyset(&st.sa_mask);
        sigaction(SIGTERM, &st, NULL);
    }
    miyoo_input_init();
    miyoo_audio_init(mute);
    miyoo_video_start();
#endif
    {
        /* recarga dos overlays como no N64 (port/ovl_reset.c): os dados iniciais de cada um, antes do jogo rodar */
        extern void Port_OvlSnapshot(void);
        Port_OvlSnapshot();
    }
    fprintf(stderr, "Iniciando o jogo...\n");
    os_thread_class(OS_RT_GAME); /* esta thread vira a de boot do jogo (as do port ja foram criadas) */
    pt_thread_name("jogo_boot");
    bootproc();
    os_platform_idle_forever();
    return 0;
}
