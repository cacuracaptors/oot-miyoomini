/* Teste do "hardware" (os_hw): ROM/DMA, save, video, tarefas do RCP, audio, controles. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "os_hw.h"

static int g_fail;
static void check(const char* name, int cond, const char* info) {
    printf("%s  %-40s %s\n", cond ? "OK    " : "FALHOU", name, info);
    if (!cond) {
        g_fail++;
    }
}
static void watchdog(int s) {
    (void)s;
    if (write(2, "FALHOU  watchdog (30 s)\n", 24) < 0) {
    }
    _exit(2);
}

static unsigned char g_stack[64];
static OSThread th[4];
#define NEWT(t, id, fn, arg, pri) osCreateThread(&(t), id, fn, arg, g_stack, pri)

static uint8_t g_romdata[1 << 20];

/* ------------------------------------------------------------- ROM / DMA */
static int g_vi_ticks;
static OSMesgQueue viq, dmaq;
static OSMesg vib[8], dmab[4];

static int slow_rom_read(uint32_t off, void* dst, uint32_t n) {
    usleep(40000); /* cartao SD lento */
    memcpy(dst, g_romdata + off, n);
    return 0;
}
static void t_vi_counter(void* a) {
    OSMesg m;
    (void)a;
    for (;;) {
        osRecvMesg(&viq, &m, OS_MESG_BLOCK);
        g_vi_ticks++;
    }
}
static void test_dma(void) {
    OSPiHandle* h;
    OSIoMesg io;
    OSMesg m;
    uint8_t buf[256];
    u32 w;
    int before;
    unsigned i;
    char info[100];
    OSMesgQueue cmdq;
    OSMesg cmdb[8];

    for (i = 0; i < sizeof g_romdata; i++) {
        g_romdata[i] = (uint8_t)(i * 7 + (i >> 8));
    }
    g_romdata[0x38] = 'N'; g_romdata[0x39] = 0; g_romdata[0x3A] = 0; g_romdata[0x3B] = 0x5A;
    g_romdata[0x3C] = 0; g_romdata[0x3D] = 0x45; g_romdata[0x3E] = 0x12; g_romdata[0x3F] = 0x34;
    os_hw_rom_set_memory(g_romdata, sizeof g_romdata);

    osCreatePiManager(150, &cmdq, cmdb, 8);
    h = osCartRomInit();

    osEPiReadIo(h, 0x3C, &w);
    snprintf(info, sizeof info, "0x%08lx", (unsigned long)w);
    check("osEPiReadIo le palavra big-endian", w == 0x00451234u, info);

    memset(buf, 0xEE, sizeof buf);
    osCreateMesgQueue(&dmaq, dmab, 4);
    memset(&io, 0, sizeof io);
    io.hdr.pri = OS_MESG_PRI_NORMAL;
    io.hdr.retQueue = &dmaq;
    io.dramAddr = buf;
    io.devAddr = 0x1234;
    io.size = sizeof buf;
    osEPiStartDma(h, &io, OS_READ);
    osRecvMesg(&dmaq, &m, OS_MESG_BLOCK);
    check("DMA devolve o proprio OSIoMesg", m == (OSMesg)&io, "");
    check("DMA copiou os bytes certos", memcmp(buf, g_romdata + 0x1234, sizeof buf) == 0, "");

    /* DMA lento nao pode travar o resto do jogo: o contador de retraces segue */
    port_hooks.rom_read = slow_rom_read;
    osCreateMesgQueue(&viq, vib, 8);
    osCreateViManager(254);
    osViSetEvent(&viq, (OSMesg)1, 1);
    NEWT(th[0], 21, t_vi_counter, NULL, 3);
    osStartThread(&th[0]);
    before = g_vi_ticks;
    io.devAddr = 0x8000;
    osEPiStartDma(h, &io, OS_READ);
    osRecvMesg(&dmaq, &m, OS_MESG_BLOCK);
    snprintf(info, sizeof info, "%d retraces durante o DMA de 40 ms", g_vi_ticks - before);
    check("jogo continua durante o DMA", g_vi_ticks - before >= 2 && g_vi_ticks - before <= 4, info);
    check("DMA lento copiou certo", memcmp(buf, g_romdata + 0x8000, sizeof buf) == 0, "");
    osStopThread(&th[0]);
}

/* ------------------------------------------------------------------ SRAM */
static void test_sram(void) {
    OSPiHandle sh;
    OSIoMesg io;
    OSMesg m;
    uint8_t wbuf[64], rbuf[64];
    int i;
    char path[] = "/tmp/os_hw_test_sram.bin";
    OSMesgQueue q;
    OSMesg qb[1];

    unlink(path);
    check("abre arquivo de save", os_hw_sram_open(path) == 0, "");
    memset(&sh, 0, sizeof sh);
    sh.baseAddress = 0xA8000000u; /* OS_PHYSICAL_TO_K1(0x08000000) */
    sh.domain = PI_DOMAIN2;
    osCreateMesgQueue(&q, qb, 1);
    for (i = 0; i < 64; i++) {
        wbuf[i] = (uint8_t)(0xA0 + i);
    }
    memset(&io, 0, sizeof io);
    io.hdr.retQueue = &q;
    io.dramAddr = wbuf;
    io.devAddr = 0x08000000u + 0x100; /* como o SsSram_Init: devAddr = endereco fisico */
    io.size = sizeof wbuf;
    osEPiStartDma(&sh, &io, OS_WRITE);
    osRecvMesg(&q, &m, OS_MESG_BLOCK);

    memset(rbuf, 0, sizeof rbuf);
    io.dramAddr = rbuf;
    osEPiStartDma(&sh, &io, OS_READ);
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    check("SRAM: escreve e le de volta", memcmp(rbuf, wbuf, 64) == 0, "");

    os_hw_sram_open(path); /* reabre: tem que ter ido para o arquivo */
    memset(rbuf, 0, sizeof rbuf);
    osEPiStartDma(&sh, &io, OS_READ);
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    check("SRAM: persiste no arquivo", memcmp(rbuf, wbuf, 64) == 0, "");
    unlink(path);
}

/* ----------------------------------------------------------------- video */
static const void* g_pres_fb[16];
static int g_pres_blank[16];
static int g_pres_n;
static void my_present(const void* fb, const OSViMode* m, int blank) {
    (void)m;
    if (g_pres_n < 16) {
        g_pres_fb[g_pres_n] = fb;
        g_pres_blank[g_pres_n] = blank;
        g_pres_n++;
    }
}
static OSViMode g_mode;
static void test_vi(void) {
    static char fbA[16], fbB[16];
    OSMesg m;
    int n, i;
    uint64_t t0, dt;
    char info[100];

    port_hooks.vi_present = my_present;
    osViSetMode(&g_mode);
    osViBlack(1);
    osViSwapBuffer(fbA);
    check("GetNext logo apos SwapBuffer", osViGetNextFramebuffer() == fbA, "");
    check("GetCurrent ainda nao mudou", osViGetCurrentFramebuffer() != fbA, "");
    /* espera um retrace */
    osCreateMesgQueue(&viq, vib, 8);
    osViSetEvent(&viq, (OSMesg)3, 1);
    osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    check("apos o retrace GetCurrent == A", osViGetCurrentFramebuffer() == fbA, "");
    osViBlack(0);
    osViSwapBuffer(fbB);
    osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    check("apos o retrace GetCurrent == B", osViGetCurrentFramebuffer() == fbB, "");
    n = g_pres_n;
    osRecvMesg(&viq, &m, OS_MESG_BLOCK); /* sem troca: nao apresenta de novo */
    osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    check("so apresenta quando o quadro muda", g_pres_n == n, "");
    check("presenter viu A (preto) depois B (aceso)",
          g_pres_n >= 2 && g_pres_fb[g_pres_n - 2] == fbA && g_pres_blank[g_pres_n - 2] == 1 &&
              g_pres_fb[g_pres_n - 1] == fbB && g_pres_blank[g_pres_n - 1] == 0,
          "");

    /* retraceCount = 3 -> mensagens a ~20 Hz */
    osViSetEvent(&viq, (OSMesg)4, 3);
    while (osRecvMesg(&viq, &m, OS_MESG_NOBLOCK) == 0) {
    }
    osRecvMesg(&viq, &m, OS_MESG_BLOCK); /* sincroniza */
    t0 = os_now_ns();
    for (i = 0; i < 10; i++) {
        osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    }
    dt = (os_now_ns() - t0) / 1000000;
    snprintf(info, sizeof info, "10 mensagens em %llu ms (queria ~500)", (unsigned long long)dt);
    check("retraceCount=3 entrega a 20 Hz", dt >= 480 && dt <= 530 && (long)m == 4, info);
    osViSetEvent(&viq, (OSMesg)3, 1);
}

/* --------------------------------------------------- vsync externo do painel */
static volatile int g_pulse_run;
static void* pulser(void* a) {
    (void)a;
    while (__atomic_load_n(&g_pulse_run, __ATOMIC_ACQUIRE)) {
        usleep(16750); /* painel do Miyoo: 16,75 ms */
        os_hw_vi_pulse();
    }
    return NULL;
}
static void test_vi_external(void) {
    OSMesg m;
    pthread_t p;
    int i;
    uint64_t t0, dt;
    char info[100];
    os_hw_vi_set_external(1);
    osViSetEvent(&viq, (OSMesg)9, 1);
    while (osRecvMesg(&viq, &m, OS_MESG_NOBLOCK) == 0) {
    }
    /* sem pulsos o relogio interno esta desligado: nada chega */
    {
        OSMesgQueue tq;
        OSMesg tb[1];
        OSTimer tm;
        osCreateMesgQueue(&tq, tb, 1);
        osSetTimer(&tm, 60 * 46875ull, 0, &tq, NULL);
        osRecvMesg(&tq, &m, OS_MESG_BLOCK);
        check("vsync externo: sem pulso nao ha retrace", osRecvMesg(&viq, &m, OS_MESG_NOBLOCK) == -1, "");
    }
    __atomic_store_n(&g_pulse_run, 1, __ATOMIC_RELEASE);
    pthread_create(&p, NULL, pulser, NULL);
    osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    t0 = os_now_ns();
    for (i = 0; i < 30; i++) {
        osRecvMesg(&viq, &m, OS_MESG_BLOCK);
    }
    dt = (os_now_ns() - t0) / 1000;
    __atomic_store_n(&g_pulse_run, 0, __ATOMIC_RELEASE);
    pthread_join(p, NULL);
    snprintf(info, sizeof info, "%.3f ms por retrace (painel: 16,75)", dt / 30000.0);
    check("vsync externo: jogo segue o painel", dt / 30 > 16500 && dt / 30 < 17300 && (long)m == 9, info);
    os_hw_vi_set_external(0);
}

/* ------------------------------------------------------------------- RCP */
static int g_gfx_pos, g_gfx_resumed_flag;
static volatile int g_audio_thread_differs;
static pthread_t g_main_pt;
static int test_gfx(OSTask* t, int resume, volatile int* y) {
    int i = resume ? g_gfx_pos : 0;
    (void)t;
    g_gfx_resumed_flag = resume;
    for (; i < 60; i++) {
        if (__atomic_load_n(y, __ATOMIC_ACQUIRE)) {
            g_gfx_pos = i;
            return 0;
        }
        usleep(1000);
    }
    return 1;
}
static int test_audio(OSTask* t) {
    (void)t;
    g_audio_thread_differs = !pthread_equal(pthread_self(), g_main_pt);
    usleep(2000);
    return 1;
}
static void test_post(void) {
    usleep(50000);
}
static void test_rcp(void) {
    static OSTask gfx, aud;
    OSMesgQueue spq, dpq;
    OSMesg spb[4], dpb[4], m;
    u32 y;
    char info[100];

    port_hooks.gfx_task = test_gfx;
    port_hooks.audio_task = test_audio;
    g_main_pt = pthread_self();
    osCreateMesgQueue(&spq, spb, 4);
    osCreateMesgQueue(&dpq, dpb, 4);
    osSetEventMesg(OS_EVENT_SP, &spq, (OSMesg)0x11);
    osSetEventMesg(OS_EVENT_DP, &dpq, (OSMesg)0x22);

    memset(&gfx, 0, sizeof gfx);
    gfx.t.type = M_GFXTASK;
    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    {
        OSMesgQueue tq;
        OSMesg tb[1];
        OSTimer tm;
        osCreateMesgQueue(&tq, tb, 1);
        osSetTimer(&tm, 15 * 46875ull, 0, &tq, NULL);
        osRecvMesg(&tq, &m, OS_MESG_BLOCK);
    }
    osSpTaskYield();
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    y = osSpTaskYielded(&gfx);
    snprintf(info, sizeof info, "yielded=%lu pos=%d", (unsigned long)y, g_gfx_pos);
    /* yield destacado: o jogo ouve "pausou" na hora; o desenho continua na thread dele */
    check("tarefa grafica 'cede' na hora (yield destacado)", y == OS_TASK_YIELDED && g_gfx_pos == 0, info);
    check("flag OS_TASK_YIELDED gravado na tarefa", (gfx.t.flags & OS_TASK_YIELDED) != 0, "");
    check("sem evento DP quando cede", osRecvMesg(&dpq, &m, OS_MESG_NOBLOCK) == -1, "");

    /* enquanto o grafico esta "pausado", o audio roda (na thread dele) */
    memset(&aud, 0, sizeof aud);
    aud.t.type = M_AUDTASK;
    osSpTaskLoad(&aud);
    osSpTaskStartGo(&aud);
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    check("audio roda durante o yield destacado", m == (OSMesg)0x11 && osSpTaskYielded(&aud) == 0, "");

    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    check("retomada: o gancho nao foi chamado de novo (resume=0)", g_gfx_resumed_flag == 0, "");
    check("tarefa termina: osSpTaskYielded == 0", osSpTaskYielded(&gfx) == 0, "");
    osRecvMesg(&dpq, &m, OS_MESG_BLOCK);
    check("evento DP chega depois do SP", m == (OSMesg)0x22, "");

    /* desenho que termina ANTES da retomada: os eventos saem na retomada */
    g_gfx_pos = 0;
    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    usleep(5000);
    osSpTaskYield();
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    usleep(90000); /* o desenho (60 ms) acaba enquanto o jogo acha que esta pausado */
    check("terminou destacado: nenhum evento antes da retomada",
          osRecvMesg(&spq, &m, OS_MESG_NOBLOCK) == -1 && osRecvMesg(&dpq, &m, OS_MESG_NOBLOCK) == -1, "");
    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    check("retomada de tarefa ja terminada: SP e DP na hora",
          osRecvMesg(&spq, &m, OS_MESG_NOBLOCK) == 0 && osRecvMesg(&dpq, &m, OS_MESG_NOBLOCK) == 0, "");
    {
        const OsHwStats* st = os_hw_stats();
        snprintf(info, sizeof info, "gfx %llu ms no total, pior %llu ms", (unsigned long long)(st->gfx_ns / 1000000),
                 (unsigned long long)(st->gfx_ns_max / 1000000));
        check("estatistica de tempo das tarefas graficas", st->gfx_ns > 55000000ull && st->gfx_ns_max > 25000000ull, info);
    }

    memset(&aud, 0, sizeof aud);
    aud.t.type = M_AUDTASK;
    osSpTaskLoad(&aud);
    osSpTaskStartGo(&aud);
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    check("audio roda em thread propria + evento SP", g_audio_thread_differs && m == (OSMesg)0x11, "");
    check("audio nao gera evento DP", osRecvMesg(&dpq, &m, OS_MESG_NOBLOCK) == -1, "");

    /* tarefa grafica ainda na fila, atras de um desenho adiado (gfx_post) do quadro anterior: o pedido de
     * yield tem de responder na hora (senao o som espera o desenho adiado inteiro) */
    port_hooks.gfx_post = test_post;
    g_gfx_pos = 0;
    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    osRecvMesg(&spq, &m, OS_MESG_BLOCK);
    osRecvMesg(&dpq, &m, OS_MESG_BLOCK); /* acabou; agora a thread do desenho faz o "adiado" (50 ms) */
    {
        static OSTask gfx2;
        uint64_t t0;
        memset(&gfx2, 0, sizeof gfx2);
        gfx2.t.type = M_GFXTASK;
        osSpTaskLoad(&gfx2);
        osSpTaskStartGo(&gfx2); /* fica na fila */
        usleep(2000);
        t0 = os_now_ns();
        osSpTaskYield();
        osRecvMesg(&spq, &m, OS_MESG_BLOCK);
        snprintf(info, sizeof info, "%.1f ms", (os_now_ns() - t0) / 1e6);
        check("yield de tarefa na fila (atras do desenho adiado) responde na hora",
              os_now_ns() - t0 < 10000000ull && osSpTaskYielded(&gfx2) == OS_TASK_YIELDED, info);
        osSpTaskLoad(&aud);
        osSpTaskStartGo(&aud);
        osRecvMesg(&spq, &m, OS_MESG_BLOCK);
        check("som roda enquanto o desenho adiado ocupa a thread do desenho", os_now_ns() - t0 < 20000000ull, "");
        osSpTaskLoad(&gfx2);
        osSpTaskStartGo(&gfx2); /* retoma */
        osRecvMesg(&spq, &m, OS_MESG_BLOCK);
        check("retomada da tarefa que estava na fila: termina", osSpTaskYielded(&gfx2) == 0, "");
        osRecvMesg(&dpq, &m, OS_MESG_BLOCK);
        check("DP da tarefa que estava na fila", m == (OSMesg)0x22, "");
    }
    usleep(60000); /* o adiado da 2a tarefa */
    port_hooks.gfx_post = NULL;
}

/* --------------------------------------------------------------- controles */
static void my_pads(OSContPad p[4]) {
    memset(p, 0, 4 * sizeof(OSContPad));
    p[0].button = PORT_BTN_A | PORT_BTN_START;
    p[0].stick_x = 60;
    p[0].stick_y = -70;
}
static void test_pads(void) {
    OSMesgQueue sq;
    OSMesg sb[1], m;
    u8 pat = 0;
    OSContStatus st[4];
    OSContPad pad[4];
    char info[100];

    port_hooks.read_pads = my_pads;
    port_hooks.pads_connected = 1;
    osCreateMesgQueue(&sq, sb, 1);
    osContInit(&sq, &pat, st);
    snprintf(info, sizeof info, "mascara=%d", pat);
    check("osContInit: so o controle 1", pat == 1 && st[0].errno == 0 && st[0].type == CONT_TYPE_NORMAL &&
                                              st[1].errno == CONT_NO_RESPONSE_ERROR, info);
    osContStartQuery(&sq);
    osRecvMesg(&sq, &m, OS_MESG_BLOCK);
    osContGetQuery(st);
    osContStartReadData(&sq);
    osRecvMesg(&sq, &m, OS_MESG_BLOCK);
    osContGetReadData(pad);
    check("osContGetReadData: botoes e analogico",
          pad[0].button == (PORT_BTN_A | PORT_BTN_START) && pad[0].stick_x == 60 && pad[0].stick_y == -70 &&
              pad[0].errno == 0 && pad[1].errno == CONT_NO_RESPONSE_ERROR,
          "");
    check("sem Controller Pak / Rumble", osPfsInitPak(&sq, NULL, 0) == PFS_ERR_NOPACK && osMotorInit(&sq, NULL, 0) != 0, "");
}

/* --------------------------------------------------------------------- AI */
static void test_ai(void) {
    char info[100];
    u32 r;
    int a, b, c;
    osAiSetFrequency(32000);
    a = osAiSetNextBuffer((void*)1, 3200);  /* 800 amostras = 25 ms */
    b = osAiSetNextBuffer((void*)1, 3200);
    c = osAiSetNextBuffer((void*)1, 3200);
    check("AI aceita 2 buffers e recusa o 3o", a == 0 && b == 0 && c == -1, "");
    r = osAiGetLength();
    snprintf(info, sizeof info, "%lu bytes (<=3200)", (unsigned long)r);
    check("osAiGetLength descreve o buffer atual", r > 2000 && r <= 3200, info);
    usleep(30000);
    r = osAiGetLength();
    check("passados 30 ms, toca o 2o buffer", r > 0 && r < 3200, "");
    usleep(40000);
    check("tudo tocado -> 0 bytes", osAiGetLength() == 0, "");
    check("fila livre de novo", osAiSetNextBuffer((void*)1, 3200) == 0, "");
}

/* ------------------------------------------------------------------- misc */
static void test_ram(void) {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)0x803B5000u;
    check("mapeia a RAM do N64 em 0x80000000", os_hw_ram_map() == 0, "");
    p[0] = 0x12345678u;
    p[1000] = 0xCAFEBABEu;
    check("RAM em K0 le e escreve", p[0] == 0x12345678u && p[1000] == 0xCAFEBABEu, "");
    check("mapear de novo e inofensivo", os_hw_ram_map() == 0 && p[0] == 0x12345678u, "");
}

static void test_misc(void) {
    OSTime a;
    u32 c0, c1;
    char info[100];
    a = osGetTime();
    (void)a;
    c0 = osGetCount();
    usleep(100000);
    c1 = osGetCount();
    snprintf(info, sizeof info, "%lu ticks em 100 ms (queria ~4687500)", (unsigned long)(c1 - c0));
    check("osGetCount a 46,875 MHz", (c1 - c0) > 4500000 && (c1 - c0) < 5000000, info);
    check("IO_READ(0x2E8) anti-pirataria", port_io_read(0x2E8) == 0xC86E2000u, "");
    check("osVirtualToPhysical == K0_TO_PHYSICAL", osVirtualToPhysical((void*)0x80123456u) == 0x00123456u, "");
}

int main(void) {
    signal(SIGALRM, watchdog);
    alarm(30);
    os_platform_init();
    os_hw_set_cpus(0, 1, -1); /* como no aparelho: jogo no nucleo 0, desenho no 1 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_dma();
    test_sram();
    test_vi();
    test_vi_external();
    test_rcp();
    test_pads();
    test_ai();
    test_misc();
    test_ram();

    if (g_fail) {
        printf("\n%d TESTE(S) FALHARAM\n", g_fail);
        return 1;
    }
    printf("\nTODOS OS TESTES PASSARAM\n");
    return 0;
}
