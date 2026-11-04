/*
 * perf_trace.c -- medicao detalhada do trabalho dos nucleos (opcao perf_trace).
 *
 * Tres coisas gravadas ao mesmo tempo, num anel em memoria (os ultimos ~30 s):
 *  1. eventos: cada thread do port marca o comeco e o fim do que esta fazendo (thread do jogo rodando qual
 *     thread do N64, desenho rasterizando, esperando a auxiliar, auxiliar desenhando ou dormindo, tela copiando
 *     ou esperando o vsync, som...), com o nucleo onde estava (sched_getcpu) e o relogio em ns;
 *  2. amostras: cada thread registrada tem um relogio de tempo de CPU proprio que dispara PT_HZ vezes por segundo
 *     de processamento dela; o sinal anota onde ela estava (pc e lr), o nucleo e a hora. Da o tempo por funcao
 *     de cada thread (e de cada trecho da linha do tempo);
 *  3. /proc: a cada 250 ms, o tempo de cada nucleo (ocupado/ocioso/interrupcoes), o de cada thread do jogo
 *     (usuario, sistema, trocas voluntarias e forcadas) e o dos outros processos do aparelho que usaram CPU.
 * SELECT+R1 (ou sair do jogo) grava tudo em perf_NN.bin na pasta do jogo; tools/oot-perf-report.py transforma em
 * uma pagina com a linha do tempo de cada nucleo e as tabelas. Desligado (0), nada disso roda.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "perf_trace.h"

int port_opt_perf_trace = 0; /* opcao perf_trace: 0 desligado; 1 ligado */

#define PT_EV_N (1u << 18)   /* eventos no anel (16 bytes cada: 4 MB) */
#define PT_SM_N (1u << 17)   /* amostras no anel (16 bytes cada: 2 MB) */
#define PT_HZ 1000           /* amostras por segundo de CPU de cada thread */
#define PT_THREADS 48
#define PT_PROC_BYTES (2u << 20)

typedef struct {
    uint64_t t;
    uint16_t code;
    uint8_t tid, cpu;
    int32_t arg;
} PtEv;

typedef struct {
    uint32_t t_us; /* (desde o comeco) */
    uint8_t tid, cpu, pad[2];
    uint32_t pc, lr;
} PtSm;

static PtEv *g_ev;
static PtSm *g_sm;
static unsigned g_evW, g_smW;
static uint64_t g_t0;
static char g_tname[PT_THREADS][24];
static int g_tsys[PT_THREADS]; /* tid do Linux */
static int g_ntid;
static __thread int t_tid = -1;
static char *g_proc;           /* texto do /proc (anel simples: quando enche, recomeca) */
static unsigned g_procLen;
static pthread_mutex_t g_procMx = PTHREAD_MUTEX_INITIALIZER;
static int g_on;
static volatile int g_dumpReq;

void pt_request_dump(void) {
    pt_ev(PT_MARK, 0);
    g_dumpReq = 1; /* (a thread do /proc grava: o jogo nao para) */
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    const ucontext_t *uc = (const ucontext_t *)ucv;
    unsigned w;
    PtSm *s;
    (void)sig;
    (void)si;
    if (g_sm == NULL || t_tid < 0) {
        return;
    }
    w = __atomic_fetch_add(&g_smW, 1u, __ATOMIC_RELAXED);
    s = &g_sm[w & (PT_SM_N - 1)];
    s->t_us = (uint32_t)((now_ns() - g_t0) / 1000u);
    s->tid = (uint8_t)t_tid;
    s->cpu = (uint8_t)sched_getcpu();
#if defined(__arm__)
    s->pc = (uint32_t)uc->uc_mcontext.arm_pc;
    s->lr = (uint32_t)uc->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
    s->pc = (uint32_t)uc->uc_mcontext.gregs[REG_RIP];
    s->lr = 0;
#else
    (void)uc;
    s->pc = s->lr = 0;
#endif
}

static void proc_add(const char *s, size_t n) {
    pthread_mutex_lock(&g_procMx);
    if (g_procLen + n > PT_PROC_BYTES) {
        g_procLen = 0; /* (anel grosseiro: o relatorio usa o que estiver) */
    }
    memcpy(g_proc + g_procLen, s, n);
    g_procLen += (unsigned)n;
    pthread_mutex_unlock(&g_procMx);
}

static void proc_file(const char *tag, const char *path) {
    static char buf[32768];
    char hdr[96];
    FILE *f = fopen(path, "r");
    size_t n;
    if (f == NULL) {
        return;
    }
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    snprintf(hdr, sizeof hdr, "@%s %llu\n", tag, (unsigned long long)((now_ns() - g_t0) / 1000u));
    proc_add(hdr, strlen(hdr));
    proc_add(buf, n);
    if (n == 0 || buf[n - 1] != '\n') {
        proc_add("\n", 1);
    }
}

/* /proc a cada 250 ms: nucleos, threads do jogo, outros processos (so os que usaram CPU desde a ultima vez) */
static void *proc_thread(void *arg) {
    static unsigned long long lastTicks[32768 / 8];
    int k = 0;
    (void)arg;
    pt_thread_name("perf_proc");
    for (;;) {
        char path[300];
        DIR *d;
        struct dirent *e;
        usleep(250000);
        if (g_dumpReq) {
            g_dumpReq = 0;
            pt_dump();
        }
        proc_file("stat", "/proc/stat");
        d = opendir("/proc/self/task");
        while (d != NULL && (e = readdir(d)) != NULL) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') {
                continue;
            }
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            proc_file("task", path);
            snprintf(path, sizeof path, "/proc/self/task/%s/status", e->d_name);
            {
                /* so as trocas de contexto */
                char buf[2048], out[320];
                FILE *f = fopen(path, "r");
                size_t n;
                char *p, *q;
                if (f == NULL) {
                    continue;
                }
                n = fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
                buf[n] = 0;
                p = strstr(buf, "voluntary_ctxt_switches:");
                q = strstr(buf, "nonvoluntary_ctxt_switches:");
                snprintf(out, sizeof out, "@ctx %s %ld %ld\n", e->d_name, p ? atol(p + 24) : -1L, q ? atol(q + 27) : -1L);
                proc_add(out, strlen(out));
            }
        }
        if (d != NULL) {
            closedir(d);
        }
        if ((k++ & 3) == 0) {
            /* outros processos (1 vez por segundo): so os que gastaram CPU */
            const int self = getpid();
            d = opendir("/proc");
            while (d != NULL && (e = readdir(d)) != NULL) {
                int pid;
                char buf[512];
                FILE *f;
                size_t n;
                if (e->d_name[0] < '0' || e->d_name[0] > '9') {
                    continue;
                }
                pid = atoi(e->d_name);
                if (pid == self || pid <= 0 || pid >= 32768) {
                    continue;
                }
                snprintf(path, sizeof path, "/proc/%d/stat", pid);
                f = fopen(path, "r");
                if (f == NULL) {
                    continue;
                }
                n = fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
                buf[n] = 0;
                {
                    /* campos 14 e 15 (utime, stime), depois do nome entre parenteses */
                    const char *r = strrchr(buf, ')');
                    unsigned long long ut = 0, st = 0;
                    int fi;
                    if (r == NULL) {
                        continue;
                    }
                    r += 2;
                    for (fi = 3; fi < 14 && r != NULL; fi++) {
                        r = strchr(r, ' ');
                        if (r != NULL) {
                            r++;
                        }
                    }
                    if (r == NULL || sscanf(r, "%llu %llu", &ut, &st) != 2) {
                        continue;
                    }
                    if (ut + st != lastTicks[pid / 8] || (pid & 7)) {
                        /* (a tabela e grosseira: guarda 1 de cada 8 pids; os outros vao sempre) */
                        if ((pid & 7) == 0) {
                            lastTicks[pid / 8] = ut + st;
                        }
                        {
                            char hdr[64];
                            snprintf(hdr, sizeof hdr, "@pid %llu\n", (unsigned long long)((now_ns() - g_t0) / 1000u));
                            proc_add(hdr, strlen(hdr));
                            proc_add(buf, n);
                            if (n == 0 || buf[n - 1] != '\n') {
                                proc_add("\n", 1);
                            }
                        }
                    }
                }
            }
            if (d != NULL) {
                closedir(d);
            }
        }
    }
    return NULL;
}

void pt_init(void) {
    struct sigaction sa;
    pthread_t th;
    if (!port_opt_perf_trace || g_on) {
        return;
    }
    g_ev = (PtEv *)calloc(PT_EV_N, sizeof *g_ev);
    g_sm = (PtSm *)calloc(PT_SM_N, sizeof *g_sm);
    g_proc = (char *)malloc(PT_PROC_BYTES);
    if (g_ev == NULL || g_sm == NULL || g_proc == NULL) {
        fprintf(stderr, "[perf] sem memoria para a medicao\n");
        return;
    }
    g_t0 = now_ns();
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    g_on = 1;
    if (pthread_create(&th, NULL, proc_thread, NULL) == 0) {
        pthread_detach(th);
    }
    fprintf(stderr, "[perf] medicao ligada: eventos, %d amostras/s de CPU por thread e /proc; SELECT+R1 grava perf_NN.bin\n", PT_HZ);
}

void pt_thread_name(const char *name) {
    int id;
    if (!g_on || t_tid >= 0) {
        return;
    }
    id = __atomic_fetch_add(&g_ntid, 1, __ATOMIC_ACQ_REL);
    if (id >= PT_THREADS) {
        return;
    }
    snprintf(g_tname[id], sizeof g_tname[id], "%s", name);
    g_tsys[id] = (int)syscall(SYS_gettid);
    t_tid = id;
    {
        /* relogio de CPU desta thread: dispara SIGPROF nela mesma a cada 1/PT_HZ s de processamento */
        struct sigevent sev;
        timer_t tm;
        struct itimerspec its;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = g_tsys[id];
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &tm) == 0) {
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 1000000000 / PT_HZ;
            its.it_value = its.it_interval;
            timer_settime(tm, 0, &its, NULL);
        }
    }
}

void pt_ev(int code, int arg) {
    unsigned w;
    PtEv *e;
    if (!g_on) {
        return;
    }
    if (t_tid < 0) {
        pt_thread_name("?");
        if (t_tid < 0) {
            return;
        }
    }
    w = __atomic_fetch_add(&g_evW, 1u, __ATOMIC_RELAXED);
    e = &g_ev[w & (PT_EV_N - 1)];
    e->t = now_ns() - g_t0;
    e->code = (uint16_t)code;
    e->tid = (uint8_t)t_tid;
    e->cpu = (uint8_t)sched_getcpu();
    e->arg = arg;
}

/* grava perf_NN.bin: "OOTPERF1", u32 versao, contagens, nomes das threads, eventos, amostras, texto do /proc */
static void proc_add(const char *s, size_t n);
void pt_dump(void) {
    static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    char path[64];
    FILE *f;
    int n, i;
    unsigned ew, sw, e0, s0, ne, ns;
    uint32_t hdr[8];
    if (!g_on) {
        return;
    }
    pthread_mutex_lock(&mx);
    for (n = 0; n < 100; n++) {
        snprintf(path, sizeof path, "perf_%02d.bin", n);
        f = fopen(path, "rb");
        if (f == NULL) {
            break;
        }
        fclose(f);
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        pthread_mutex_unlock(&mx);
        return;
    }
    proc_file("maps", "/proc/self/maps"); /* (onde cada biblioteca esta: amostras fora do executavel) */
    ew = __atomic_load_n(&g_evW, __ATOMIC_ACQUIRE);
    sw = __atomic_load_n(&g_smW, __ATOMIC_ACQUIRE);
    ne = ew < PT_EV_N ? ew : PT_EV_N;
    ns = sw < PT_SM_N ? sw : PT_SM_N;
    e0 = ew - ne;
    s0 = sw - ns;
    fwrite("OOTPERF1", 1, 8, f);
    hdr[0] = 1;
    hdr[1] = (uint32_t)(g_ntid < PT_THREADS ? g_ntid : PT_THREADS);
    hdr[2] = ne;
    hdr[3] = ns;
    hdr[4] = g_procLen;
    hdr[5] = PT_HZ;
    hdr[6] = (uint32_t)sysconf(_SC_CLK_TCK);
    hdr[7] = (uint32_t)((now_ns() - g_t0) / 1000u);
    fwrite(hdr, 4, 8, f);
    for (i = 0; i < (int)hdr[1]; i++) {
        int32_t sys = g_tsys[i];
        fwrite(g_tname[i], 1, 24, f);
        fwrite(&sys, 4, 1, f);
    }
    for (i = 0; i < (int)ne; i++) {
        fwrite(&g_ev[(e0 + (unsigned)i) & (PT_EV_N - 1)], sizeof(PtEv), 1, f);
    }
    for (i = 0; i < (int)ns; i++) {
        fwrite(&g_sm[(s0 + (unsigned)i) & (PT_SM_N - 1)], sizeof(PtSm), 1, f);
    }
    pthread_mutex_lock(&g_procMx);
    fwrite(g_proc, 1, g_procLen, f);
    pthread_mutex_unlock(&g_procMx);
    fclose(f);
    fprintf(stderr, "[perf] gravado %s: %u eventos, %u amostras, %u bytes do /proc\n", path, ne, ns, hdr[4]);
    pthread_mutex_unlock(&mx);
}
