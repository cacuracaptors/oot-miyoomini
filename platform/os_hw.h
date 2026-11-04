/*
 * os_hw.h -- "hardware" do N64 do port de Ocarina of Time (parte 2):
 * video (VI), leitura da ROM e do save (PI), tarefas do RCP (SP/DP), audio (AI),
 * controles (SI) e as funcoes de cache/memoria.
 *
 * O jogo continua chamando as funcoes osXxx() de sempre. O que cada uma faz de
 * verdade (desenhar, tocar, ler arquivo...) e decidido pelas funcoes de "gancho"
 * em port_hooks, que o programa principal preenche. Sem ganchos, tudo funciona em
 * modo mudo (sem imagem, som nem controles), o que serve para testar no PC.
 */
#ifndef OS_HW_H
#define OS_HW_H

#include "os_core.h"
#include "ultra64/pi.h"
#include "ultra64/vi.h"
#include "ultra64/sptask.h"
#include "ultra64/controller.h"
#include "ultra64/pfs.h"
#include "ultra64/motor.h"

/* Botoes do N64 (campo OSContPad.button) */
#define PORT_BTN_A       0x8000
#define PORT_BTN_B       0x4000
#define PORT_BTN_Z       0x2000
#define PORT_BTN_START   0x1000
#define PORT_BTN_DUP     0x0800
#define PORT_BTN_DDOWN   0x0400
#define PORT_BTN_DLEFT   0x0200
#define PORT_BTN_DRIGHT  0x0100
#define PORT_BTN_L       0x0020
#define PORT_BTN_R       0x0010
#define PORT_BTN_CUP     0x0008
#define PORT_BTN_CDOWN   0x0004
#define PORT_BTN_CLEFT   0x0002
#define PORT_BTN_CRIGHT  0x0001

#define PORT_SRAM_SIZE 0x8000

typedef struct PortHooks {
    /* --- video: roda dentro do jogo, no retrace, SO quando o quadro mostrado muda
     *     (ou o "preto" liga/desliga). Deve ser rapida (copiar ponteiro, avisar). --- */
    void (*vi_present)(const void* framebuffer, const OSViMode* mode, int blank);

    /* --- RCP: rodam em threads do port, NUNCA dentro do jogo ---
     * gfx_task: executa a lista de comandos graficos. 'resume' != 0 se e a
     *   continuacao de uma tarefa que cedeu. Deve olhar *yield_req de vez em quando
     *   (use __atomic_load_n(yield_req, __ATOMIC_ACQUIRE)) e, se ligado, parar num ponto seguro e devolver 0 (cedeu). Devolve 1 ao
     *   terminar. */
    int (*gfx_task)(OSTask* task, int resume, volatile int* yield_req);
    int (*audio_task)(OSTask* task); /* sintetiza o audio; devolve 1 */
    int (*jpeg_task)(OSTask* task);  /* decodifica JPEG; devolve 1 */

    /* --- audio (AI) --- */
    int (*ai_submit)(const void* pcm, uint32_t bytes); /* 0 ok, -1 fila cheia */
    uint32_t (*ai_remaining)(void);                    /* bytes ainda nao tocados */
    void (*ai_set_freq)(uint32_t hz);

    /* --- ROM (cartucho virtual) e save --- */
    int (*rom_read)(uint32_t offset, void* dst, uint32_t size); /* 0 ok */
    int (*sram_rw)(int write, uint32_t offset, void* buf, uint32_t size);

    /* --- controles: preenche os 4 OSContPad; port 0 = jogador 1 --- */
    void (*read_pads)(OSContPad pads[4]);
    int pads_connected; /* quantos controles existem (1..4) */

    /* --- RCP: roda na thread do desenho logo DEPOIS de o jogo saber que a tarefa grafica acabou
     *     (trabalho adiado: imagens intermediarias que so aparecem mais tarde). Pode ser NULL. --- */
    void (*gfx_post)(void);

    /* --- Rumble Pak no controle 1: liga/desliga o motor (o jogo faz a intensidade ligando e desligando a cada
     *     quadro). NULL ou rumble_on() == 0: sem Rumble Pak. Pode ser NULL. --- */
    void (*rumble)(int on);
    int (*rumble_on)(void);
} PortHooks;

extern PortHooks port_hooks;


/* Prototipos das funcoes osXxx implementadas em os_hw.c (mesmos do decomp). */
void osCreateViManager(OSPri pri);
void osViSetMode(OSViMode* mode);
void osViBlack(u8 active);
void osViSwapBuffer(void* frameBufPtr);
void* osViGetNextFramebuffer(void);
void* osViGetCurrentFramebuffer(void);
void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount);
void osViSetSpecialFeatures(u32 func);
void osViSetXScale(f32 value);
void osViSetYScale(f32 scale);
void osViExtendVStart(u32 value);
void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQueue, OSMesg* cmdBuf, s32 cmdMsgCnt);
s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction);
s32 osEPiReadIo(OSPiHandle* handle, u32 devAddr, u32* data);
s32 osEPiWriteIo(OSPiHandle* handle, u32 devAddr, u32 data);
OSPiHandle* osCartRomInit(void);
OSPiHandle* osDriveRomInit(void);
void osSpTaskLoad(OSTask* task);
void osSpTaskStartGo(OSTask* task);
void osSpTaskYield(void);
OSYieldResult osSpTaskYielded(OSTask* task);
s32 osAiSetFrequency(u32 frequency);
u32 osAiGetLength(void);
s32 osAiSetNextBuffer(void* buf, u32 size);
s32 osContInit(OSMesgQueue* mq, u8* ctlBitfield, OSContStatus* status);
s32 osContStartQuery(OSMesgQueue* mq);
void osContGetQuery(OSContStatus* data);
s32 osContStartReadData(OSMesgQueue* mq);
void osContGetReadData(OSContPad* contData);
s32 osContSetCh(u8 ch);
s32 osPfsInitPak(OSMesgQueue* queue, OSPfs* pfs, s32 channel);
u32 osGetCount(void);
u32 osGetMemSize(void);
u32 osVirtualToPhysical(void* vaddr);
void osInvalDCache(void* vaddr, s32 nbytes);
void osWritebackDCache(void* vaddr, s32 nbytes);

/* IO_READ/IO_WRITE do jogo (rcp.h do port) chamam estas duas. */
unsigned int port_io_read(unsigned int addr);
void port_io_write(unsigned int addr, unsigned int value);

/* ---- ajudantes prontos (implementacao padrao dos ganchos de ROM/save) ---- */

/* Abre o "cartucho virtual" (arquivo) com mmap. Devolve 0 se deu certo. */
int os_hw_rom_open(const char* path);
/* Ou usa um bloco de memoria (testes). O bloco NAO e copiado. */
void os_hw_rom_set_memory(const void* data, uint32_t size);
uint32_t os_hw_rom_size(void);

/* Save em arquivo (32 KiB). Se path for NULL, fica so na memoria. */
int os_hw_sram_open(const char* path);

/* Mapeia a RAM do N64 em 0x80000000 (osMemSize bytes). Chamar uma vez, no inicio,
 * depois de os_hw_set_mem_size(). Devolve 0 se deu certo. */
int os_hw_ram_map(void);

/* Quantidade de RAM que o jogo "enxerga" (osMemSize). Padrao 4 MiB. */
void os_hw_set_mem_size(uint32_t bytes);

/* Divisao dos nucleos (Miyoo Mini Plus tem 2): o jogo inteiro num, o desenho no outro,
 * como no Mario Kart 64. Use -1 para "livre". Chamar no inicio, antes de criar threads. */
void os_hw_set_cpus(int game_cpu, int gfx_cpu, int audio_cpu);

/* Relogio de video externo (vsync do painel). Veja os_hw.c. */
void os_hw_vi_set_external(int ext);
void os_hw_vi_pulse(void);

/* Estatisticas, para o programa principal e os testes. */
typedef struct OsHwStats {
    uint32_t retraces;       /* retraces de video entregues */
    uint32_t presents;       /* chamadas de vi_present */
    uint32_t dma_reads;      /* DMAs de leitura atendidos */
    uint64_t dma_bytes;
    uint32_t gfx_tasks, gfx_yields, audio_tasks, jpeg_tasks, other_tasks;
    uint64_t gfx_ns, gfx_ns_max;     /* tempo gasto nas tarefas graficas (total e pior caso) */
    uint64_t audio_ns, audio_ns_max;
    uint64_t jpeg_ns;
    uint64_t gfx_cpu_ns;             /* tempo de CPU de verdade da thread do desenho */
} OsHwStats;
const OsHwStats* os_hw_stats(void);

#endif /* OS_HW_H */
