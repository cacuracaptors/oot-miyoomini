/*
 * Teste da camada de SO (os_core). Roda no PC (x86-64 ou ARM): cada cenario usa
 * threads, filas e timers do jeito que o jogo usa, e confere a ordem em que as
 * coisas acontecem contra o que o libultra de verdade faria.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "os_core.h"

#define MS(x) ((OSTime)(x) * 46875ull)

static int g_fail;
static char g_log[256];
static int g_loglen;

static void L(char c) {
    if (g_loglen < (int)sizeof(g_log) - 1) {
        g_log[g_loglen++] = c;
        g_log[g_loglen] = 0;
    }
}
static void log_reset(void) {
    g_loglen = 0;
    g_log[0] = 0;
}
static void check_str(const char* name, const char* want) {
    if (strcmp(g_log, want) == 0) {
        printf("OK      %-34s [%s]\n", name, g_log);
    } else {
        printf("FALHOU  %-34s obtido [%s] esperado [%s]\n", name, g_log, want);
        g_fail++;
    }
}
static void check(const char* name, int cond, const char* info) {
    if (cond) {
        printf("OK      %-34s %s\n", name, info);
    } else {
        printf("FALHOU  %-34s %s\n", name, info);
        g_fail++;
    }
}

static unsigned char g_stack[64]; /* o parametro 'sp' e ignorado pela camada */
#define NEWT(t, id, fn, arg, pri) osCreateThread(&(t), id, fn, arg, g_stack, pri)

static void watchdog(int sig) {
    (void)sig;
    const char m[] = "FALHOU  watchdog: o teste travou (30 s)\n";
    if (write(2, m, sizeof(m) - 1) < 0) {
    }
    _exit(2);
}

/* ---------------------------------------------------------------- 1 */
static OSMesgQueue q1, q2, qdone;
static OSMesg b1[8], b2[8], bd[8];
static OSThread th[8];

static void t_hi_echo(void* a) {
    OSMesg m;
    int i;
    (void)a;
    for (i = 0; i < 3; i++) {
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
        L('h');
    }
    osSendMesg(&qdone, (OSMesg)1, OS_MESG_BLOCK);
}
static void test_preempt(void) {
    int i;
    OSMesg m;
    log_reset();
    osCreateMesgQueue(&q1, b1, 8);
    osCreateMesgQueue(&qdone, bd, 8);
    NEWT(th[0], 10, t_hi_echo, NULL, 10);
    osStartThread(&th[0]);
    for (i = 0; i < 3; i++) {
        L('m');
        osSendMesg(&q1, (OSMesg)(long)i, OS_MESG_BLOCK);
    }
    osRecvMesg(&qdone, &m, OS_MESG_BLOCK);
    check_str("prioridade maior preempte no send", "mhmhmh");
}

/* ---------------------------------------------------------------- 2 */
static void t_yield(void* a) {
    char c = (char)(long)a;
    int i;
    for (i = 0; i < 3; i++) {
        L(c);
        osYieldThread();
    }
    osSendMesg(&qdone, (OSMesg)1, OS_MESG_BLOCK);
}
static void test_yield(void) {
    OSMesg m;
    log_reset();
    osCreateMesgQueue(&qdone, bd, 8);
    osSetThreadPri(NULL, 20);
    NEWT(th[0], 11, t_yield, (void*)'A', 5);
    NEWT(th[1], 12, t_yield, (void*)'B', 5);
    osStartThread(&th[0]);
    osStartThread(&th[1]);
    osSetThreadPri(NULL, 1);
    osRecvMesg(&qdone, &m, OS_MESG_BLOCK);
    osRecvMesg(&qdone, &m, OS_MESG_BLOCK);
    check_str("yield entre iguais alterna", "ABABAB");
}

/* ---------------------------------------------------------------- 3 */
static void t_producer(void* a) {
    int i;
    (void)a;
    for (i = 1; i <= 3; i++) {
        L('0' + i);
        osSendMesg(&q1, (OSMesg)(long)i, OS_MESG_BLOCK);
    }
    osSendMesg(&qdone, (OSMesg)1, OS_MESG_BLOCK);
}
static void test_full_queue(void) {
    OSMesg m;
    int i;
    log_reset();
    osCreateMesgQueue(&q1, b1, 1);
    osCreateMesgQueue(&qdone, bd, 8);
    osSetThreadPri(NULL, 20);
    NEWT(th[0], 13, t_producer, NULL, 5);
    osStartThread(&th[0]);
    osSetThreadPri(NULL, 1);
    for (i = 0; i < 3; i++) {
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
        L('a' + (int)(long)m);
    }
    osRecvMesg(&qdone, &m, OS_MESG_BLOCK);
    /* produtor (prioridade maior) escreve 1, tenta 2 e bloqueia; cada recv o acorda e
     * ele preempte antes do consumidor registrar o que leu */
    check_str("send bloqueia em fila cheia", "123bcd");
}

/* ---------------------------------------------------------------- 4 */
static void test_noblock_jam(void) {
    OSMesg m;
    int r1, r2, r3;
    osCreateMesgQueue(&q1, b1, 2);
    r1 = osRecvMesg(&q1, &m, OS_MESG_NOBLOCK);
    osSendMesg(&q1, (OSMesg)1, OS_MESG_NOBLOCK);
    osSendMesg(&q1, (OSMesg)2, OS_MESG_NOBLOCK);
    r2 = osSendMesg(&q1, (OSMesg)3, OS_MESG_NOBLOCK);
    r3 = osJamMesg(&q1, (OSMesg)9, OS_MESG_NOBLOCK);
    check("recv/send/jam NOBLOCK devolvem -1", r1 == -1 && r2 == -1 && r3 == -1, "");
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    log_reset();
    L('0' + (int)(long)m);
    osJamMesg(&q1, (OSMesg)9, OS_MESG_BLOCK);
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    L('0' + (int)(long)m);
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    L('0' + (int)(long)m);
    check_str("jam fura a fila (1, depois 9, 2)", "192");
}

/* ---------------------------------------------------------------- 5 */
static void t_waiter(void* a) {
    OSMesg m;
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    L((char)(long)a);
    osSendMesg(&qdone, (OSMesg)1, OS_MESG_BLOCK);
}
static void test_wake_order(void) {
    OSMesg m;
    int i;
    log_reset();
    osCreateMesgQueue(&q1, b1, 8);
    osCreateMesgQueue(&qdone, bd, 8);
    NEWT(th[0], 14, t_waiter, (void*)'3', 3);
    NEWT(th[1], 15, t_waiter, (void*)'7', 7);
    NEWT(th[2], 16, t_waiter, (void*)'5', 5);
    osStartThread(&th[0]);
    osStartThread(&th[1]);
    osStartThread(&th[2]);
    for (i = 0; i < 3; i++) {
        osSendMesg(&q1, (OSMesg)(long)i, OS_MESG_BLOCK);
    }
    for (i = 0; i < 3; i++) {
        osRecvMesg(&qdone, &m, OS_MESG_BLOCK);
    }
    check_str("espera acorda por prioridade", "753");
}

/* ---------------------------------------------------------------- 6 */
static void t_idle_like(void* a) {
    (void)a;
    L('i');
    osSetThreadPri(NULL, 0);
    for (;;) {
    } /* nunca deve rodar de novo */
}
static OSTimer tm, tm2;
static void test_parked(void) {
    OSMesg m;
    log_reset();
    osCreateMesgQueue(&q1, b1, 8);
    NEWT(th[0], 17, t_idle_like, NULL, 10);
    osStartThread(&th[0]);
    L('m');
    osSetTimer(&tm, MS(30), 0, &q1, (OSMesg)1);
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    L('t');
    check_str("thread com prioridade 0 fica parada", "imt");
}

/* ---------------------------------------------------------------- 7 */
static void test_timers(void) {
    OSMesg m;
    uint64_t t0, dt;
    int i;
    char info[96];
    osCreateMesgQueue(&q1, b1, 8);

    t0 = os_now_ns();
    osSetTimer(&tm, MS(50), 0, &q1, (OSMesg)1);
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    dt = (os_now_ns() - t0) / 1000000;
    snprintf(info, sizeof info, "%llu ms (queria 50)", (unsigned long long)dt);
    check("timer unico", dt >= 49 && dt <= 80, info);

    t0 = os_now_ns();
    osSetTimer(&tm, MS(20), MS(20), &q1, (OSMesg)2);
    for (i = 0; i < 5; i++) {
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    }
    dt = (os_now_ns() - t0) / 1000000;
    osStopTimer(&tm);
    snprintf(info, sizeof info, "5 disparos em %llu ms (queria 100)", (unsigned long long)dt);
    check("timer periodico", dt >= 98 && dt <= 130, info);
    /* depois de parado, o periodico nao pode mais disparar */
    while (osRecvMesg(&q1, &m, OS_MESG_NOBLOCK) == 0) {
    }
    osCreateMesgQueue(&q2, b2, 8);
    osSetTimer(&tm2, MS(60), 0, &q2, (OSMesg)9);
    osRecvMesg(&q2, &m, OS_MESG_BLOCK);
    check("osStopTimer", osRecvMesg(&q1, &m, OS_MESG_NOBLOCK) == -1, "");

    {
        OSTime a, b;
        double ms;
        a = osGetTime();
        osSetTimer(&tm, MS(100), 0, &q1, (OSMesg)3);
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
        b = osGetTime();
        ms = (double)(b - a) / 46875.0;
        snprintf(info, sizeof info, "%.1f ms (queria 100)", ms);
        check("osGetTime (46,875 MHz)", ms > 98 && ms < 130, info);
    }
}

/* ---------------------------------------------------------------- 8 */
static void* poster(void* a) {
    (void)a;
    usleep(30000);
    os_async_send(&q1, (OSMesg)42);
    return NULL;
}
static void test_async(void) {
    OSMesg m = 0;
    pthread_t p;
    uint64_t t0 = os_now_ns(), dt;
    char info[96];
    osCreateMesgQueue(&q1, b1, 8);
    pthread_create(&p, NULL, poster, NULL);
    osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    pthread_join(p, NULL);
    dt = (os_now_ns() - t0) / 1000000;
    snprintf(info, sizeof info, "recebeu %ld apos %llu ms", (long)m, (unsigned long long)dt);
    check("mensagem de thread externa acorda", (long)m == 42 && dt >= 29 && dt < 80, info);
}

/* ---------------------------------------------------------------- 8b */
static volatile uint64_t g_post_ns;
static void* poster2(void* a) {
    uint64_t t0;
    (void)a;
    usleep(10000); /* o jogo ja esta no laco ocupado */
    t0 = os_now_ns();
    os_async_send(&q1, (OSMesg)5);
    g_post_ns = os_now_ns() - t0;
    return NULL;
}
static void test_async_busy(void) {
    OSMesg m = 0;
    pthread_t p;
    uint64_t t0;
    char info[96];
    osCreateMesgQueue(&q1, b1, 8);
    g_post_ns = 1000000000ull;
    pthread_create(&p, NULL, poster2, NULL);
    t0 = os_now_ns();
    while (os_now_ns() - t0 < 60000000ull) {
    } /* 60 ms de "calculo" sem chamar nenhuma funcao do SO */
    pthread_join(p, NULL);
    snprintf(info, sizeof info, "os_async_send levou %llu us com o jogo ocupado", (unsigned long long)(g_post_ns / 1000));
    check("thread do port nao fica presa", g_post_ns < 5000000ull, info);
    check("mensagem entregue depois", osRecvMesg(&q1, &m, OS_MESG_NOBLOCK) == 0 && (long)m == 5, "");
}

/* ---------------------------------------------------------------- 9 */
static int g_ticks;
static void on_tick(void* a) {
    (void)a;
    g_ticks++;
    os_event_fire(OS_EVENT_VI);
}
static void test_periodic_event(void) {
    OSMesg m;
    int id, i;
    uint64_t t0, dt;
    char info[96];
    osCreateMesgQueue(&q1, b1, 8);
    osSetEventMesg(OS_EVENT_VI, &q1, (OSMesg)7);
    t0 = os_now_ns();
    id = os_periodic_add(16666667ull, on_tick, NULL); /* ~60 Hz */
    for (i = 0; i < 12; i++) {
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
    }
    os_periodic_remove(id);
    dt = (os_now_ns() - t0) / 1000000;
    snprintf(info, sizeof info, "12 retraces em %llu ms (queria 200), msg=%ld", (unsigned long long)dt, (long)m);
    check("retrace periodico + osSetEventMesg", dt >= 195 && dt <= 235 && (long)m == 7 && g_ticks >= 12, info);
}

/* ---------------------------------------------------------------- 10 */
static void t_pong(void* a) {
    OSMesg m;
    (void)a;
    for (;;) {
        osRecvMesg(&q1, &m, OS_MESG_BLOCK);
        osSendMesg(&q2, m, OS_MESG_BLOCK);
    }
}
static void test_stress(void) {
    OSMesg m;
    int i, n = 20000;
    uint64_t t0;
    char info[96];
    double us;
    osCreateMesgQueue(&q1, b1, 8);
    osCreateMesgQueue(&q2, b2, 8);
    NEWT(th[0], 18, t_pong, NULL, 5);
    osStartThread(&th[0]);
    t0 = os_now_ns();
    for (i = 0; i < n; i++) {
        osSendMesg(&q1, (OSMesg)(long)i, OS_MESG_BLOCK);
        osRecvMesg(&q2, &m, OS_MESG_BLOCK);
        if ((long)m != i) {
            check("ping-pong", 0, "mensagem errada");
            return;
        }
    }
    us = (double)(os_now_ns() - t0) / 1000.0 / n;
    snprintf(info, sizeof info, "%.1f us por ida-e-volta (2 trocas de thread)", us);
    check("ping-pong 20000 voltas", 1, info);
    osStopThread(&th[0]);
}

/* ---------------------------------------------------------------- 11 */
static void t_exit(void* a) {
    (void)a;
    L('x');
}
static void test_thread_end(void) {
    log_reset();
    NEWT(th[0], 19, t_exit, NULL, 10);
    osStartThread(&th[0]);
    L('m');
    check("thread que retorna termina", th[0].state == OS_STATE_STOPPED && strcmp(g_log, "xm") == 0, g_log);
}

int main(void) {
    signal(SIGALRM, watchdog);
    alarm(30);
    os_platform_init();
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_preempt();
    test_yield();
    test_full_queue();
    test_noblock_jam();
    test_wake_order();
    test_parked();
    test_timers();
    test_async();
    test_async_busy();
    test_periodic_event();
    test_stress();
    test_thread_end();

    if (g_fail) {
        printf("\n%d TESTE(S) FALHARAM\n", g_fail);
        return 1;
    }
    printf("\nTODOS OS TESTES PASSARAM\n");
    return 0;
}
