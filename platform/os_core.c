/*
 * os_core.c -- camada de "sistema operacional do N64" (parte 1):
 * threads, filas de mensagem, timers, tempo e eventos de hardware.
 *
 * COMO FUNCIONA
 *
 * O N64 tem uma CPU so, e o libultra escolhe a thread pela PRIORIDADE, sem fatia de
 * tempo: a thread em execucao so perde a CPU quando bloqueia (osRecvMesg numa fila
 * vazia, osSendMesg numa fila cheia...) ou quando uma thread de prioridade MAIOR
 * fica pronta. O jogo foi escrito contando com isso, entao aqui fazemos igual:
 *
 *  - cada thread do N64 e uma pthread, mas so UMA roda por vez (quem tem a CPU);
 *  - todo codigo do jogo roda com um trinco global (g_lock) fechado. Ele so e
 *    aberto quando a thread bloqueia (pthread_cond_wait). Por isso nao ha corrida
 *    entre threads do jogo, e as "interrupcoes" (retrace do video, timers) so sao
 *    entregues em pontos seguros: dentro das chamadas osXxx;
 *  - se nenhuma thread esta pronta, quem bloqueou por ultimo vira o laco ocioso:
 *    dorme ate o proximo evento de hardware ou ate uma thread do port enviar algo
 *    (os_async_send);
 *  - threads de prioridade 0 que baixaram a propria prioridade (a thread ociosa do
 *    jogo faz "for (;;) {}") ficam "estacionadas" e nunca sao escalonadas.
 *
 * Todas as funcoes osXxx abaixo devem ser chamadas por threads do jogo.
 * Threads do port (renderizador, audio...) usam so as funcoes os_async_*.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>

#include "perf_trace.h"
#include "os_core.h"
#include "ultra64/exception.h"

#define OSCORE_STACK_BYTES (1024 * 1024)
#define OSCORE_MAX_THREADS 64
#define OSCORE_MAX_PERIODIC 8
#define OSCORE_MAX_CATCHUP 8 /* quantos periodos atrasados sao recuperados de uma vez */

int os_debug_log = 0;

/* ------------------------------------------------------------------ tempo */

uint64_t os_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t ns_to_ticks(uint64_t ns) {
    return (ns / 1000000000ull) * OSCORE_TICKS_PER_SEC + ((ns % 1000000000ull) * 46875ull) / 1000000ull;
}

static uint64_t ticks_to_ns(uint64_t t) {
    /* 1e9 / 46875000 = 64 / 3 */
    return (t / OSCORE_TICKS_PER_SEC) * 1000000000ull + ((t % OSCORE_TICKS_PER_SEC) * 64ull) / 3ull;
}

static uint64_t g_epoch_ns;      /* instante em que a camada iniciou */
static uint64_t g_time_offset;   /* ajuste de osSetTime, em ticks */

/* ---------------------------------------------------------------- estruturas */

typedef struct NThread {
    OSThread* t;
    pthread_cond_t cv;
    void (*entry)(void*);
    void* arg;
    int started; /* a pthread ja foi criada */
    int dead;
    int parked;  /* pediu prioridade IDLE: nunca escalonar */
    pthread_t th;
    int has_th;
    unsigned long long seq; /* ordem de chegada entre os prontos */
} NThread;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NThread* g_current;              /* thread com a CPU (NULL = ociosa) */
static unsigned long long g_seq;
static NThread* g_threads[OSCORE_MAX_THREADS];
static int g_nthreads;
static int g_inited;

static OSThread g_boot_thread;
static NThread g_boot_n;

static OSTimer* g_timers;               /* ordenada por t->value (vencimento em ns) */

typedef struct {
    int active;
    uint64_t period_ns;
    uint64_t next_ns;
    void (*fn)(void*);
    void* arg;
} Periodic;
static Periodic g_periodic[OSCORE_MAX_PERIODIC];

static struct {
    OSMesgQueue* mq;
    OSMesg msg;
} g_events[OS_NUM_EVENTS];

static OSIntMask g_intmask = OS_IM_CPU;

/* ------------------------------------------- mensagens de threads do port */

/*
 * Threads do port (renderizador, audio, leitura da ROM) NUNCA pegam o trinco do jogo,
 * porque o jogo o segura quase o tempo todo e elas ficariam paradas. Elas deixam o
 * recado numa lista (com um trinco proprio, curtissimo) e cutucam um eventfd. O jogo
 * esvazia a lista em pontos seguros, e a thread ociosa dorme nesse eventfd.
 */
#define OSCORE_ASYNC_MAX 1024
typedef struct {
    OSMesgQueue* mq; /* NULL = evento de hardware (ou chamada, se fn != NULL) */
    OSMesg msg;
    OSEvent ev;
    void (*fn)(void*); /* chamada para rodar dentro do jogo */
    void* arg;
} AsyncItem;
static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;
static AsyncItem g_async[OSCORE_ASYNC_MAX];
static int g_async_n;
static int g_async_pending; /* leitura sem trinco */
static int g_efd = -1;

static void async_push(OSMesgQueue* mq, OSMesg msg, OSEvent ev, void (*fn)(void*), void* arg) {
    uint64_t one = 1;
    for (;;) {
        pthread_mutex_lock(&g_async_lock);
        if (g_async_n < OSCORE_ASYNC_MAX) {
            g_async[g_async_n].mq = mq;
            g_async[g_async_n].msg = msg;
            g_async[g_async_n].ev = ev;
            g_async[g_async_n].fn = fn;
            g_async[g_async_n].arg = arg;
            g_async_n++;
            __atomic_store_n(&g_async_pending, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&g_async_lock);
            break;
        }
        pthread_mutex_unlock(&g_async_lock);
        usleep(1000); /* lista cheia: o jogo esta ocupado, espera um pouco */
    }
    if (write(g_efd, &one, sizeof one) < 0) {
        /* eventfd cheio: ja ha um aviso pendente, tudo bem */
    }
}

static NThread* nt(OSThread* t) {
    return (NThread*)(uintptr_t)t->context.at;
}

/* ------------------------------------------------------ filas de espera */

/* Filas de threads (mq->mtqueue / mq->fullqueue), ordenadas por prioridade,
 * a maior primeiro; entre iguais, quem chegou antes. */
static void q_insert(OSThread** q, OSThread* t) {
    OSThread** pp = q;
    while (*pp != NULL && (*pp)->priority >= t->priority) {
        pp = &(*pp)->next;
    }
    t->next = *pp;
    *pp = t;
    t->queue = q;
}

static OSThread* q_pop(OSThread** q) {
    OSThread* t = *q;
    if (t != NULL) {
        *q = t->next;
        t->next = NULL;
        t->queue = NULL;
    }
    return t;
}

static void q_remove(OSThread* t) {
    OSThread** pp;
    if (t->queue == NULL) {
        return;
    }
    pp = t->queue;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp == t) {
        *pp = t->next;
    }
    t->next = NULL;
    t->queue = NULL;
}

/* ------------------------------------------------------------- escalonador */

static void run_hw_events(void);

static NThread* pick_best(void) {
    NThread* best = NULL;
    int i;
    for (i = 0; i < g_nthreads; i++) {
        NThread* n = g_threads[i];
        u16 s;
        if (n->dead || n->parked) {
            continue;
        }
        s = n->t->state;
        if (s != OS_STATE_RUNNABLE && s != OS_STATE_RUNNING) {
            continue;
        }
        if (best == NULL || n->t->priority > best->t->priority ||
            (n->t->priority == best->t->priority && n->seq < best->seq)) {
            best = n;
        }
    }
    return best;
}

static uint64_t next_hw_due_ns(void) {
    uint64_t due = 0;
    int i;
    if (g_timers != NULL) {
        due = g_timers->value;
    }
    for (i = 0; i < OSCORE_MAX_PERIODIC; i++) {
        if (g_periodic[i].active && (due == 0 || g_periodic[i].next_ns < due)) {
            due = g_periodic[i].next_ns;
        }
    }
    return due;
}

/* Dorme (com o trinco aberto) ate o proximo evento ou ate uma thread do port enviar
 * algo. Enquanto o trinco esta aberto nenhuma thread do jogo roda (todas esperam a vez
 * nas suas variaveis de condicao), entao o estado do jogo fica intacto. */
uint64_t os_idle_ns; /* tempo total sem nenhuma thread do jogo rodando (estatistica) */

static void idle_wait(void) {
    uint64_t due = next_hw_due_ns(), t0;
    struct timespec ts, *tp = NULL;
    struct pollfd pf;
    if (due != 0) {
        uint64_t now = os_now_ns();
        uint64_t d = due > now ? due - now : 0;
        ts.tv_sec = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        tp = &ts;
    }
    pf.fd = g_efd;
    pf.events = POLLIN;
    pf.revents = 0;
    t0 = os_now_ns();
    PT_EV(PT_GAME_IDLE, 0); /* (medicao: nenhuma thread do N64 pronta) */
    pthread_mutex_unlock(&g_lock);
    if (ppoll(&pf, 1, tp, NULL) > 0 && (pf.revents & POLLIN)) {
        uint64_t v;
        if (read(g_efd, &v, sizeof v) < 0) {
        }
    }
    pthread_mutex_lock(&g_lock);
    PT_EV(PT_GAME_IDLE + 1, 0);
    os_idle_ns += os_now_ns() - t0;
}

static void* thread_main(void* p);

static int g_game_cpu = -1;

/* Prende a thread que chama a um nucleo (cpu < 0 = livre). So em Linux. */
void os_pin_cpu(int cpu) {
#ifdef __linux__
    cpu_set_t set;
    if (cpu < 0) {
        return;
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);
#else
    (void)cpu;
#endif
}

/* Prioridade (nice) da thread que chama. As threads do jogo (logica e o som do jogo) ficam na frente
 * do desenho: quando os dois nucleos estao desenhando, o som do jogo ainda roda na hora e nao seca. */
void os_thread_nice(int nice) {
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice);
#else
    (void)nice;
#endif
}

/* Classe da thread que chama (opcoes.txt: prioridade_real).
 *   0          normal (desenho e o resto): divide os nucleos pelo Linux
 *   OS_RT_*    tempo real (SCHED_FIFO): passa na frente das normais NA HORA em que acorda.
 * As de tempo real sao curtas e dormem muito (logica do jogo, som, relogio do video); o desenho
 * usa o que sobra dos dois nucleos. Com prioridade so por "nice" o Linux as vezes demorava
 * 10..16 ms para rodar quem acordou, e o som do jogo e o relogio atrasavam.
 * Importante: thread nova herda a classe de quem a cria, entao cada thread do port diz a sua. */
int os_rt = 1;
/* O desenho tambem em tempo real, abaixo de todas as outras: nenhum programa do sistema toma o nucleo
 * no meio do quadro (medido no aparelho: com prioridade comum, de vez em quando um quadro levava
 * 8..10 ms a mais; em tempo real os picos sumiram). */
int os_rt_draw = 0; /* padrao 0 (lote 99): com 1 + alvo 30 havia engasgos de imagem (3+ vsyncs, ~27-35/min contra ~6) e
                     * estalos no som (lotes 75-80); o Regis usa 0 */

void os_thread_class(int level) {
#ifdef __linux__
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    if (level == OS_RT_DRAW && !os_rt_draw) {
        level = 0;
    }
    if (level > 0 && os_rt) {
        sp.sched_priority = level;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
            return;
        }
        {
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "os: o Linux recusou o tempo real; usando so a prioridade comum\n");
            }
        }
    }
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    os_thread_nice(level > 0 ? -8 - level : 0); /* sem tempo real: so a prioridade comum */
#else
    (void)level;
#endif
}

/* Nucleo onde rodam TODAS as threads do jogo (so uma roda por vez). Valido para
 * threads criadas depois; a de boot e prendida na hora. A classe da thread de boot e
 * dada por quem chama bootproc (depois de criar as threads do port). */
void os_set_game_cpu(int cpu) {
    g_game_cpu = cpu;
    os_pin_cpu(cpu);
}

static void start_pthread(NThread* n) {
    pthread_attr_t a;
    pthread_t th;
    n->started = 1;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, OSCORE_STACK_BYTES);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, thread_main, n) != 0) {
        fprintf(stderr, "os_core: nao consegui criar a thread %d\n", (int)n->t->id);
        abort();
    }
    n->th = th;
    n->has_th = 1;
    pthread_attr_destroy(&a);
}

/*
 * Passa a CPU para a melhor thread pronta. 'self' e a thread que esta chamando:
 * o estado dela ja deve ter sido ajustado (WAITING, STOPPED, RUNNABLE...).
 * Volta quando 'self' for escolhida de novo. Se leave != 0, entrega a CPU e volta
 * na hora (usado quando a thread esta terminando).
 */
/* estatistica (-perf): maior trecho que uma thread do jogo rodou sem ceder a vez, e mensagens perdidas
 * (fila cheia num envio sem bloqueio: por exemplo um retrace que o som do jogo nao chegou a ver) */
uint64_t os_max_run_ns;
int os_max_run_tid;
uint32_t os_msg_drops;
static uint64_t g_run_t0;
/* demora do Linux para colocar para rodar a thread do jogo que recebeu a vez */
uint64_t os_wake_max_ns, os_wake_sum_ns;
uint32_t os_wake_n;
static uint64_t g_switch_t;
/* filas que mais perderam mensagens */
void* os_drop_mq[8];
uint32_t os_drop_cnt[8];

static void woke_now(void) {
    uint64_t now = os_now_ns(), d = now - g_switch_t;
    if (g_switch_t != 0) {
        if (d > os_wake_max_ns) {
            os_wake_max_ns = d;
        }
        os_wake_sum_ns += d;
        os_wake_n++;
    }
    g_run_t0 = now;
}

static void run_mark(NThread* self) {
    uint64_t now = os_now_ns();
    if (self != NULL && g_run_t0 != 0 && now - g_run_t0 > os_max_run_ns) {
        os_max_run_ns = now - g_run_t0;
        os_max_run_tid = (int)self->t->id;
    }
}

static void reschedule(NThread* self, int leave) {
    run_mark(self);
    for (;;) {
        NThread* best;
        run_hw_events();
        best = pick_best();
        if (best == NULL) {
            g_current = NULL;
            idle_wait();
            continue;
        }
        best->t->state = OS_STATE_RUNNING;
        g_run_t0 = os_now_ns();
        PT_EV(PT_GAME_RUN, (int)best->t->id); /* (medicao: esta thread do N64 passa a rodar) */
        if (best == self && !leave) {
            g_current = self;
            return;
        }
        g_current = best;
        g_switch_t = os_now_ns();
        if (!best->started) {
            start_pthread(best);
        } else {
            pthread_cond_signal(&best->cv);
        }
        if (leave) {
            return;
        }
        while (g_current != self) {
            pthread_cond_wait(&self->cv, &g_lock);
        }
        woke_now();
        return;
    }
}

/* Ponto seguro: entrega eventos vencidos e cede a CPU se uma thread de prioridade
 * maior (ou a propria thread, se estacionada) pede. So para threads do jogo. */
static void sched_point(void) {
    NThread* self = g_current;
    NThread* best;
    if (self == NULL) {
        return;
    }
    run_hw_events();
    best = pick_best();
    if (best != self) {
        if (self->t->state == OS_STATE_RUNNING) {
            self->t->state = OS_STATE_RUNNABLE;
        }
        reschedule(self, 0);
    }
}

static void wake(OSThread* t) {
    if (t->state == OS_STATE_WAITING) {
        t->state = OS_STATE_RUNNABLE;
        nt(t)->seq = ++g_seq;
    }
}

static void block_on(OSThread** q) {
    NThread* self = g_current;
    self->t->state = OS_STATE_WAITING;
    q_insert(q, self->t);
    reschedule(self, 0);
}

static void* thread_main(void* p) {
    NThread* n = (NThread*)p;
    os_pin_cpu(g_game_cpu);
    os_thread_class(OS_RT_GAME);
    {
        char nm[24];
        snprintf(nm, sizeof nm, "jogo_t%d", (int)n->t->id);
        PT_NAME(nm);
    }
    pthread_mutex_lock(&g_lock);
    while (g_current != n) {
        pthread_cond_wait(&n->cv, &g_lock);
    }
    woke_now();
    n->entry(n->arg);
    /* a funcao da thread retornou: o libultra chama __osCleanupThread */
    n->dead = 1;
    n->t->state = OS_STATE_STOPPED;
    q_remove(n->t);
    reschedule(n, 1);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---------------------------------------------------------- mensagens */

/* Envio sem trocar de thread. Devolve 0 se enfileirou, -1 se cheia e nao deu para
 * bloquear (flag NOBLOCK). Com flag BLOCK bloqueia a thread atual ate caber. */
static int send_core(OSMesgQueue* mq, OSMesg msg, s32 flag, int jam) {
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            int i;
            os_msg_drops++;
            for (i = 0; i < 8; i++) {
                if (os_drop_mq[i] == (void*)mq || os_drop_mq[i] == NULL) {
                    os_drop_mq[i] = (void*)mq;
                    os_drop_cnt[i]++;
                    break;
                }
            }
            return -1;
        }
        block_on(&mq->fullqueue);
    }
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    if (mq->mtqueue != NULL) {
        wake(q_pop(&mq->mtqueue));
    }
    return 0;
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    s32 r;
    run_hw_events();
    r = send_core(mq, msg, flag, 0);
    sched_point();
    return r;
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    s32 r;
    run_hw_events();
    r = send_core(mq, msg, flag, 1);
    sched_point();
    return r;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    run_hw_events();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            sched_point();
            return -1;
        }
        block_on(&mq->mtqueue);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    if (mq->fullqueue != NULL) {
        wake(q_pop(&mq->fullqueue));
    }
    sched_point();
    return 0;
}

int os_send_nb(OSMesgQueue* mq, OSMesg msg) {
    return send_core(mq, msg, OS_MESG_NOBLOCK, 0);
}

/* ---------------------------------------------------------------- eventos */

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg) {
    if (e < OS_NUM_EVENTS) {
        g_events[e].mq = mq;
        g_events[e].msg = msg;
    }
}

void os_event_fire(OSEvent e) {
    if (e < OS_NUM_EVENTS && g_events[e].mq != NULL) {
        send_core(g_events[e].mq, g_events[e].msg, OS_MESG_NOBLOCK, 0);
    }
}

void os_async_send(OSMesgQueue* mq, OSMesg msg) {
    async_push(mq, msg, 0, NULL, NULL);
}

void os_async_call(void (*fn)(void*), void* arg) {
    async_push(NULL, NULL, 0, fn, arg);
}

void os_async_event(OSEvent e) {
    async_push(NULL, NULL, e, NULL, NULL);
}

/* Entrega as mensagens das threads do port. Chamada com o trinco fechado. */
static void drain_async(void) {
    AsyncItem local[OSCORE_ASYNC_MAX];
    int n, i;
    if (!__atomic_load_n(&g_async_pending, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&g_async_lock);
    n = g_async_n;
    memcpy(local, g_async, (size_t)n * sizeof(AsyncItem));
    g_async_n = 0;
    __atomic_store_n(&g_async_pending, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_async_lock);
    for (i = 0; i < n; i++) {
        if (local[i].fn != NULL) {
            local[i].fn(local[i].arg);
        } else if (local[i].mq != NULL) {
            send_core(local[i].mq, local[i].msg, OS_MESG_NOBLOCK, 0);
        } else {
            os_event_fire(local[i].ev);
        }
    }
}

/* ----------------------------------------------------------------- timers */

static void timer_insert(OSTimer* t) {
    OSTimer** pp = &g_timers;
    OSTimer* prev = NULL;
    while (*pp != NULL && (*pp)->value <= t->value) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    t->next = *pp;
    if (*pp != NULL) {
        (*pp)->prev = t;
    }
    t->prev = prev;
    *pp = t;
}

static int timer_remove(OSTimer* t) {
    OSTimer** pp = &g_timers;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp == NULL) {
        return 0;
    }
    *pp = t->next;
    if (t->next != NULL) {
        t->next->prev = t->prev;
    }
    t->next = NULL;
    t->prev = NULL;
    return 1;
}

/*
 * Nesta camada, t->value guarda o vencimento em nanossegundos do relogio
 * monotonico (o jogo nunca le esse campo) e t->interval continua em ticks.
 */
s32 osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    OSTime first = (countdown != 0) ? countdown : interval;
    timer_remove(t);
    t->interval = interval;
    t->mq = mq;
    t->msg = msg;
    t->value = os_now_ns() + ticks_to_ns(first);
    timer_insert(t);
    return 0;
}

s32 osStopTimer(OSTimer* t) {
    return timer_remove(t) ? 0 : -1;
}

/* Entrega o que venceu: timers, tarefas periodicas (retrace do video...). Roda com
 * o trinco fechado; so enfileira mensagens e acorda threads, nunca troca de thread. */
static void run_hw_events(void) {
    uint64_t now;
    int i;
    drain_async();
    if (g_timers == NULL) {
        int any = 0;
        for (i = 0; i < OSCORE_MAX_PERIODIC; i++) {
            any |= g_periodic[i].active;
        }
        if (!any) {
            return;
        }
    }
    now = os_now_ns();
    while (g_timers != NULL && g_timers->value <= now) {
        OSTimer* t = g_timers;
        g_timers = t->next;
        if (g_timers != NULL) {
            g_timers->prev = NULL;
        }
        t->next = NULL;
        t->prev = NULL;
        if (t->interval != 0) {
            uint64_t step = ticks_to_ns(t->interval);
            t->value += step;
            if (t->value + step * OSCORE_MAX_CATCHUP < now) {
                t->value = now + step;
            }
            timer_insert(t);
        }
        send_core(t->mq, t->msg, OS_MESG_NOBLOCK, 0);
    }
    for (i = 0; i < OSCORE_MAX_PERIODIC; i++) {
        Periodic* p = &g_periodic[i];
        int n = 0;
        while (p->active && now >= p->next_ns && n < OSCORE_MAX_CATCHUP) {
            p->next_ns += p->period_ns;
            n++;
            p->fn(p->arg);
        }
        if (p->active && now >= p->next_ns) {
            p->next_ns = now + p->period_ns;
        }
    }
}

int os_periodic_add(uint64_t period_ns, void (*fn)(void*), void* arg) {
    int i;
    for (i = 0; i < OSCORE_MAX_PERIODIC; i++) {
        if (!g_periodic[i].active) {
            g_periodic[i].period_ns = period_ns;
            g_periodic[i].next_ns = os_now_ns() + period_ns;
            g_periodic[i].fn = fn;
            g_periodic[i].arg = arg;
            g_periodic[i].active = 1;
            return i;
        }
    }
    return -1;
}

void os_periodic_remove(int id) {
    if (id >= 0 && id < OSCORE_MAX_PERIODIC) {
        g_periodic[id].active = 0;
    }
}

/* ------------------------------------------------------------------ tempo */

OSTime osGetTime(void) {
    run_hw_events();
    sched_point();
    return ns_to_ticks(os_now_ns() - g_epoch_ns) + g_time_offset;
}

void osSetTime(OSTime time) {
    g_time_offset = time - ns_to_ticks(os_now_ns() - g_epoch_ns);
}

OSIntMask osSetIntMask(OSIntMask m) {
    /* Todo codigo do jogo ja roda atomico (trinco global): so guardamos o valor. */
    OSIntMask old = g_intmask;
    g_intmask = m;
    return old;
}

OSIntMask osGetIntMask(void) {
    return g_intmask;
}

s32 __osDisableInt(void) {
    OSIntMask old = osSetIntMask(OS_IM_NONE);
    return (s32)old;
}

void __osRestoreInt(s32 mask) {
    osSetIntMask((OSIntMask)mask);
}

/* ---------------------------------------------------------------- threads */

void osCreateThread(OSThread* t, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    NThread* n;
    int i;
    (void)sp; /* a pilha do jogo nao e usada: cada thread tem a sua, da pthread */
    os_log("[os] osCreateThread id=%d pri=%d entry=%p\n", (int)id, (int)pri, (void*)entry);
    n = (NThread*)calloc(1, sizeof(NThread));
    if (n == NULL) {
        abort();
    }
    pthread_cond_init(&n->cv, NULL);
    n->t = t;
    n->entry = entry;
    n->arg = arg;
    t->next = NULL;
    t->queue = NULL;
    t->tlnext = NULL;
    t->priority = pri;
    t->id = id;
    t->fp = 0;
    t->flags = 0;
    t->state = OS_STATE_STOPPED;
    t->context.at = (u64)(uintptr_t)n;
    /* reaproveita a vaga se este OSThread ja existia (thread recriada) */
    for (i = 0; i < g_nthreads; i++) {
        if (g_threads[i]->t == t) {
            g_threads[i]->dead = 1;
            g_threads[i] = n;
            return;
        }
    }
    if (g_nthreads >= OSCORE_MAX_THREADS) {
        fprintf(stderr, "os_core: threads demais\n");
        abort();
    }
    g_threads[g_nthreads++] = n;
}

void osStartThread(OSThread* t) {
    os_log("[os] osStartThread id=%d\n", (int)t->id);
    if (t->state == OS_STATE_STOPPED) {
        if (t->queue != NULL) {
            t->state = OS_STATE_WAITING; /* estava esperando numa fila quando foi parada */
        } else {
            t->state = OS_STATE_RUNNABLE;
            nt(t)->seq = ++g_seq;
        }
    }
    sched_point();
}

void osStopThread(OSThread* t) {
    NThread* self = g_current;
    if (t == NULL) {
        t = self->t;
    }
    if (t == self->t) {
        t->state = OS_STATE_STOPPED;
        reschedule(self, 0);
    } else if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) {
        t->state = OS_STATE_STOPPED; /* se estava numa fila, continua nela */
    }
}

void osDestroyThread(OSThread* t) {
    NThread* self = g_current;
    if (t == NULL) {
        t = self->t;
    }
    os_log("[os] osDestroyThread id=%d\n", (int)t->id);
    nt(t)->dead = 1;
    t->state = OS_STATE_STOPPED;
    q_remove(t);
    if (t == self->t) {
        reschedule(self, 0); /* nao volta: a thread nunca mais e escolhida */
    }
}

void osSetThreadPri(OSThread* t, OSPri pri) {
    NThread* self = g_current;
    if (t == NULL) {
        t = self->t;
    }
    t->priority = pri;
    if (t->queue != NULL) {
        OSThread** q = t->queue;
        q_remove(t);
        q_insert(q, t);
    }
    /* A thread ociosa do jogo faz osSetThreadPri(NULL, 0) e depois "for (;;) {}":
     * nunca mais deve ser escalonada. */
    nt(t)->parked = (pri == OS_PRIORITY_IDLE);
    sched_point();
}

OSPri osGetThreadPri(OSThread* t) {
    return (t != NULL) ? t->priority : g_current->t->priority;
}

OSId osGetThreadId(OSThread* t) {
    return (t != NULL) ? t->id : g_current->t->id;
}

/* Ponto de troca colocado no meio do codigo do jogo (oot-patch.py): se ha retrace, timer vencido
 * ou mensagem do port pendente, entrega na hora (e a CPU, se acordou alguem de prioridade maior).
 * Sem nada pendente custa uma leitura do relogio. So para threads do jogo (trinco fechado). */
void port_preempt_point(void) {
    uint64_t due;
    if (g_current == NULL) {
        return;
    }
    if (!__atomic_load_n(&g_async_pending, __ATOMIC_ACQUIRE)) {
        due = next_hw_due_ns();
        if (due == 0 || os_now_ns() < due) {
            return;
        }
    }
    sched_point();
}

void osYieldThread(void) {
    NThread* self = g_current;
    self->seq = ++g_seq;
    self->t->state = OS_STATE_RUNNABLE;
    reschedule(self, 0);
}

/* --------------------------------------------------------------- partida */

void os_platform_init(void) {
    if (g_inited) {
        return;
    }
    g_inited = 1;
    g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_efd < 0) {
        fprintf(stderr, "os_core: eventfd falhou\n");
        abort();
    }

    g_epoch_ns = os_now_ns();
    g_time_offset = 0;

    /* A thread principal do processo e a thread de boot do N64. Fica com o trinco
     * fechado de agora em diante; so o abre quando bloqueia. */
    pthread_mutex_lock(&g_lock);
    memset(&g_boot_thread, 0, sizeof(g_boot_thread));
    memset(&g_boot_n, 0, sizeof(g_boot_n));
    pthread_cond_init(&g_boot_n.cv, NULL);
    g_boot_n.t = &g_boot_thread;
    g_boot_n.started = 1;
    g_boot_n.th = pthread_self();
    g_boot_n.has_th = 1;
    g_boot_thread.id = 0;
    g_boot_thread.priority = OS_PRIORITY_IDLE + 1;
    g_boot_thread.state = OS_STATE_RUNNING;
    g_boot_thread.context.at = (u64)(uintptr_t)&g_boot_n;
    g_threads[g_nthreads++] = &g_boot_n;
    g_current = &g_boot_n;
}

void os_platform_idle_forever(void) {
    g_boot_n.parked = 1;
    g_boot_thread.state = OS_STATE_RUNNABLE;
    reschedule(&g_boot_n, 0);
    for (;;) {
        pthread_cond_wait(&g_boot_n.cv, &g_lock); /* nunca acontece */
    }
}

/* ------------------------------------------------------------- diagnostico */

/* Mostra o estado de todas as threads do jogo e, se sig != 0, manda esse sinal para cada
 * uma (o programa principal instala um tratador que imprime onde ela esta parada).
 * NAO pega o trinco do jogo: serve justamente para quando o jogo esta preso. */
void os_dump_threads(int sig) {
    static const char* names[] = { "?", "parada", "pronta", "?", "rodando", "?", "?", "?", "esperando" };
    int i, n = g_nthreads;
    NThread* cur = g_current;
    for (i = 0; i < n; i++) {
        NThread* t = g_threads[i];
        int st;
        if (t == NULL || t->t == NULL) {
            continue;
        }
        st = (int)t->t->state;
        fprintf(stderr, "[thread %d] id=%d pri=%d estado=%s%s%s%s\n", i, (int)t->t->id, (int)t->t->priority,
                (st >= 0 && st <= 8) ? names[st] : "?", (t == cur) ? " (COM A CPU)" : "", t->parked ? " (estacionada)" : "",
                t->dead ? " (morta)" : "");
        if (sig != 0 && t->has_th && !t->dead) {
            pthread_kill(t->th, sig);
            usleep(150000);
        }
    }
    if (cur == NULL) {
        fprintf(stderr, "[os] nenhuma thread do jogo esta com a CPU (ociosa, esperando evento)\n");
    }
}

/* ------------------------------------------------------------- diversos */

/* Log de depuracao do port (o osSyncPrintf do jogo, com DEBUG_FEATURES=0, nao imprime). */
void os_log(const char* fmt, ...) {
    if (os_debug_log) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
}
