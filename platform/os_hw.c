/*
 * os_hw.c -- "hardware" do N64 (parte 2): VI, PI, SP/DP, AI, SI, cache e memoria.
 * Veja os_hw.h. Tudo que o jogo chama aqui e osXxx() do libultra; o trabalho de
 * verdade (imagem, som, arquivos) fica nos ganchos de port_hooks.
 *
 * Regra de ouro: as funcoes osXxx() rodam dentro do jogo (com o trinco global do
 * os_core). As threads auxiliares deste arquivo (leitura da ROM, tarefas do RCP)
 * so conversam com o jogo por os_async_send()/os_async_event().
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "perf_trace.h"
#include "os_hw.h"
#include "ultra64/os_system.h"
#include "ultra64/exception.h"

#define PHYS_ROM_BASE  0x10000000u
#define PHYS_ROM_END   0x1FC00000u
#define PHYS_SRAM_BASE 0x08000000u
#define PHYS_64DD_BASE 0x05000000u
#define PHYS_64DD_END  0x06000000u

#define SP_STATUS_HALT_ 0x0001u

static OsHwStats g_stats;
static int g_ram_mapped;
#define STAT_ADD(f, n) __atomic_fetch_add(&g_stats.f, (n), __ATOMIC_RELAXED)

static void stat_max(uint64_t* p, uint64_t v) {
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(p, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

const OsHwStats* os_hw_stats(void) {
    return &g_stats;
}

/* ------------------------------------------------------------ variaveis do SO */

s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0; /* 0 = reset a frio */
s32 osCicId = 6105;
u32 osMemSize = 0x400000;
s32 osAppNMIBuffer[0x10];
OSPiHandle* __osPiTable;
OSPiHandle* __osCurrentHandle[2];

void os_hw_set_mem_size(uint32_t bytes) {
    osMemSize = bytes;
}

/*
 * A RAM do N64 vive em 0x80000000 (KSEG0). O jogo escreve enderecos fixos la
 * (framebuffers em 0x803B5000..., o heap do sistema entre _buffersSegmentEnd e o
 * framebuffer), entao mapeamos esse bloco EXATAMENTE nesse endereco. So as paginas
 * tocadas ocupam memoria de verdade. O codigo e os dados do jogo ficam fora, no
 * executavel normal.
 */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000 /* kernels antigos ignoram o bit e tratam o endereco como dica */
#endif

int os_hw_ram_map(void) {
    void* want = (void*)(uintptr_t)0x80000000u;
    void* m;
    if (g_ram_mapped) {
        return 0;
    }
    m = mmap(want, osMemSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED || m != want) {
        if (m != MAP_FAILED) {
            munmap(m, osMemSize);
        }
        fprintf(stderr, "os_hw: nao consegui mapear a RAM do N64 em 0x80000000\n");
        return -1;
    }
    g_ram_mapped = 1;
    return 0;
}

/* ------------------------------------------------- ganchos padrao (modo mudo) */

static const uint8_t* g_rom;
static uint32_t g_rom_size;
static uint8_t g_sram[PORT_SRAM_SIZE];
static int g_sram_fd = -1;

int os_hw_rom_open(const char* path) {
    struct stat st;
    void* m;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        return -1;
    }
    g_rom = (const uint8_t*)m;
    g_rom_size = (uint32_t)st.st_size;
    return 0;
}

void os_hw_rom_set_memory(const void* data, uint32_t size) {
    g_rom = (const uint8_t*)data;
    g_rom_size = size;
}

uint32_t os_hw_rom_size(void) {
    return g_rom_size;
}

static int def_rom_read(uint32_t off, void* dst, uint32_t n) {
    if (g_rom == NULL || off >= g_rom_size) {
        memset(dst, 0, n);
        return -1;
    }
    if (n > g_rom_size - off) {
        uint32_t ok = g_rom_size - off;
        memcpy(dst, g_rom + off, ok);
        memset((uint8_t*)dst + ok, 0, n - ok);
        return -1;
    }
    memcpy(dst, g_rom + off, n);
    return 0;
}

int os_hw_sram_open(const char* path) {
    ssize_t r;
    if (g_sram_fd >= 0) {
        close(g_sram_fd);
        g_sram_fd = -1;
    }
    if (path == NULL) {
        return 0;
    }
    g_sram_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g_sram_fd < 0) {
        return -1;
    }
    memset(g_sram, 0, sizeof g_sram);
    r = pread(g_sram_fd, g_sram, sizeof g_sram, 0);
    (void)r;
    return 0;
}

static int def_sram_rw(int write, uint32_t off, void* buf, uint32_t n) {
    if (off >= PORT_SRAM_SIZE || n > PORT_SRAM_SIZE - off) {
        if (!write) {
            memset(buf, 0, n);
        }
        return -1;
    }
    if (write) {
        memcpy(g_sram + off, buf, n);
        if (g_sram_fd >= 0) {
            if (pwrite(g_sram_fd, g_sram + off, n, off) == (ssize_t)n) {
                fdatasync(g_sram_fd);
            }
        }
    } else {
        memcpy(buf, g_sram + off, n);
    }
    return 0;
}

static void def_vi_present(const void* fb, const OSViMode* m, int blank) {
    (void)fb;
    (void)m;
    (void)blank;
}
static int def_gfx_task(OSTask* t, int resume, volatile int* y) {
    (void)t;
    (void)resume;
    (void)y;
    return 1;
}
static int def_task(OSTask* t) {
    (void)t;
    return 1;
}

/* Audio simulado: consome 'freq' amostras estereo por segundo, como o AI de verdade.
 * Serve para o jogo ter o relogio de audio mesmo sem saida de som. */
static struct {
    uint32_t freq;
    uint64_t start[2], end[2];
    uint32_t bytes[2];
    int head;
} g_ai = { 32000, { 0 }, { 0 }, { 0 }, 0 };

static int def_ai_submit(const void* pcm, uint32_t n) {
    uint64_t now = os_now_ns();
    uint64_t begin = now;
    int i, used = 0, last = -1;
    (void)pcm;
    for (i = 0; i < 2; i++) {
        if (g_ai.end[i] > now) {
            used++;
            if (g_ai.end[i] > begin) {
                begin = g_ai.end[i];
                last = i;
            }
        }
    }
    (void)last;
    if (used >= 2) {
        return -1;
    }
    for (i = 0; i < 2; i++) {
        int k = (g_ai.head + i) & 1;
        if (g_ai.end[k] <= now) {
            g_ai.start[k] = begin;
            g_ai.end[k] = begin + (uint64_t)n * 1000000000ull / ((uint64_t)g_ai.freq * 4ull);
            g_ai.bytes[k] = n;
            g_ai.head = k ^ 1;
            return 0;
        }
    }
    return -1;
}
static uint32_t def_ai_remaining(void) {
    uint64_t now = os_now_ns();
    int i;
    for (i = 0; i < 2; i++) {
        if (g_ai.start[i] <= now && g_ai.end[i] > now) {
            return (uint32_t)((g_ai.end[i] - now) * ((uint64_t)g_ai.freq * 4ull) / 1000000000ull) & ~7u;
        }
    }
    return 0;
}
static void def_ai_set_freq(uint32_t hz) {
    g_ai.freq = hz;
}
static void def_read_pads(OSContPad pads[4]) {
    memset(pads, 0, 4 * sizeof(OSContPad));
}

PortHooks port_hooks = {
    def_vi_present, def_gfx_task, def_task, def_task, def_ai_submit, def_ai_remaining,
    def_ai_set_freq, def_rom_read, def_sram_rw, def_read_pads, 1,
};

/* ---------------------------------------------------------------------- VI */

#define VI_PERIOD_NS 16683333ull /* 59,94 Hz */

static struct {
    int created;
    int periodic_id;
    OSViMode* mode;
    const void* cur_fb;
    const void* next_fb;
    int swap_pending;
    int black;
    int black_shown;
    int first_present;
    u32 features;
    f32 xscale, yscale;
    u32 vstart_extend;
    OSMesgQueue* mq;
    OSMesg msg;
    u32 retrace_count;
    u32 retrace_left;
} g_vi = { 0, -1, NULL, NULL, NULL, 0, 1, -1, 1, 0, 1.0f, 1.0f, 0, NULL, NULL, 1, 1 };

static void vi_tick(void* unused) {
    int changed = 0;
    (void)unused;
    STAT_ADD(retraces, 1);
    if (g_vi.swap_pending) {
        g_vi.cur_fb = g_vi.next_fb;
        g_vi.swap_pending = 0;
        changed = 1;
    }
    if (g_vi.black != g_vi.black_shown) {
        g_vi.black_shown = g_vi.black;
        changed = 1;
    }
    if (changed || g_vi.first_present) {
        g_vi.first_present = 0;
        STAT_ADD(presents, 1);
        port_hooks.vi_present(g_vi.cur_fb, g_vi.mode, g_vi.black);
    }
    if (g_vi.retrace_left > 0) {
        g_vi.retrace_left--;
    }
    if (g_vi.retrace_left == 0) {
        if (g_vi.mq != NULL) {
            os_send_nb(g_vi.mq, g_vi.msg);
        }
        g_vi.retrace_left = g_vi.retrace_count != 0 ? g_vi.retrace_count : 1;
    }
}

static int g_vi_external;
static int g_vi_pulses_pending;

static void vi_pulse_run(void* unused) {
    (void)unused;
    __atomic_fetch_sub(&g_vi_pulses_pending, 1, __ATOMIC_RELAXED);
    if (g_vi.created) {
        vi_tick(NULL);
    }
}

/* O relogio do jogo pode seguir o vsync REAL do painel (no Miyoo ele bate a ~16,75 ms,
 * nao 16,67): a thread de video chama os_hw_vi_pulse() a cada vsync. Com ext != 0 o
 * relogio interno de 59,94 Hz e desligado. Chamar antes de osCreateViManager. */
void os_hw_vi_set_external(int ext) {
    g_vi_external = ext;
    if (ext && g_vi.periodic_id >= 0) {
        os_periodic_remove(g_vi.periodic_id);
        g_vi.periodic_id = -1;
    } else if (!ext && g_vi.created && g_vi.periodic_id < 0) {
        g_vi.periodic_id = os_periodic_add(VI_PERIOD_NS, vi_tick, NULL);
    }
}

/* Pode ser chamada de qualquer thread. Se o jogo estiver atrasado, no maximo 8
 * pulsos ficam acumulados (o resto e descartado, como um retrace perdido). */
void os_hw_vi_pulse(void) {
    if (!g_vi_external) {
        return;
    }
    if (__atomic_load_n(&g_vi_pulses_pending, __ATOMIC_RELAXED) >= 8) {
        return;
    }
    __atomic_fetch_add(&g_vi_pulses_pending, 1, __ATOMIC_RELAXED);
    os_async_call(vi_pulse_run, NULL);
}

void osCreateViManager(OSPri pri) {
    (void)pri;
    if (g_vi.created) {
        return;
    }
    g_vi.created = 1;
    if (!g_vi_external) {
        g_vi.periodic_id = os_periodic_add(VI_PERIOD_NS, vi_tick, NULL);
    }
}

void osViSetMode(OSViMode* mode) {
    g_vi.mode = mode;
}

void osViBlack(u8 active) {
    g_vi.black = active ? 1 : 0;
}

void osViSwapBuffer(void* fb) {
    g_vi.next_fb = fb;
    g_vi.swap_pending = 1;
}

void* osViGetNextFramebuffer(void) {
    return (void*)g_vi.next_fb;
}

void* osViGetCurrentFramebuffer(void) {
    return (void*)g_vi.cur_fb;
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    g_vi.mq = mq;
    g_vi.msg = msg;
    g_vi.retrace_count = retraceCount != 0 ? retraceCount : 1;
    if (g_vi.retrace_left == 0 || g_vi.retrace_left > g_vi.retrace_count) {
        g_vi.retrace_left = g_vi.retrace_count;
    }
}

void osViSetSpecialFeatures(u32 f) {
    g_vi.features = f;
}
void osViSetXScale(f32 v) {
    g_vi.xscale = v;
}
void osViSetYScale(f32 v) {
    g_vi.yscale = v;
}
void osViExtendVStart(u32 v) {
    g_vi.vstart_extend = v;
}

/* ---------------------------------------------------------------------- PI */

#define PI_QMAX 128
typedef struct {
    OSPiHandle* h;
    OSIoMesg* mb;
    int dir;
    int copied; /* leitura de ROM ja feita na hora (so falta avisar o fim) */
} PiReq;

static pthread_mutex_t g_pi_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pi_c = PTHREAD_COND_INITIALIZER;
static PiReq g_pi_q[PI_QMAX];
static int g_pi_head, g_pi_n;
static int g_pi_started;

static uint32_t pi_phys(const OSPiHandle* h, uint32_t dev) {
    return (((uint32_t)h->baseAddress) | dev) & 0x1FFFFFFFu;
}

/* le a ROM agora (memoria mapeada: pode ser de qualquer thread) */
/* texturas novas (port/gfx/gfx_hd.c): de onde veio cada pedaco da RAM. Fraco: os testes ligam sem ele. */
__attribute__((weak)) void port_rom_note(uint32_t off, void* ram, uint32_t n) {
    (void)off;
    (void)ram;
    (void)n;
}

static void pi_rom_copy(uint32_t phys, void* ram, uint32_t n) {
    if (port_hooks.rom_read(phys - PHYS_ROM_BASE, ram, n) != 0 && os_debug_log) {
        fprintf(stderr, "os_hw: leitura de ROM falhou off=%08x n=%u\n", phys - PHYS_ROM_BASE, n);
    }
    port_rom_note(phys - PHYS_ROM_BASE, ram, n);
    STAT_ADD(dma_reads, 1);
    STAT_ADD(dma_bytes, n);
}

static void pi_do(const PiReq* r) {
    uint32_t phys = pi_phys(r->h, (uint32_t)r->mb->devAddr);
    uint32_t n = (uint32_t)r->mb->size;
    void* ram = r->mb->dramAddr;
    if (r->copied) {
        if (r->mb->hdr.retQueue != NULL) {
            os_async_send(r->mb->hdr.retQueue, (OSMesg)r->mb);
        }
        return;
    }
    os_log("[pi] %s phys=%08x ram=%p n=%u\n", r->dir == OS_READ ? "le" : "escreve", phys, ram, n);
    if (phys >= PHYS_ROM_BASE && phys < PHYS_ROM_END) {
        if (r->dir == OS_READ) {
            pi_rom_copy(phys, ram, n);
        }
    } else if (phys >= PHYS_SRAM_BASE && phys < PHYS_ROM_BASE) {
        port_hooks.sram_rw(r->dir == OS_WRITE, phys - PHYS_SRAM_BASE, ram, n);
    } else if (phys >= PHYS_64DD_BASE && phys < PHYS_64DD_END) {
        if (r->dir == OS_READ) {
            memset(ram, 0, n);
        }
    }
    if (r->mb->hdr.retQueue != NULL) {
        os_async_send(r->mb->hdr.retQueue, (OSMesg)r->mb);
    }
}

static volatile int g_pi_busy; /* pedido sendo atendido pela thread do PI */

/* estado rapido (audio_thread_manager.c): ainda ha leitura pedida ou em andamento? */
int os_hw_pi_pending(void) {
    int n;
    pthread_mutex_lock(&g_pi_m);
    n = g_pi_n + g_pi_busy;
    pthread_mutex_unlock(&g_pi_m);
    return n;
}

static void* pi_main(void* unused) {
    (void)unused;
    os_thread_class(OS_RT_GAME);
    PT_NAME("dma");
    for (;;) {
        PiReq r;
        pthread_mutex_lock(&g_pi_m);
        while (g_pi_n == 0) {
            pthread_cond_wait(&g_pi_c, &g_pi_m);
        }
        r = g_pi_q[g_pi_head];
        g_pi_head = (g_pi_head + 1) % PI_QMAX;
        g_pi_n--;
        g_pi_busy = 1;
        pthread_mutex_unlock(&g_pi_m);
        PT_EV(PT_PI_DMA, 0);
        pi_do(&r);
        PT_EV(PT_PI_DMA + 1, 0);
        pthread_mutex_lock(&g_pi_m);
        g_pi_busy = 0;
        pthread_mutex_unlock(&g_pi_m);
    }
    return NULL;
}

static void pi_start_thread(void) {
    pthread_t th;
    if (g_pi_started) {
        return;
    }
    g_pi_started = 1;
    if (pthread_create(&th, NULL, pi_main, NULL) != 0) {
        fprintf(stderr, "os_hw: nao consegui criar a thread de leitura\n");
        abort();
    }
    pthread_detach(th);
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQueue, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void)pri;
    osCreateMesgQueue(cmdQueue, cmdBuf, cmdMsgCnt);
    pi_start_thread();
}

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    pi_start_thread();
    mb->piHandle = handle;
    mb->hdr.type = (direction == OS_READ) ? OS_MESG_TYPE_DMAREAD : OS_MESG_TYPE_DMAWRITE;
    pthread_mutex_lock(&g_pi_m);
    if (g_pi_n >= PI_QMAX) {
        pthread_mutex_unlock(&g_pi_m);
        fprintf(stderr, "os_hw: fila de DMA cheia\n");
        return -1;
    }
    g_pi_q[(g_pi_head + g_pi_n) % PI_QMAX].h = handle;
    g_pi_q[(g_pi_head + g_pi_n) % PI_QMAX].mb = mb;
    g_pi_q[(g_pi_head + g_pi_n) % PI_QMAX].dir = direction;
    g_pi_q[(g_pi_head + g_pi_n) % PI_QMAX].copied = 0;
    {
        /* Leituras pequenas da ROM (amostras de som, pedacos de dados) sao copiadas aqui mesmo: no
         * N64 o DMA termina em microssegundos, e o jogo conta com isso. O som pede as amostras e
         * monta a lista de comandos no mesmo quadro; se a copia ficasse na fila, o tocador lia o
         * buffer antes de chegar e o som saia mudo. Leituras grandes continuam na thread do PI. */
        uint32_t phys = pi_phys(handle, (uint32_t)mb->devAddr);
        if (direction == OS_READ && phys >= PHYS_ROM_BASE && phys < PHYS_ROM_END && mb->size <= 0x10000) {
            pi_rom_copy(phys, mb->dramAddr, (uint32_t)mb->size);
            g_pi_q[(g_pi_head + g_pi_n) % PI_QMAX].copied = 1;
        }
    }
    g_pi_n++;
    pthread_cond_signal(&g_pi_c);
    pthread_mutex_unlock(&g_pi_m);
    return 0;
}

s32 osEPiReadIo(OSPiHandle* h, u32 devAddr, u32* data) {
    uint32_t phys = pi_phys(h, (uint32_t)devAddr);
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (phys >= PHYS_ROM_BASE && phys < PHYS_ROM_END) {
        port_hooks.rom_read(phys - PHYS_ROM_BASE, b, 4);
    } else if (phys >= PHYS_SRAM_BASE && phys < PHYS_ROM_BASE) {
        port_hooks.sram_rw(0, phys - PHYS_SRAM_BASE, b, 4);
    }
    /* o hardware devolve a palavra de 32 bits em big-endian */
    *data = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
    return 0;
}

s32 osEPiWriteIo(OSPiHandle* h, u32 devAddr, u32 data) {
    uint32_t phys = pi_phys(h, (uint32_t)devAddr);
    uint8_t b[4];
    b[0] = (uint8_t)(data >> 24);
    b[1] = (uint8_t)(data >> 16);
    b[2] = (uint8_t)(data >> 8);
    b[3] = (uint8_t)data;
    if (phys >= PHYS_SRAM_BASE && phys < PHYS_ROM_BASE) {
        port_hooks.sram_rw(1, phys - PHYS_SRAM_BASE, b, 4);
    }
    return 0;
}

static OSPiHandle g_cart_handle;
static OSPiHandle g_drive_handle;

OSPiHandle* osCartRomInit(void) {
    if (g_cart_handle.baseAddress == 0) {
        g_cart_handle.type = 0;
        g_cart_handle.domain = PI_DOMAIN1;
        g_cart_handle.baseAddress = 0xB0000000u;
        g_cart_handle.next = __osPiTable;
        __osPiTable = &g_cart_handle;
    }
    return &g_cart_handle;
}

OSPiHandle* osDriveRomInit(void) {
    if (g_drive_handle.baseAddress == 0) {
        g_drive_handle.type = 5;
        g_drive_handle.domain = PI_DOMAIN1;
        g_drive_handle.baseAddress = 0xA5000000u;
        g_drive_handle.next = __osPiTable;
        __osPiTable = &g_drive_handle;
    }
    return &g_drive_handle;
}

void __osPiCreateAccessQueue(void) {
}
void __osPiGetAccess(void) {
}
void __osPiRelAccess(void) {
}

/* ------------------------------------------------------------------ SP / DP */

typedef struct {
    OSTask* t;
    int resume;
} SpJob;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    SpJob q[16];
    int head, n;
    int started;
    int audio;
} SpWorker;

static SpWorker g_gfxw = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, { { 0, 0 } }, 0, 0, 0, 0 };
static SpWorker g_audw = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, { { 0, 0 } }, 0, 0, 0, 1 };

static struct {
    OSTask* loaded;
    volatile int yield_req;
    int yield_latch; /* a tarefa recebeu pedido de yield */
    int yielded;     /* e parou por causa dele */
} g_sp;

/* "Yield destacado": no N64 o escalonador pede ao RSP para pausar a tarefa grafica
 * (osSpTaskYield) sempre que ha uma tarefa de audio esperando, roda o audio e depois
 * retoma o grafico. Aqui o audio tem a sua propria thread, entao nao ha o que pausar:
 * o jogo ouve "pausou" na hora (evento SP), o desenho continua na thread dele, e quando
 * o jogo "retoma" a tarefa so esperamos ela terminar. Sem isso o audio so rodava depois
 * de cada quadro inteiro (20 vezes por segundo em vez de 60) e o som faltava. */
static pthread_mutex_t g_gx_mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    OSTask* running;  /* tarefa grafica sendo desenhada agora (thread do desenho) */
    int detached;     /* o jogo acha que ela esta pausada */
    int resumed;      /* o jogo ja mandou retomar */
    int done;         /* terminou enquanto o jogo achava que estava pausada */
    OSTask* task;     /* a tarefa destacada */
    OSTask* queued;   /* tarefa grafica na fila, ainda nao comecou (o desenho adiado do quadro anterior
                       * ainda ocupa a thread do desenho) */
} g_gx;

static void sp_run(SpWorker* w, SpJob j) {
    uint32_t type = (uint32_t)j.t->t.type;
    int done = 1;
    uint64_t t0 = os_now_ns(), dt;
    if (w->audio) {
        STAT_ADD(audio_tasks, 1);
        PT_EV(PT_RCP_AUDIO, 0);
        port_hooks.audio_task(j.t);
        PT_EV(PT_RCP_AUDIO + 1, 0);
        dt = os_now_ns() - t0;
        STAT_ADD(audio_ns, dt);
        stat_max(&g_stats.audio_ns_max, dt);
    } else if (type == M_GFXTASK) {
        STAT_ADD(gfx_tasks, 1);
        pthread_mutex_lock(&g_gx_mx);
        g_gx.running = j.t;
        if (g_gx.queued == j.t) {
            g_gx.queued = NULL;
        }
        if (!(g_gx.detached && g_gx.task == j.t)) {
            /* (destacada ainda na fila: continua destacada) */
            g_gx.detached = 0;
        }
        g_gx.done = 0;
        pthread_mutex_unlock(&g_gx_mx);
        {
            struct timespec c0, c1;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
            PT_EV(PT_RCP_GFX, 0);
            done = port_hooks.gfx_task(j.t, j.resume, &g_sp.yield_req);
            PT_EV(PT_RCP_GFX + 1, done);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
            STAT_ADD(gfx_cpu_ns, (uint64_t)(c1.tv_sec - c0.tv_sec) * 1000000000ull + (uint64_t)c1.tv_nsec - (uint64_t)c0.tv_nsec);
        }
        dt = os_now_ns() - t0;
        STAT_ADD(gfx_ns, dt);
        stat_max(&g_stats.gfx_ns_max, dt);
        pthread_mutex_lock(&g_gx_mx);
        g_gx.running = NULL;
        if (g_gx.detached && !g_gx.resumed) {
            g_gx.done = 1; /* o jogo ainda acha que esta pausada: avisa quando ele retomar */
            pthread_mutex_unlock(&g_gx_mx);
            if (port_hooks.gfx_post != NULL) {
                port_hooks.gfx_post();
            }
            return;
        }
        g_gx.detached = 0;
        pthread_mutex_unlock(&g_gx_mx);
        done = 1;
    } else if (type == M_NJPEGTASK) {
        STAT_ADD(jpeg_tasks, 1);
        PT_EV(PT_RCP_OTHER, 0);
        port_hooks.jpeg_task(j.t);
        PT_EV(PT_RCP_OTHER + 1, 0);
        STAT_ADD(jpeg_ns, os_now_ns() - t0);
    } else {
        STAT_ADD(other_tasks, 1);
    }
    if (!w->audio) {
        __atomic_store_n(&g_sp.yielded, !done, __ATOMIC_RELEASE);
        if (!done) {
            STAT_ADD(gfx_yields, 1);
        }
    }
    os_async_event(OS_EVENT_SP);
    if (!w->audio && type == M_GFXTASK && done) {
        os_async_event(OS_EVENT_DP);
        if (port_hooks.gfx_post != NULL) {
            port_hooks.gfx_post(); /* o jogo ja recebeu o "acabou"; o trabalho adiado vem agora */
        }
    }
}

static int g_gfx_cpu = -1;
static int g_audio_cpu = -1;

void os_hw_set_cpus(int game_cpu, int gfx_cpu, int audio_cpu) {
    g_gfx_cpu = gfx_cpu;
    g_audio_cpu = audio_cpu;
    os_set_game_cpu(game_cpu);
}

static void* sp_main(void* p) {
    SpWorker* w = (SpWorker*)p;
    os_pin_cpu(w->audio ? g_audio_cpu : g_gfx_cpu);
    /* o som em tempo real; o desenho tambem, abaixo do jogo, do som e da tela (os_core.c) */
    os_thread_class(w->audio ? OS_RT_AUDIO : OS_RT_DRAW);
    PT_NAME(w->audio ? "rcp_som" : "desenho");
    for (;;) {
        SpJob j;
        pthread_mutex_lock(&w->m);
        while (w->n == 0) {
            pthread_cond_wait(&w->c, &w->m);
        }
        j = w->q[w->head];
        w->head = (w->head + 1) % 16;
        w->n--;
        pthread_mutex_unlock(&w->m);
        sp_run(w, j);
    }
    return NULL;
}

static void sp_submit(SpWorker* w, SpJob j) {
    pthread_mutex_lock(&w->m);
    if (!w->started) {
        pthread_t th;
        w->started = 1;
        if (pthread_create(&th, NULL, sp_main, w) != 0) {
            fprintf(stderr, "os_hw: nao consegui criar a thread do RCP\n");
            abort();
        }
        pthread_detach(th);
    }
    if (w->n >= 16) {
        pthread_mutex_unlock(&w->m);
        fprintf(stderr, "os_hw: fila do RCP cheia\n");
        return;
    }
    w->q[(w->head + w->n) % 16] = j;
    w->n++;
    pthread_cond_signal(&w->c);
    pthread_mutex_unlock(&w->m);
}

void osSpTaskLoad(OSTask* t) {
    g_sp.loaded = t;
}

void osSpTaskStartGo(OSTask* t) {
    SpJob j;
    pthread_mutex_lock(&g_gx_mx);
    if (g_gx.detached && t == g_gx.task && (uint32_t)t->t.type == M_GFXTASK) {
        /* retomada de uma tarefa destacada: ela nunca parou */
        __atomic_store_n(&g_sp.yield_req, 0, __ATOMIC_RELEASE);
        g_sp.yield_latch = 0;
        __atomic_store_n(&g_sp.yielded, 0, __ATOMIC_RELEASE);
        g_gx.resumed = 1;
        if (g_gx.done) {
            g_gx.detached = 0;
            g_gx.done = 0;
            pthread_mutex_unlock(&g_gx_mx);
            os_event_fire(OS_EVENT_SP);
            os_event_fire(OS_EVENT_DP);
            return;
        }
        pthread_mutex_unlock(&g_gx_mx);
        return;
    }
    if ((uint32_t)t->t.type == M_GFXTASK) {
        g_gx.queued = t;
    }
    pthread_mutex_unlock(&g_gx_mx);
    j.t = t;
    j.resume = (t->t.flags & OS_TASK_YIELDED) != 0;
    __atomic_store_n(&g_sp.yield_req, 0, __ATOMIC_RELEASE);
    g_sp.yield_latch = 0;
    __atomic_store_n(&g_sp.yielded, 0, __ATOMIC_RELEASE);
    sp_submit((uint32_t)t->t.type == M_AUDTASK ? &g_audw : &g_gfxw, j);
}

void osSpTaskYield(void) {
    g_sp.yield_latch = 1;
    pthread_mutex_lock(&g_gx_mx);
    if ((g_gx.running != NULL || g_gx.queued != NULL) && (!g_gx.detached || g_gx.resumed)) {
        /* tambem a que ainda esta na fila (atras do desenho adiado do quadro anterior): sem isto o
         * escalonador esperava ela comecar para rodar o som, e o som faltava nas cenas pesadas */
        g_gx.detached = 1;
        g_gx.resumed = 0;
        g_gx.done = 0;
        g_gx.task = g_gx.running != NULL ? g_gx.running : g_gx.queued;
        __atomic_store_n(&g_sp.yielded, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_gx_mx);
        STAT_ADD(gfx_yields, 1);
        os_event_fire(OS_EVENT_SP); /* "pausou": o escalonador ja pode rodar o audio */
        return;
    }
    pthread_mutex_unlock(&g_gx_mx);
    __atomic_store_n(&g_sp.yield_req, 1, __ATOMIC_RELEASE);
}

OSYieldResult osSpTaskYielded(OSTask* task) {
    u32 result = __atomic_load_n(&g_sp.yielded, __ATOMIC_ACQUIRE) ? OS_TASK_YIELDED : 0;
    if (g_sp.yield_latch) {
        task->t.flags |= result;
        task->t.flags &= ~OS_TASK_DP_WAIT;
    }
    return result;
}

u32 __osSpGetStatus(void) {
    return SP_STATUS_HALT_;
}
void __osSpSetStatus(u32 status) {
    (void)status;
}
u32 osDpGetStatus(void) {
    return 0;
}
void osDpSetStatus(u32 status) {
    (void)status;
}

/* Acesso a "registradores" por IO_READ/IO_WRITE (o rcp.h do port aponta para ca). */
unsigned int port_io_read(unsigned int addr) {
    if (addr == 0x000002E8u) {
        return 0xC86E2000u; /* verificacao anti-pirataria do Bg_Zg */
    }
    return 0;
}
void port_io_write(unsigned int addr, unsigned int value) {
    (void)addr;
    (void)value;
}

/* ---------------------------------------------------------------------- AI */

s32 osAiSetFrequency(u32 frequency) {
    port_hooks.ai_set_freq((uint32_t)frequency);
    return (s32)frequency;
}

u32 osAiGetLength(void) {
    return port_hooks.ai_remaining();
}

s32 osAiSetNextBuffer(void* buf, u32 size) {
    return port_hooks.ai_submit(buf, (uint32_t)size) == 0 ? 0 : -1;
}

void Audio_InvalDCache(void* buf, s32 size) {
    (void)buf;
    (void)size;
}

void Audio_WritebackDCache(void* buf, s32 size) {
    (void)buf;
    (void)size;
}

/* ------------------------------------------------------------ controles (SI) */

static OSContPad g_pad_snap[4];

static int pads_n(void) {
    int n = port_hooks.pads_connected;
    return n < 1 ? 1 : (n > 4 ? 4 : n);
}

static int rumble_pak(int ch) {
    return ch == 0 && port_hooks.rumble != NULL && port_hooks.rumble_on != NULL && port_hooks.rumble_on();
}

static void fill_status(OSContStatus* st) {
    int i, n = pads_n();
    for (i = 0; i < 4; i++) {
        if (i < n) {
            st[i].type = CONT_TYPE_NORMAL;
            /* acessorio: so o Rumble Pak no controle 1, se o aparelho vibra e a opcao esta ligada */
            st[i].status = rumble_pak(i) ? CONT_CARD_ON : 0;
            st[i].errno = 0;
        } else {
            st[i].type = 0;
            st[i].status = 0;
            st[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContInit(OSMesgQueue* mq, u8* ctlBitfield, OSContStatus* status) {
    (void)mq;
    *ctlBitfield = (u8)((1 << pads_n()) - 1);
    fill_status(status);
    return 0;
}

s32 osContStartQuery(OSMesgQueue* mq) {
    os_send_nb(mq, NULL);
    return 0;
}

void osContGetQuery(OSContStatus* data) {
    fill_status(data);
}

s32 osContStartReadData(OSMesgQueue* mq) {
    port_hooks.read_pads(g_pad_snap);
    os_send_nb(mq, NULL);
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    int i, n = pads_n();
    for (i = 0; i < 4; i++) {
        if (i < n) {
            pad[i] = g_pad_snap[i];
            pad[i].errno = 0;
        } else {
            memset(&pad[i], 0, sizeof pad[i]);
            pad[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContSetCh(u8 ch) {
    (void)ch;
    return 0;
}

s32 osMotorInit(OSMesgQueue* q, OSPfs* pfs, s32 channel) {
    (void)q;
    if (!rumble_pak(channel)) {
        return PFS_ERR_NOPACK; /* sem Rumble Pak */
    }
    pfs->channel = channel;
    return 0;
}
s32 __osMotorAccess(OSPfs* pfs, s32 vibrate) {
    if (rumble_pak(pfs->channel)) {
        port_hooks.rumble(vibrate != 0);
    } else if (port_hooks.rumble != NULL) {
        port_hooks.rumble(0); /* (a opcao foi desligada com o motor ligado) */
    }
    return 0;
}

/* Controller Pak: nao existe. O jogo salva na SRAM. */
s32 osPfsInitPak(OSMesgQueue* q, OSPfs* pfs, s32 ch) {
    (void)q;
    (void)pfs;
    (void)ch;
    return PFS_ERR_NOPACK;
}
s32 osPfsFreeBlocks(OSPfs* pfs, s32* left) {
    (void)pfs;
    *left = 0;
    return PFS_ERR_NOPACK;
}
s32 osPfsFindFile(OSPfs* p, u16 c, u32 g, u8* gn, u8* en, s32* f) {
    (void)p; (void)c; (void)g; (void)gn; (void)en; (void)f;
    return PFS_ERR_NOPACK;
}
s32 osPfsReadWriteFile(OSPfs* p, s32 f, u8 fl, s32 o, s32 s, u8* d) {
    (void)p; (void)f; (void)fl; (void)o; (void)s; (void)d;
    return PFS_ERR_NOPACK;
}
s32 osPfsAllocateFile(OSPfs* p, u16 c, u32 g, u8* gn, u8* en, s32 sz, s32* f) {
    (void)p; (void)c; (void)g; (void)gn; (void)en; (void)sz; (void)f;
    return PFS_ERR_NOPACK;
}
s32 osPfsDeleteFile(OSPfs* p, u16 c, u32 g, u8* gn, u8* en) {
    (void)p; (void)c; (void)g; (void)gn; (void)en;
    return PFS_ERR_NOPACK;
}
s32 osPfsFileState(OSPfs* p, s32 f, OSPfsState* st) {
    (void)p; (void)f; (void)st;
    return PFS_ERR_NOPACK;
}

/* ------------------------------------------------- cache, memoria, diversos */

void osInvalDCache(void* a, s32 n) {
    (void)a; (void)n;
}
void osInvalICache(void* a, s32 n) {
    (void)a; (void)n;
}
void osWritebackDCache(void* a, s32 n) {
    (void)a; (void)n;
}
void osWritebackDCacheAll(void) {
}

/* Mesma convencao do OS_K0_TO_PHYSICAL do jogo: endereco - 0x80000000 (com volta). */
u32 osVirtualToPhysical(void* v) {
    return (u32)((uint32_t)(uintptr_t)v - 0x80000000u);
}

u32 osGetMemSize(void) {
    return osMemSize;
}

void osInitialize(void) {
}

u32 osGetCount(void) {
    return (u32)((os_now_ns() * 3ull) / 64ull);
}

s32 osAfterPreNMI(void) {
    return 0;
}

static struct {
    s32 (*cb)(void);
    void* sp;
} g_hwintr[16];

void __osSetHWIntrRoutine(OSHWIntr i, s32 (*cb)(void), void* sp) {
    if (i < 16) {
        g_hwintr[i].cb = cb;
        g_hwintr[i].sp = sp;
    }
}
void __osGetHWIntrRoutine(OSHWIntr i, s32 (**cb)(void), void** sp) {
    *cb = (i < 16) ? g_hwintr[i].cb : NULL;
    *sp = (i < 16) ? g_hwintr[i].sp : NULL;
}

void __osSetFpcCsr(u32 v) {
    (void)v;
}
u32 __osGetFpcCsr(void) {
    return 0;
}

static OSThread g_end_marker = { 0 };

OSThread* __osGetActiveQueue(void) {
    g_end_marker.priority = -1;
    g_end_marker.tlnext = NULL;
    return &g_end_marker;
}
OSThread* __osGetCurrFaultedThread(void) {
    return NULL;
}
