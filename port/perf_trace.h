/* perf_trace.h -- medicao detalhada do trabalho dos nucleos (perf_trace.c, opcao perf_trace) */
#ifndef PERF_TRACE_H
#define PERF_TRACE_H

/* codigos dos eventos: par = comeco, impar = fim (o relatorio junta os pares por thread) */
enum {
    PT_GAME_RUN = 2,   /* (arg = id da thread do N64 que passou a rodar; fim = a thread do jogo cedeu) */
    PT_GAME_IDLE = 4,  /* nenhuma thread do N64 pronta: esperando (vsync, DMA, desenho...) */
    PT_RCP_GFX = 10,   /* tarefa de desenho do RCP (arg = 1 terminou, 0 pausou) */
    PT_RCP_AUDIO = 12, /* tarefa de som */
    PT_RCP_OTHER = 14, /* outra (JPEG) */
    PT_DRAW_RASTER = 20, /* desenho: a thread do desenho rasterizando a fila (arg = trabalhos) */
    PT_DRAW_WAIT = 22,   /* desenho: esperando a auxiliar acabar a parte dela */
    PT_DRAW_STEAL = 24,  /* desenho: pegando linhas da auxiliar */
    PT_HELP_RUN = 30,    /* auxiliar rasterizando */
    PT_HELP_SLEEP = 32,  /* auxiliar dormindo esperando trabalho */
    PT_FLIP_PRESENT = 40, /* tela: mostrando uma imagem (arg = vsyncs desde a anterior) */
    PT_FLIP_VSYNC = 42,   /* tela: esperando o vsync */
    PT_FLIP_GE = 44,      /* tela: 2D pelo acelerador (pecas) */
    PT_AO_WRITE = 50,     /* som: entregando ao MI_AO (bloqueia quando o buffer esta cheio) */
    PT_FRAME = 61,        /* (instantaneo) um desenho terminou (arg = tipo: 0..5 como o registro_quadros) */
    PT_MARK = 63,         /* (instantaneo) SELECT+R1 */
    PT_BG_GE = 70,        /* fundo JPEG pelo acelerador */
    PT_PI_DMA = 80,       /* DMA do cartucho */
    PT_HD_LOAD = 90       /* carregando uma textura nova */
};

extern int port_opt_perf_trace;
void pt_init(void);
void pt_thread_name(const char *name);
void pt_ev(int code, int arg);
void pt_dump(void);
void pt_request_dump(void);

/* (nos arquivos que tambem entram no reprodutor de quadros, sem perf_trace.c: chamadas fracas) */
#define PT_EV(code, arg)                                                                                                \
    do {                                                                                                                 \
        extern void pt_ev(int, int) __attribute__((weak));                                                               \
        if (pt_ev) {                                                                                                     \
            pt_ev((code), (arg));                                                                                        \
        }                                                                                                                \
    } while (0)
#define PT_NAME(name)                                                                                                   \
    do {                                                                                                                 \
        extern void pt_thread_name(const char *) __attribute__((weak));                                                  \
        if (pt_thread_name) {                                                                                            \
            pt_thread_name(name);                                                                                        \
        }                                                                                                                \
    } while (0)

#endif
