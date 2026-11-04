/*
 * audio_miyoo.c -- a saida de som do Miyoo Mini Plus (MI_AO, a saida de audio do
 * proprio chip), como nos ports do Mario Kart 64, Hexen II e OpenLara.
 *
 * O jogo continua "tocando" no AI simulado do port (platform/os_hw.c), que anda no
 * relogio real: e por ele que o jogo decide quanto som gerar a cada quadro. Cada
 * bloco que o jogo entrega ao AI tambem vem para ca (miyoo_audio_push) e entra num
 * anel; uma thread nossa tira do anel, passa da frequencia do jogo (32 kHz no OoT)
 * para 44100 Hz com interpolacao linear e manda blocos ao MI_AO. O passo e ajustado
 * de leve (ate 3,5%, inaudivel) para o anel ficar perto do alvo em vez de secar ou
 * transbordar de vez em quando.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if __has_include(<mi_ao.h>)
#include <mi_ao.h>
#include <mi_sys.h>
#define HAVE_MI_AO 1
#endif

#include "../perf_trace.h"
#include "miyoo.h"
#include "os_core.h"

#define OUT_RATE 44100
#define OUT_FRAMES 512   /* quadros por bloco mandado ao MI_AO (11,6 ms) */
#define OUT_RING 8
#define RING_FRAMES 16384
#define TARGET_MS 90     /* som guardado no anel (folga para os quadros pesados) */

typedef struct {
    int16_t l, r;
} frame_t;

static frame_t ring[RING_FRAMES];
static volatile uint32_t ring_w, ring_r;

/* Volume do sistema: ao ligar o MI_AO o volume ia para 0 dB (quase o maximo) e so voltava ao do sistema quando uma
 * tecla de volume era apertada. O OnionOS guarda o volume (0..20) em /appconfigs/system.json ("vol") e o aplica com
 * a mesma conta do keymon (Onion, src/common/system/volume.h): dB = round(48 * log10(1 + vol)) - 60. */
static int system_volume_db(void) {
    char buf[4096];
    const char* p;
    size_t n;
    int vol;
    FILE* f = fopen("/appconfigs/system.json", "r");
    if (f == NULL) {
        fprintf(stderr, "som: sem /appconfigs/system.json, volume 0 dB\n");
        return 0;
    }
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    p = strstr(buf, "\"vol\"");
    if (p == NULL || (p = strchr(p, ':')) == NULL) {
        fprintf(stderr, "som: volume do sistema nao encontrado, 0 dB\n");
        return 0;
    }
    vol = atoi(p + 1);
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 20) {
        vol = 20;
    }
    {
        int db = (vol > 0 ? (int)lround(48.0 * log10(1.0 + vol)) : 0) - 60;
        if (db > 30) {
            db = 30;
        }
        fprintf(stderr, "som: volume do sistema %d (de 20) = %d dB\n", vol, db);
        return db;
    }
}

static pthread_mutex_t ring_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t src_rate = 32000;
static int started, running;
static volatile uint32_t st_frames, st_peak; /* (-perf) quadros recebidos e o pico do sinal */

void miyoo_audio_push(const int16_t* pcm, uint32_t frames, uint32_t rate) {
    uint32_t w, i;
    if (!started) {
        return;
    }
    pthread_mutex_lock(&ring_mx);
    if (rate >= 8000 && rate <= 64000) {
        src_rate = rate;
    }
    w = ring_w;
    for (i = 0; i < frames; i++) {
        int16_t l = pcm[i * 2], r = pcm[i * 2 + 1];
        uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        ring[(w + i) % RING_FRAMES].l = l;
        ring[(w + i) % RING_FRAMES].r = r;
        if (al > st_peak) st_peak = al;
        if (ar > st_peak) st_peak = ar;
    }
    st_frames += frames;
    w += frames;
    if ((int32_t)(w - ring_r) > RING_FRAMES - 64) {
        ring_r = w - (RING_FRAMES - 64);
    }
    ring_w = w;
    pthread_mutex_unlock(&ring_mx);
}

#ifdef HAVE_MI_AO
static struct {
    MI_S32 (*SysInit)(void);
    MI_S32 (*SetDepth)(MI_SYS_ChnPort_t*, MI_U32, MI_U32);
    MI_S32 (*SetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t*);
    MI_S32 (*GetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t*);
    MI_S32 (*Enable)(MI_AUDIO_DEV);
    MI_S32 (*Disable)(MI_AUDIO_DEV);
    MI_S32 (*EnableChn)(MI_AUDIO_DEV, MI_AO_CHN);
    MI_S32 (*DisableChn)(MI_AUDIO_DEV, MI_AO_CHN);
    MI_S32 (*SendFrame)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t*, MI_S32);
    MI_S32 (*QueryChnStat)(MI_AUDIO_DEV, MI_AO_CHN, MI_AO_ChnState_t*);
    MI_S32 (*SetVolume)(MI_AUDIO_DEV, MI_S32);
} ao;

static MI_AUDIO_Attr_t ao_attr;
static pthread_t ao_thread;
static frame_t ao_blocks[OUT_RING][OUT_FRAMES];
static uint32_t phase; /* posicao entre o quadro atual e o proximo, 16.16 */
static int gap_logs;

static void* open_lib(const char* name) {
    static const char* dirs[] = { "", "/customer/lib/", "/config/lib/", "/lib/", "/usr/lib/" };
    char path[96];
    unsigned i;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        void* h;
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            return h;
        }
    }
    return NULL;
}

static void fill_block(frame_t* out) {
    static int primed;
    static double learned;
    uint32_t r, w, step;
    int32_t fill, target;
    double err, adj;
    int i, dry = 0;

    pthread_mutex_lock(&ring_mx);
    r = ring_r;
    w = ring_w;
    fill = (int32_t)(w - r);
    target = (int32_t)(src_rate * TARGET_MS / 1000);
    if (!primed && fill >= target) {
        primed = 1;
    }
    if (!primed || fill < 2) {
        if (fill < 2 && primed && gap_logs < 50) {
            gap_logs++;
            fprintf(stderr, "som: o anel esvaziou em t=%.0f ms (o jogo nao mandou som a tempo)\n", miyoo_now_ms());
        }
        if (fill < 2) {
            primed = 0;
        }
        pthread_mutex_unlock(&ring_mx);
        memset(out, 0, OUT_FRAMES * sizeof(frame_t));
        return;
    }
    err = (double)(fill - target) / target;
    if (err > 1.0) {
        err = 1.0;
    }
    if (err < -1.0) {
        err = -1.0;
    }
    learned += err * 0.0002;
    if (learned > 0.03) {
        learned = 0.03;
    }
    if (learned < -0.03) {
        learned = -0.03;
    }
    adj = learned + err * 0.005;
    if (adj > 0.035) {
        adj = 0.035;
    }
    if (adj < -0.035) {
        adj = -0.035;
    }
    step = (uint32_t)((double)src_rate / OUT_RATE * (1.0 + adj) * 65536.0);
    for (i = 0; i < OUT_FRAMES; i++) {
        frame_t a, b;
        int32_t f;
        if ((int32_t)(w - r) < 2) {
            dry = 1;
            out[i] = i ? out[i - 1] : ring[r % RING_FRAMES];
            continue;
        }
        a = ring[r % RING_FRAMES];
        b = ring[(r + 1) % RING_FRAMES];
        f = (int32_t)(phase & 0xFFFF);
        out[i].l = (int16_t)(a.l + (((int32_t)b.l - a.l) * f >> 16));
        out[i].r = (int16_t)(a.r + (((int32_t)b.r - a.r) * f >> 16));
        phase += step;
        r += phase >> 16;
        phase &= 0xFFFF;
    }
    ring_r = r;
    pthread_mutex_unlock(&ring_mx);
    if (dry && gap_logs < 50) {
        gap_logs++;
        fprintf(stderr, "som: o anel acabou no meio de um bloco em t=%.0f ms\n", miyoo_now_ms());
    }
}

static void* ao_loop(void* arg) {
    int slot = 0, logs = 0;
    double last_print = 0;
    (void)arg;
    os_thread_class(OS_RT_AOUT); /* entrega do som ao hardware: sempre na frente */
    pt_thread_name("som_saida");
    while (running) {
        if (miyoo_opt_perf) {
            double now = miyoo_now_ms();
            if (last_print == 0) {
                last_print = now;
            } else if (now - last_print >= 2000.0) {
                extern unsigned audio_hle_unknown;
                fprintf(stderr, "perf som: %u quadros/s do jogo (%u Hz), pico %u de 32767, anel %d, comandos desconhecidos %u\n",
                        (unsigned)(st_frames * 1000.0 / (now - last_print)), (unsigned)src_rate, (unsigned)st_peak,
                        (int)(ring_w - ring_r), audio_hle_unknown);
                st_frames = 0;
                st_peak = 0;
                last_print = now;
            }
        }
        frame_t* buf = ao_blocks[slot];
        MI_AUDIO_Frame_t frame;
        slot = (slot + 1) % OUT_RING;
        PT_EV(PT_AO_WRITE, 0);
        fill_block(buf);
        PT_EV(PT_AO_WRITE + 1, 0);
        memset(&frame, 0, sizeof frame);
        frame.eBitwidth = ao_attr.eBitwidth;
        frame.eSoundmode = ao_attr.eSoundmode;
        frame.u32Len = OUT_FRAMES * sizeof(frame_t);
        frame.apVirAddr[0] = buf;
        frame.apVirAddr[1] = NULL;
        /* espera vaga: so alguns blocos na fila do MI_AO (encher a fila ate a borda fazia o
         * MI_AO sobrescrever som ainda nao tocado, no port do Hexen II) */
        while (running) {
            MI_AO_ChnState_t st;
            MI_U32 total, lim;
            memset(&st, 0, sizeof st);
            if (ao.QueryChnStat(0, 0, &st) != MI_SUCCESS) {
                break;
            }
            total = st.u32ChnFreeNum + st.u32ChnBusyNum;
            lim = total > 64 ? frame.u32Len * 3 : 3; /* bytes ou blocos, conforme a versao */
            if (st.u32ChnBusyNum <= lim) {
                break;
            }
            usleep(2000);
        }
        if (ao.SendFrame(0, 0, &frame, 20) != MI_SUCCESS && logs < 5) {
            fprintf(stderr, "som: o MI_AO recusou um bloco\n");
            logs++;
        }
    }
    return NULL;
}

static int ao_start(void) {
    void *hs = open_lib("libmi_sys.so"), *ha = open_lib("libmi_ao.so");
    MI_AUDIO_Attr_t attr;
    if (!hs || !ha) {
        fprintf(stderr, "som: sem libmi_sys/libmi_ao, sem som\n");
        return 0;
    }
    ao.SysInit = dlsym(hs, "MI_SYS_Init");
    ao.SetDepth = dlsym(hs, "MI_SYS_SetChnOutputPortDepth");
    ao.SetPubAttr = dlsym(ha, "MI_AO_SetPubAttr");
    ao.GetPubAttr = dlsym(ha, "MI_AO_GetPubAttr");
    ao.Enable = dlsym(ha, "MI_AO_Enable");
    ao.Disable = dlsym(ha, "MI_AO_Disable");
    ao.EnableChn = dlsym(ha, "MI_AO_EnableChn");
    ao.DisableChn = dlsym(ha, "MI_AO_DisableChn");
    ao.SendFrame = dlsym(ha, "MI_AO_SendFrame");
    ao.QueryChnStat = dlsym(ha, "MI_AO_QueryChnStat");
    ao.SetVolume = dlsym(ha, "MI_AO_SetVolume");
    if (!ao.SetPubAttr || !ao.GetPubAttr || !ao.Enable || !ao.Disable || !ao.EnableChn || !ao.DisableChn ||
        !ao.SendFrame || !ao.QueryChnStat) {
        fprintf(stderr, "som: MI_AO incompleto, sem som\n");
        return 0;
    }
    if (ao.SysInit) {
        ao.SysInit();
    }
    memset(&attr, 0, sizeof attr);
    attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.u32FrmNum = 6;
    attr.u32PtNumPerFrm = OUT_FRAMES;
    attr.u32ChnCnt = 2;
    attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
    attr.eSamplerate = (MI_AUDIO_SampleRate_e)OUT_RATE;
    if (ao.SetPubAttr(0, &attr) != MI_SUCCESS || ao.GetPubAttr(0, &ao_attr) != MI_SUCCESS) {
        fprintf(stderr, "som: MI_AO nao aceitou o formato, sem som\n");
        return 0;
    }
    if (ao.Enable(0) != MI_SUCCESS) {
        fprintf(stderr, "som: MI_AO nao ligou, sem som\n");
        return 0;
    }
    if (ao.EnableChn(0, 0) != MI_SUCCESS) {
        fprintf(stderr, "som: canal do MI_AO nao ligou, sem som\n");
        ao.Disable(0);
        return 0;
    }
    if (ao.SetVolume) {
        ao.SetVolume(0, system_volume_db());
    }
    if (ao.SetDepth) {
        MI_SYS_ChnPort_t port;
        memset(&port, 0, sizeof port);
        port.eModId = E_MI_MODULE_ID_AO;
        port.u32DevId = 0;
        port.u32ChnId = 0;
        port.u32PortId = 0;
        ao.SetDepth(&port, 12, 13);
    }
    running = 1;
    if (pthread_create(&ao_thread, NULL, ao_loop, NULL) != 0) {
        running = 0;
        ao.DisableChn(0, 0);
        ao.Disable(0);
        fprintf(stderr, "som: sem thread de som, sem som\n");
        return 0;
    }
    fprintf(stderr, "som: MI_AO %d Hz estereo\n", OUT_RATE);
    return 1;
}
#endif

void miyoo_audio_init(int mute) {
    if (mute) {
        fprintf(stderr, "som: desligado (-mudo)\n");
        return;
    }
#ifdef HAVE_MI_AO
    started = ao_start();
#else
    fprintf(stderr, "som: compilado sem mi_ao.h, sem som\n");
#endif
}

void miyoo_audio_shutdown(void) {
#ifdef HAVE_MI_AO
    if (started && running) {
        running = 0;
        pthread_join(ao_thread, NULL);
        ao.DisableChn(0, 0);
        ao.Disable(0);
    }
#endif
    started = 0;
}
