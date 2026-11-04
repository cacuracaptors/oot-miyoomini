/*
 * os_core.h -- camada de "sistema operacional do N64" do port de Ocarina of Time
 * para o Miyoo Mini Plus (parte 1: threads, filas de mensagem, timers, tempo e
 * eventos de hardware).
 *
 * As funcoes osXxx() que o jogo chama estao declaradas nos cabecalhos do proprio
 * decomp (include/ultra64.h). Este arquivo so declara o que o RESTO DO PORT
 * (video, audio, leitura da ROM, renderizador) usa para falar com essa camada.
 */
#ifndef OS_CORE_H
#define OS_CORE_H

#include <stdint.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif
#include "ultra64/ultratypes.h"
#include "ultra64/thread.h"
#include "ultra64/message.h"
#include "ultra64/time.h"
#include "ultra64/exception.h"

/*
 * Prototipos das funcoes osXxx implementadas em os_core.c (mesmos do decomp).
 * Ficam aqui para os_core.c e os testes nao precisarem incluir ultra64.h, que
 * conflita com os cabecalhos do sistema (bzero etc.).
 */
void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count);
s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag);
s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag);
s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag);
void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg);
s32 osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg);
s32 osStopTimer(OSTimer* timer);
OSTime osGetTime(void);
void osSetTime(OSTime time);
OSIntMask osSetIntMask(OSIntMask);
OSIntMask osGetIntMask(void);
s32 __osDisableInt(void);
void __osRestoreInt(s32);
void osCreateThread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri);
void osStartThread(OSThread* thread);
void osStopThread(OSThread* thread);
void osDestroyThread(OSThread* thread);
void osSetThreadPri(OSThread* thread, OSPri pri);
OSPri osGetThreadPri(OSThread* thread);
OSId osGetThreadId(OSThread* thread);
void osYieldThread(void);

/* Contador do N64: 46,875 MHz (OS_CPU_COUNTER do decomp). */
#define OSCORE_TICKS_PER_SEC 46875000ull

/* 1 = osSyncPrintf escreve em stderr. Por padrao 0 (o jogo de varejo nao imprime). */
extern int os_debug_log;
void os_log(const char* fmt, ...);

/*
 * Chamar UMA vez, na thread principal do processo, antes de bootproc().
 * A thread principal passa a ser a "thread de boot" do N64.
 */
void os_platform_init(void);

/*
 * Chamar depois que bootproc() retornar. A thread principal vira a thread
 * ociosa: fica entregando eventos de hardware e nunca retorna.
 */
void os_platform_idle_forever(void);

/* ---- para threads do PORT (renderizador, audio, leitura de arquivos) ---- */

/* Envia sem bloquear (como uma interrupcao) e acorda o jogo. Pode ser chamada de
 * qualquer pthread que NAO seja uma thread do jogo. */
void os_async_send(OSMesgQueue* mq, OSMesg msg);

/* Roda fn(arg) dentro do jogo, num ponto seguro (por exemplo o pulso de vsync do
 * painel). Mesma regra de chamada. */
void os_async_call(void (*fn)(void*), void* arg);

/* Afinidade de nucleo (Linux). cpu < 0 = nao prende. */
void os_pin_cpu(int cpu);
void os_set_game_cpu(int cpu);

/* Dispara um evento de hardware (OS_EVENT_SP, OS_EVENT_DP, ...): envia a mensagem
 * registrada com osSetEventMesg(). Mesma regra de chamada. */
void os_async_event(OSEvent e);

/* ---- para codigo do port que roda DENTRO do jogo (ja com o trinco global) ---- */

/* Envio sem bloquear e sem trocar de thread. Devolve 0 se coube, -1 se a fila
 * estava cheia. */
int os_send_nb(OSMesgQueue* mq, OSMesg msg);

/* Idem os_async_event, mas para uso dentro do jogo. */
void os_event_fire(OSEvent e);

/* Tarefa periodica de "hardware" (por exemplo o retrace do video a 60 Hz). A funcao
 * roda dentro do jogo (com o trinco) em pontos seguros, e pode chamar os_send_nb()
 * e os_event_fire(). Devolve um id (>= 0) ou -1 se nao ha vaga. */
int os_periodic_add(uint64_t period_ns, void (*fn)(void*), void* arg);
void os_periodic_remove(int id);

/* Diagnostico: estado de cada thread do jogo; sig != 0 manda esse sinal a cada uma. */
void os_dump_threads(int sig);

/* Relogio monotonico em nanossegundos. */
uint64_t os_now_ns(void);
/* prioridade da thread que chama (nice do Linux: menor = mais prioridade) */
void os_thread_nice(int nice);
/* classe da thread que chama: 0 = normal, OS_RT_* = tempo real (veja os_core.c) */
#define OS_RT_DRAW 1  /* desenho (e a auxiliar do desenho, que segue a mesma classe) */
#define OS_RT_GAME 2  /* threads do jogo e a do cartucho (PI) */
#define OS_RT_AUDIO 3 /* tarefa de som (HLE) */
#define OS_RT_VIDEO 4 /* tela: vsync e relogio do jogo */
#define OS_RT_AOUT 5  /* entrega do som ao hardware */
void os_thread_class(int level);
extern int os_rt; /* opcoes.txt prioridade_real */
extern int os_rt_draw; /* opcoes.txt desenho_tempo_real */
extern uint64_t os_idle_ns; /* tempo sem nenhuma thread do jogo rodando */
extern uint64_t os_max_run_ns; /* maior trecho de uma thread do jogo sem ceder a vez (zerado por quem le) */
extern int os_max_run_tid;
extern uint32_t os_msg_drops;   /* mensagens perdidas (fila cheia, envio sem bloqueio) */
extern uint64_t os_wake_max_ns, os_wake_sum_ns; /* demora para a thread que recebeu a vez rodar */
extern uint32_t os_wake_n;
extern void* os_drop_mq[8];     /* filas que perderam mensagens e quantas */
extern uint32_t os_drop_cnt[8];

#endif /* OS_CORE_H */
