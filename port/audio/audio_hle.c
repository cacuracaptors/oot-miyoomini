/*
 * audio_hle.c -- o microcodigo de audio do Ocarina of Time (ABI "nead") em C.
 *
 * Tabela de comandos e significado de cada um: os mesmos do HLE do mupen64plus
 * (rsp-hle: alist_nead.c, alist_process_nead_oot, e alist.c), que reproduz o som do
 * jogo como o RSP. A diferenca e a memoria: o mupen guarda a RAM e a DMEM em palavras
 * de 32 bits trocadas (por isso os indices "^ S" dele); aqui tudo fica em ordem
 * natural, com as amostras de 16 bits em endian nativo. Os indices que o mupen embaralha
 * por causa disso (o filtro, o interleave) estao escritos aqui ja desembaralhados.
 *
 * Por que funciona sem trocar bytes: tudo de 16 bits que estes comandos leem da RAM
 * ou foi escrito por eles mesmos (estados do ADPCM, do resampler, do filtro, buffers de
 * reverb), ou e dado nativo do executavel (ondas sinteticas, filtros), ou vem das
 * soundfonts que o port converte ao carregar (codebooks e estados de loop: veja
 * audio_font.c). As amostras comprimidas (ADPCM, 8 bits) sao lidas byte a byte.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_hle.h"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define AHLE_NEON 1 /* versoes NEON dos comandos que mais pesam: mesmo resultado, bit a bit (tests/test_audio_hle.c) */
#else
#define AHLE_NEON 0
#endif

uint8_t* audio_hle_ram = (uint8_t*)(uintptr_t)0x80000000u;
unsigned audio_hle_unknown;

/* diagnostico (audio_hle_report): quantos comandos de cada tipo e o maior valor visto em cada
 * etapa (amostras decodificadas, entrada do mixer, o que foi salvo na RAM) */
static unsigned g_cnt[32], g_tasks, g_envNonzero;
static unsigned g_bookEntries, g_frames, g_badPred; /* teste dos dados ADPCM: indice de preditor fora do livro */
static int16_t g_book0[8];
static int g_peakDec, g_peakEnvIn, g_peakSave;
static inline int peak16(const int16_t* p, unsigned n, int pk) {
    unsigned i = 0;
#if AHLE_NEON
    if (n >= 8) {
        /* |x| lido como 16 bits sem sinal: vabs(-32768) = 0x8000 = 32768, o valor certo */
        uint16x8_t m = vdupq_n_u16(0);
        for (; i + 8 <= n; i += 8) {
            m = vmaxq_u16(m, vreinterpretq_u16_s16(vabsq_s16(vld1q_s16(p + i))));
        }
        {
            uint16x4_t h = vmax_u16(vget_low_u16(m), vget_high_u16(m));
            h = vpmax_u16(h, h);
            h = vpmax_u16(h, h);
            if ((int)vget_lane_u16(h, 0) > pk) {
                pk = (int)vget_lane_u16(h, 0);
            }
        }
    }
#endif
    for (; i < n; i++) {
        int v = p[i] < 0 ? -p[i] : p[i];
        if (v > pk) {
            pk = v;
        }
    }
    return pk;
}

/* ---- DMEM: 4 KB, com folga antes e depois (o resampler le 4 amostras antes da
 *      entrada; copias grandes podem passar do fim sem estragar nada) ---- */
#define DMEM_SIZE 0x1000
static union {
    uint8_t b[0x400 + DMEM_SIZE + 0x1000];
    int16_t s[(0x400 + DMEM_SIZE + 0x1000) / 2];
    uint64_t align_;
} g_mem;
#define DMEM (g_mem.b + 0x400)

static inline uint8_t* U8(uint32_t a) {
    return DMEM + (a & 0xfff);
}
static inline int16_t* S16(uint32_t a) {
    return (int16_t*)(DMEM + (a & 0xffe));
}
/* amostra numero "pos" da DMEM (o mupen: sample()) */
static inline int16_t* SMP(uint32_t pos) {
    return (int16_t*)DMEM + (pos & 0x7ff);
}
/* ponteiro "solto" (copias e mixagens em bloco, como o mupen) */
static inline int16_t* P16(uint32_t a) {
    return (int16_t*)(DMEM + a);
}
/* Endereco da RAM: o RSP do N64 so olha 24 bits (16 MB), mas aqui o executavel (com o BSS do
 * jogo, onde fica o heap de audio) pode passar de 0x81000000. Com 24 bits os buffers de audio
 * acima disso caiam em outro lugar e o som saia mudo. Ponteiro K0 (0x8xxxxxxx) ou endereco
 * fisico: os dois viram base + endereco sem o bit alto. */
static inline uint8_t* RAM(uint32_t a) {
    return audio_hle_ram + (a & 0x7fffffff);
}

static inline int16_t clamp16(int32_t x) {
    return (int16_t)(x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
}

/* ---- estado do microcodigo ---- */
static struct {
    uint16_t in, out, count;
    uint32_t loop;
    int16_t table[0x100]; /* codebook do ADPCM (16 entradas de 16 amostras) */
    uint16_t env_values[3];
    uint16_t env_steps[3];
    uint16_t filter_count;
    uint32_t filter_lut[2];
} st;

static const int16_t RESAMPLE_LUT[64 * 4] = {
    (int16_t)0x0c39, (int16_t)0x66ad, (int16_t)0x0d46, (int16_t)0xffdf, (int16_t)0x0b39, (int16_t)0x6696, (int16_t)0x0e5f, (int16_t)0xffd8,
    (int16_t)0x0a44, (int16_t)0x6669, (int16_t)0x0f83, (int16_t)0xffd0, (int16_t)0x095a, (int16_t)0x6626, (int16_t)0x10b4, (int16_t)0xffc8,
    (int16_t)0x087d, (int16_t)0x65cd, (int16_t)0x11f0, (int16_t)0xffbf, (int16_t)0x07ab, (int16_t)0x655e, (int16_t)0x1338, (int16_t)0xffb6,
    (int16_t)0x06e4, (int16_t)0x64d9, (int16_t)0x148c, (int16_t)0xffac, (int16_t)0x0628, (int16_t)0x643f, (int16_t)0x15eb, (int16_t)0xffa1,
    (int16_t)0x0577, (int16_t)0x638f, (int16_t)0x1756, (int16_t)0xff96, (int16_t)0x04d1, (int16_t)0x62cb, (int16_t)0x18cb, (int16_t)0xff8a,
    (int16_t)0x0435, (int16_t)0x61f3, (int16_t)0x1a4c, (int16_t)0xff7e, (int16_t)0x03a4, (int16_t)0x6106, (int16_t)0x1bd7, (int16_t)0xff71,
    (int16_t)0x031c, (int16_t)0x6007, (int16_t)0x1d6c, (int16_t)0xff64, (int16_t)0x029f, (int16_t)0x5ef5, (int16_t)0x1f0b, (int16_t)0xff56,
    (int16_t)0x022a, (int16_t)0x5dd0, (int16_t)0x20b3, (int16_t)0xff48, (int16_t)0x01be, (int16_t)0x5c9a, (int16_t)0x2264, (int16_t)0xff3a,
    (int16_t)0x015b, (int16_t)0x5b53, (int16_t)0x241e, (int16_t)0xff2c, (int16_t)0x0101, (int16_t)0x59fc, (int16_t)0x25e0, (int16_t)0xff1e,
    (int16_t)0x00ae, (int16_t)0x5896, (int16_t)0x27a9, (int16_t)0xff10, (int16_t)0x0063, (int16_t)0x5720, (int16_t)0x297a, (int16_t)0xff02,
    (int16_t)0x001f, (int16_t)0x559d, (int16_t)0x2b50, (int16_t)0xfef4, (int16_t)0xffe2, (int16_t)0x540d, (int16_t)0x2d2c, (int16_t)0xfee8,
    (int16_t)0xffac, (int16_t)0x5270, (int16_t)0x2f0d, (int16_t)0xfedb, (int16_t)0xff7c, (int16_t)0x50c7, (int16_t)0x30f3, (int16_t)0xfed0,
    (int16_t)0xff53, (int16_t)0x4f14, (int16_t)0x32dc, (int16_t)0xfec6, (int16_t)0xff2e, (int16_t)0x4d57, (int16_t)0x34c8, (int16_t)0xfebd,
    (int16_t)0xff0f, (int16_t)0x4b91, (int16_t)0x36b6, (int16_t)0xfeb6, (int16_t)0xfef5, (int16_t)0x49c2, (int16_t)0x38a5, (int16_t)0xfeb0,
    (int16_t)0xfedf, (int16_t)0x47ed, (int16_t)0x3a95, (int16_t)0xfeac, (int16_t)0xfece, (int16_t)0x4611, (int16_t)0x3c85, (int16_t)0xfeab,
    (int16_t)0xfec0, (int16_t)0x4430, (int16_t)0x3e74, (int16_t)0xfeac, (int16_t)0xfeb6, (int16_t)0x424a, (int16_t)0x4060, (int16_t)0xfeaf,
    (int16_t)0xfeaf, (int16_t)0x4060, (int16_t)0x424a, (int16_t)0xfeb6, (int16_t)0xfeac, (int16_t)0x3e74, (int16_t)0x4430, (int16_t)0xfec0,
    (int16_t)0xfeab, (int16_t)0x3c85, (int16_t)0x4611, (int16_t)0xfece, (int16_t)0xfeac, (int16_t)0x3a95, (int16_t)0x47ed, (int16_t)0xfedf,
    (int16_t)0xfeb0, (int16_t)0x38a5, (int16_t)0x49c2, (int16_t)0xfef5, (int16_t)0xfeb6, (int16_t)0x36b6, (int16_t)0x4b91, (int16_t)0xff0f,
    (int16_t)0xfebd, (int16_t)0x34c8, (int16_t)0x4d57, (int16_t)0xff2e, (int16_t)0xfec6, (int16_t)0x32dc, (int16_t)0x4f14, (int16_t)0xff53,
    (int16_t)0xfed0, (int16_t)0x30f3, (int16_t)0x50c7, (int16_t)0xff7c, (int16_t)0xfedb, (int16_t)0x2f0d, (int16_t)0x5270, (int16_t)0xffac,
    (int16_t)0xfee8, (int16_t)0x2d2c, (int16_t)0x540d, (int16_t)0xffe2, (int16_t)0xfef4, (int16_t)0x2b50, (int16_t)0x559d, (int16_t)0x001f,
    (int16_t)0xff02, (int16_t)0x297a, (int16_t)0x5720, (int16_t)0x0063, (int16_t)0xff10, (int16_t)0x27a9, (int16_t)0x5896, (int16_t)0x00ae,
    (int16_t)0xff1e, (int16_t)0x25e0, (int16_t)0x59fc, (int16_t)0x0101, (int16_t)0xff2c, (int16_t)0x241e, (int16_t)0x5b53, (int16_t)0x015b,
    (int16_t)0xff3a, (int16_t)0x2264, (int16_t)0x5c9a, (int16_t)0x01be, (int16_t)0xff48, (int16_t)0x20b3, (int16_t)0x5dd0, (int16_t)0x022a,
    (int16_t)0xff56, (int16_t)0x1f0b, (int16_t)0x5ef5, (int16_t)0x029f, (int16_t)0xff64, (int16_t)0x1d6c, (int16_t)0x6007, (int16_t)0x031c,
    (int16_t)0xff71, (int16_t)0x1bd7, (int16_t)0x6106, (int16_t)0x03a4, (int16_t)0xff7e, (int16_t)0x1a4c, (int16_t)0x61f3, (int16_t)0x0435,
    (int16_t)0xff8a, (int16_t)0x18cb, (int16_t)0x62cb, (int16_t)0x04d1, (int16_t)0xff96, (int16_t)0x1756, (int16_t)0x638f, (int16_t)0x0577,
    (int16_t)0xffa1, (int16_t)0x15eb, (int16_t)0x643f, (int16_t)0x0628, (int16_t)0xffac, (int16_t)0x148c, (int16_t)0x64d9, (int16_t)0x06e4,
    (int16_t)0xffb6, (int16_t)0x1338, (int16_t)0x655e, (int16_t)0x07ab, (int16_t)0xffbf, (int16_t)0x11f0, (int16_t)0x65cd, (int16_t)0x087d,
    (int16_t)0xffc8, (int16_t)0x10b4, (int16_t)0x6626, (int16_t)0x095a, (int16_t)0xffd0, (int16_t)0x0f83, (int16_t)0x6669, (int16_t)0x0a44,
    (int16_t)0xffd8, (int16_t)0x0e5f, (int16_t)0x6696, (int16_t)0x0b39, (int16_t)0xffdf, (int16_t)0x0d46, (int16_t)0x66ad, (int16_t)0x0c39,
};

/* ---------------------------------------------------------------- comandos simples */

static void cmd_clearbuff(uint32_t w1, uint32_t w2) {
    uint16_t dmem = (uint16_t)w1;
    uint16_t count = w2 & 0xfff;
    if ((uint32_t)(dmem & 0xfff) + count <= 0x1000u) {
        memset(U8(dmem), 0, count); /* sem dar a volta na DMEM: o mesmo que byte a byte */
        return;
    }
    while (count != 0) {
        *U8(dmem++) = 0;
        --count;
    }
}

static void cmd_loadbuff(uint32_t w1, uint32_t w2) {
    uint32_t count = (w1 >> 12) & 0xfff;
    uint32_t dmem = w1 & 0xfff;
    uint32_t addr = w2;
    dmem &= ~3u;
    addr &= ~7u;
    count = (count + 7) & ~7u;
    memcpy(DMEM + dmem, RAM(addr), count);
}

static void cmd_savebuff(uint32_t w1, uint32_t w2) {
    uint32_t count = (w1 >> 12) & 0xfff;
    uint32_t dmem = w1 & 0xfff;
    uint32_t addr = w2;
    dmem &= ~3u;
    addr &= ~7u;
    count = (count + 7) & ~7u;
    g_peakSave = peak16((const int16_t*)(DMEM + dmem), count / 2, g_peakSave);
    memcpy(RAM(addr), DMEM + dmem, count);
}

static void cmd_setbuff(uint32_t w1, uint32_t w2) {
    st.in = (uint16_t)w1;
    st.out = (uint16_t)(w2 >> 16);
    st.count = (uint16_t)w2;
}

static void cmd_setloop(uint32_t w1, uint32_t w2) {
    (void)w1;
    st.loop = w2;
}

static void cmd_loadadpcm(uint32_t w1, uint32_t w2) {
    uint32_t count = (uint16_t)w1; /* bytes */
    if (count > sizeof st.table) {
        count = sizeof st.table;
    }
    memcpy(st.table, RAM(w2), count & ~1u);
    g_bookEntries = count / 2;
    memcpy(g_book0, st.table, sizeof g_book0);
}

static void cmd_dmemmove(uint32_t w1, uint32_t w2) {
    uint16_t dmemi = (uint16_t)w1;
    uint16_t dmemo = (uint16_t)(w2 >> 16);
    uint16_t count = (uint16_t)w2;
    if (count == 0) {
        return;
    }
    count = (uint16_t)((count + 3) & ~3);
    while (count != 0) { /* byte a byte, para frente (sobreposicao igual ao RSP) */
        *U8(dmemo++) = *U8(dmemi++);
        --count;
    }
}

static void cmd_mixer(uint32_t w1, uint32_t w2) {
    uint32_t count = ((w1 >> 12) & 0xff0) >> 1;
    int32_t gain = (int16_t)w1;
    int16_t* dst = P16((uint16_t)w2);
    const int16_t* src = P16((uint16_t)(w2 >> 16));
#if AHLE_NEON
    for (; count >= 8; count -= 8, dst += 8, src += 8) {
        const int16x8_t a = vld1q_s16(src), d = vld1q_s16(dst);
        const int32x4_t lo = vaddq_s32(vmovl_s16(vget_low_s16(d)), vshrq_n_s32(vmull_n_s16(vget_low_s16(a), (int16_t)gain), 15));
        const int32x4_t hi = vaddq_s32(vmovl_s16(vget_high_s16(d)), vshrq_n_s32(vmull_n_s16(vget_high_s16(a), (int16_t)gain), 15));
        vst1q_s16(dst, vcombine_s16(vqmovn_s32(lo), vqmovn_s32(hi)));
    }
#endif
    while (count != 0) {
        *dst = clamp16(*dst + ((*src * gain) >> 15));
        ++dst;
        ++src;
        --count;
    }
}

static void cmd_addmixer(uint32_t w1, uint32_t w2) {
    uint32_t count = ((w1 >> 12) & 0xff0) >> 1;
    int16_t* dst = P16((uint16_t)w2);
    const int16_t* src = P16((uint16_t)(w2 >> 16));
#if AHLE_NEON
    for (; count >= 8; count -= 8, dst += 8, src += 8) {
        vst1q_s16(dst, vqaddq_s16(vld1q_s16(dst), vld1q_s16(src)));
    }
#endif
    while (count != 0) {
        *dst = clamp16(*dst + *src);
        ++dst;
        ++src;
        --count;
    }
}

static void cmd_hilogain(uint32_t w1, uint32_t w2) {
    int32_t gain = (int8_t)(w1 >> 16); /* Q4.4 */
    uint32_t count = (w1 & 0xfff) >> 1;
    int16_t* dst = P16((uint16_t)(w2 >> 16));
#if AHLE_NEON
    for (; count >= 8; count -= 8, dst += 8) {
        const int16x8_t d = vld1q_s16(dst);
        vst1q_s16(dst, vcombine_s16(vqshrn_n_s32(vmull_n_s16(vget_low_s16(d), (int16_t)gain), 4),
                                    vqshrn_n_s32(vmull_n_s16(vget_high_s16(d), (int16_t)gain), 4)));
    }
#endif
    while (count != 0) {
        *dst = clamp16((*dst * gain) >> 4);
        ++dst;
        --count;
    }
}

static void cmd_duplicate(uint32_t w1, uint32_t w2) {
    uint32_t count = (w1 >> 16) & 0xff;
    uint16_t dmemi = (uint16_t)w1;
    uint16_t dmemo = (uint16_t)(w2 >> 16);
    uint8_t buf[128];
    memcpy(buf, DMEM + dmemi, 128);
    while (count != 0) {
        memcpy(DMEM + dmemo, buf, 128);
        dmemo += 128;
        --count;
    }
}

static void cmd_interl(uint32_t w1, uint32_t w2) {
    uint32_t count = (uint16_t)w1;
    uint16_t dmemi = (uint16_t)(w2 >> 16);
    uint16_t dmemo = (uint16_t)w2;
    while (count != 0) {
        *S16(dmemo) = *S16(dmemi);
        dmemo += 2;
        dmemi += 4;
        --count;
    }
}

/* L e R separados -> L,R,L,R... (para o AI) */
static void cmd_interleave(uint32_t w1, uint32_t w2) {
    uint32_t count = ((w1 >> 12) & 0xff0) >> 2;
    int16_t* dst = P16((uint16_t)w1);
    const int16_t* l = P16((uint16_t)(w2 >> 16));
    const int16_t* r = P16((uint16_t)w2);
#if AHLE_NEON
    for (; count >= 4; count -= 4, dst += 16, l += 8, r += 8) {
        int16x8x2_t v;
        v.val[0] = vld1q_s16(l);
        v.val[1] = vld1q_s16(r);
        vst2q_s16(dst, v);
    }
#endif
    while (count != 0) {
        int16_t l1 = l[0], l2 = l[1], r1 = r[0], r2 = r[1];
        dst[0] = l1;
        dst[1] = r1;
        dst[2] = l2;
        dst[3] = r2;
        dst += 4;
        l += 2;
        r += 2;
        --count;
    }
}

static void cmd_copy_blocks(uint32_t w1, uint32_t w2) { /* comando 16 */
    int blocks = (int)((w1 >> 16) & 0xff);
    uint16_t dmemi = (uint16_t)w1;
    uint16_t dmemo = (uint16_t)(w2 >> 16);
    int block_size = (int)(uint16_t)w2;
    do {
        int left = block_size;
        do {
            memmove(DMEM + dmemo, DMEM + dmemi, 0x20);
            left -= 0x20;
            dmemi += 0x20;
            dmemo += 0x20;
        } while (left > 0);
        --blocks;
    } while (blocks > 0);
}

/* ---------------------------------------------------------------- envelope / mixer */

static void cmd_envsetup1(uint32_t w1, uint32_t w2) {
    st.env_values[2] = (uint16_t)((w1 >> 8) & 0xff00);
    st.env_steps[2] = (uint16_t)w1;
    st.env_steps[0] = (uint16_t)(w2 >> 16);
    st.env_steps[1] = (uint16_t)w2;
}

static void cmd_envsetup2(uint32_t w1, uint32_t w2) {
    (void)w1;
    st.env_values[0] = (uint16_t)(w2 >> 16);
    st.env_values[1] = (uint16_t)w2;
}

static void cmd_envmixer(uint32_t w1, uint32_t w2) {
    uint32_t dmemi = (w1 >> 12) & 0xff0;
    uint32_t count = (w1 >> 8) & 0xff;
    int swap = (w1 >> 4) & 1;
    int16_t* in = P16(dmemi);
    int16_t* dl = P16((w2 >> 20) & 0xff0);
    int16_t* dr = P16((w2 >> 12) & 0xff0);
    int16_t* wl = P16((w2 >> 4) & 0xff0);
    int16_t* wr = P16((w2 << 4) & 0xff0);
    int16_t x0 = (int16_t)(0 - (int16_t)((w1 & 0x2) >> 1));
    int16_t x1 = (int16_t)(0 - (int16_t)(w1 & 0x1));
    int16_t x2 = (int16_t)(0 - (int16_t)((w1 & 0x8) >> 1));
    int16_t x3 = (int16_t)(0 - (int16_t)((w1 & 0x4) >> 1));
    uint32_t v0 = st.env_values[0], v1 = st.env_values[1], v2 = st.env_values[2];

    g_peakEnvIn = peak16(in, (count + 7) & ~7u, g_peakEnvIn);
    if (v0 != 0 || v1 != 0) {
        g_envNonzero++;
    }
    if (swap) {
        int16_t* t = wl;
        wl = wr;
        wr = t;
    }
    count = (count + 7) & ~7u;
#if AHLE_NEON
    {
        /* (int16)((s * v) >> 16) = metade alta do produto de 32 bits; ^ x = troca de sinal (x = 0 ou -1);
         * clamp16(a + b) = soma com saturacao */
        const int16x8_t vx0 = vdupq_n_s16(x0), vx1 = vdupq_n_s16(x1), vx2 = vdupq_n_s16(x2), vx3 = vdupq_n_s16(x3);
        for (; count != 0; count -= 8) {
            const int16x8_t s = vld1q_s16(in);
            const int32x4_t slo = vmovl_s16(vget_low_s16(s)), shi = vmovl_s16(vget_high_s16(s));
            const int16x8_t l = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(slo, (int32_t)v0), 16),
                                                       vshrn_n_s32(vmulq_n_s32(shi, (int32_t)v0), 16)), vx0);
            const int16x8_t r = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(slo, (int32_t)v1), 16),
                                                       vshrn_n_s32(vmulq_n_s32(shi, (int32_t)v1), 16)), vx1);
            const int16x8_t l2 = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(l)), (int32_t)v2), 16),
                                                        vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(l)), (int32_t)v2), 16)), vx2);
            const int16x8_t r2 = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(r)), (int32_t)v2), 16),
                                                        vshrn_n_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(r)), (int32_t)v2), 16)), vx3);
            vst1q_s16(dl, vqaddq_s16(vld1q_s16(dl), l));
            vst1q_s16(dr, vqaddq_s16(vld1q_s16(dr), r));
            vst1q_s16(wl, vqaddq_s16(vld1q_s16(wl), l2));
            vst1q_s16(wr, vqaddq_s16(vld1q_s16(wr), r2));
            v0 = (uint16_t)(v0 + st.env_steps[0]);
            v1 = (uint16_t)(v1 + st.env_steps[1]);
            v2 = (uint16_t)(v2 + st.env_steps[2]);
            dl += 8;
            dr += 8;
            wl += 8;
            wr += 8;
            in += 8;
        }
    }
#endif
    while (count != 0) {
        int i;
        for (i = 0; i < 8; ++i) {
            int32_t s = in[i];
            /* produto de 32 bits e corte em 16, como o mupen ((int32 * uint32) >> 16) */
            int16_t l = (int16_t)((uint32_t)(s * (int32_t)v0) >> 16) ^ x0;
            int16_t r = (int16_t)((uint32_t)(s * (int32_t)v1) >> 16) ^ x1;
            int16_t l2 = (int16_t)((uint32_t)((int32_t)l * (int32_t)v2) >> 16) ^ x2;
            int16_t r2 = (int16_t)((uint32_t)((int32_t)r * (int32_t)v2) >> 16) ^ x3;
            dl[i] = clamp16(dl[i] + l);
            dr[i] = clamp16(dr[i] + r);
            wl[i] = clamp16(wl[i] + l2);
            wr[i] = clamp16(wr[i] + r2);
        }
        v0 = (uint16_t)(v0 + st.env_steps[0]);
        v1 = (uint16_t)(v1 + st.env_steps[1]);
        v2 = (uint16_t)(v2 + st.env_steps[2]);
        dl += 8;
        dr += 8;
        wl += 8;
        wr += 8;
        in += 8;
        count -= 8;
    }
    st.env_values[0] = (uint16_t)v0;
    st.env_values[1] = (uint16_t)v1;
    st.env_values[2] = (uint16_t)v2;
}

/* ---------------------------------------------------------------- resampler */

static void cmd_resample(uint32_t w1, uint32_t w2) {
    int init = (w1 >> 16) & 1;
    uint32_t pitch = (w1 & 0xffff) << 1;
    uint32_t addr = w2;
    uint32_t ipos = ((uint32_t)st.in >> 1) - 4;
    uint32_t opos = (uint32_t)st.out >> 1;
    uint32_t count = (((uint32_t)st.count + 0xf) & ~0xfu) >> 1;
    uint32_t accu;
    int k;

    if (init) {
        for (k = 0; k < 4; ++k) {
            *SMP(ipos + k) = 0;
        }
        accu = 0;
    } else {
        const int16_t* s = (const int16_t*)RAM(addr);
        for (k = 0; k < 4; ++k) {
            *SMP(ipos + k) = s[k];
        }
        accu = *(const uint16_t*)RAM(addr + 8);
    }
#if AHLE_NEON
    /* sem dar a volta na DMEM no trecho: 4 amostras por saida com uma leitura e uma multiplicacao NEON
     * (a soma de 32 bits e a mesma: inteiros, sem arredondar no meio) */
    while (count != 0 && opos + 1 <= 0x800u) {
        const uint32_t ip = ipos & 0x7ff;
        if (ip + 4 > 0x800u) {
            break;
        }
        {
            const int16_t* lut = RESAMPLE_LUT + ((accu & 0xfc00) >> 8);
            const int32x4_t m = vmull_s16(vld1_s16((const int16_t*)DMEM + ip), vld1_s16(lut));
            const int32x2_t p2 = vpadd_s32(vget_low_s32(m), vget_high_s32(m));
            *SMP(opos++) = clamp16((vget_lane_s32(p2, 0) + vget_lane_s32(p2, 1)) >> 15);
        }
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xffff;
        --count;
    }
#endif
    while (count != 0) {
        const int16_t* lut = RESAMPLE_LUT + ((accu & 0xfc00) >> 8);
        *SMP(opos++) = clamp16((*SMP(ipos) * lut[0] + *SMP(ipos + 1) * lut[1] + *SMP(ipos + 2) * lut[2] +
                                *SMP(ipos + 3) * lut[3]) >> 15);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xffff;
        --count;
    }
    {
        int16_t* s = (int16_t*)RAM(addr);
        for (k = 0; k < 4; ++k) {
            s[k] = *SMP(ipos + k);
        }
        *(uint16_t*)RAM(addr + 8) = (uint16_t)accu;
    }
}

static void cmd_resample_zoh(uint32_t w1, uint32_t w2) {
    uint32_t pitch = (w1 & 0xffff) << 1;
    uint32_t accu = w2 & 0xffff;
    uint32_t ipos = (uint32_t)st.in >> 1;
    uint32_t opos = (uint32_t)st.out >> 1;
    uint32_t count = (uint32_t)st.count >> 1;
    while (count != 0) {
        *SMP(opos++) = *SMP(ipos);
        accu += pitch;
        ipos += accu >> 16;
        accu &= 0xffff;
        --count;
    }
}

/* ---------------------------------------------------------------- ADPCM e 8 bits */

static void residuals(int16_t* dst, const int16_t* src, const int16_t* cb, const int16_t* last) {
#if AHLE_NEON
    /* as 8 somas de uma vez: src[k] * b2 deslocado k + 1 posicoes (zeros na frente) para k = 0..6 */
    const int16x8_t s = vld1q_s16(src), b1 = vld1q_s16(cb), b2 = vld1q_s16(cb + 8), z = vdupq_n_s16(0);
    const int16_t l1 = last[0], l2 = last[1];
    int32x4_t lo = vshll_n_s16(vget_low_s16(s), 11), hi = vshll_n_s16(vget_high_s16(s), 11);
    lo = vmlal_n_s16(lo, vget_low_s16(b1), l1);
    hi = vmlal_n_s16(hi, vget_high_s16(b1), l1);
    lo = vmlal_n_s16(lo, vget_low_s16(b2), l2);
    hi = vmlal_n_s16(hi, vget_high_s16(b2), l2);
#define RES_K(K)                                                                  \
    {                                                                             \
        const int16x8_t sh = vextq_s16(z, b2, 7 - (K));                           \
        lo = vmlal_n_s16(lo, vget_low_s16(sh), src[K]);                           \
        hi = vmlal_n_s16(hi, vget_high_s16(sh), src[K]);                          \
    }
    RES_K(0) RES_K(1) RES_K(2) RES_K(3) RES_K(4) RES_K(5) RES_K(6)
#undef RES_K
    vst1q_s16(dst, vcombine_s16(vqshrn_n_s32(lo, 11), vqshrn_n_s32(hi, 11)));
#else
    const int16_t* b1 = cb;
    const int16_t* b2 = cb + 8;
    int32_t l1 = last[0], l2 = last[1];
    int i, j;
    for (i = 0; i < 8; ++i) {
        int32_t acc = (int32_t)src[i] << 11;
        acc += b1[i] * l1 + b2[i] * l2;
        for (j = 0; j < i; ++j) { /* rdot(i, b2, src) */
            acc += b2[j] * src[i - 1 - j];
        }
        dst[i] = clamp16(acc >> 11);
    }
#endif
}

static void cmd_adpcm(uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xff;
    uint32_t addr = w2;
    int init = flags & 1, loop = flags & 2, two = flags & 4;
    uint32_t dmemo = st.out, dmemi = st.in;
    uint32_t count = ((uint32_t)st.count + 0x1f) & ~0x1fu;
    int16_t last[16];
    int i;

    if (init) {
        memset(last, 0, sizeof last);
    } else {
        memcpy(last, RAM(loop ? st.loop : addr), sizeof last);
    }
    for (i = 0; i < 16; ++i, dmemo += 2) {
        *S16(dmemo) = last[i];
    }
    while (count != 0) {
        int16_t frame[16];
        uint8_t code = *U8(dmemi++);
        unsigned scale = code >> 4;
        g_frames++;
        if (g_bookEntries != 0 && (unsigned)(code & 0xf) * 16u >= g_bookEntries) {
            g_badPred++; /* com dados certos o preditor sempre existe no livro */
        }
        const int16_t* cb = st.table + ((code & 0xf) << 4);
        if (two) {
            unsigned rs = scale < 14 ? 14 - scale : 0;
            for (i = 0; i < 4; ++i) {
                uint8_t b = *U8(dmemi++);
                frame[i * 4 + 0] = (int16_t)((int16_t)((uint16_t)(b & 0xc0) << 8) >> rs);
                frame[i * 4 + 1] = (int16_t)((int16_t)((uint16_t)(b & 0x30) << 10) >> rs);
                frame[i * 4 + 2] = (int16_t)((int16_t)((uint16_t)(b & 0x0c) << 12) >> rs);
                frame[i * 4 + 3] = (int16_t)((int16_t)((uint16_t)(b & 0x03) << 14) >> rs);
            }
        } else {
            unsigned rs = scale < 12 ? 12 - scale : 0;
            for (i = 0; i < 8; ++i) {
                uint8_t b = *U8(dmemi++);
                frame[i * 2 + 0] = (int16_t)((int16_t)((uint16_t)(b & 0xf0) << 8) >> rs);
                frame[i * 2 + 1] = (int16_t)((int16_t)((uint16_t)(b & 0x0f) << 12) >> rs);
            }
        }
        residuals(last, frame, cb, last + 14);
        residuals(last + 8, frame + 8, cb, last + 6);
        for (i = 0; i < 16; ++i, dmemo += 2) {
            *S16(dmemo) = last[i];
        }
        count -= 32;
    }
    g_peakDec = peak16((const int16_t*)(DMEM + (st.out & 0xffe)), ((((uint32_t)st.count + 0x1f) & ~0x1fu) >> 1) + 16, g_peakDec);
    memcpy(RAM(addr), last, sizeof last);
}

/* amostras de 8 bits (o mupen nao tem; mesmo formato de estado do ADPCM) */
static void cmd_s8dec(uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xff;
    uint32_t addr = w2;
    uint32_t dmemo = st.out, dmemi = st.in;
    int32_t count = (int32_t)(((uint32_t)st.count + 0x1f) & ~0x1fu);
    int16_t last[16];
    int i;

    if (flags & 1) {
        memset(last, 0, sizeof last);
    } else {
        memcpy(last, RAM((flags & 2) ? st.loop : addr), sizeof last);
    }
    for (i = 0; i < 16; ++i, dmemo += 2) {
        *S16(dmemo) = last[i];
    }
    while (count > 0) {
        for (i = 0; i < 16; ++i, dmemo += 2) {
            last[i] = (int16_t)((uint16_t)*U8(dmemi++) << 8);
            *S16(dmemo) = last[i];
        }
        count -= 32;
    }
    memcpy(RAM(addr), last, sizeof last);
}

/* ---------------------------------------------------------------- filtro (reverb) */

static void cmd_filter(uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xff;
    uint32_t addr = w2;
    if (flags > 1) {
        st.filter_count = (uint16_t)w1;
        st.filter_lut[0] = addr;
        return;
    }
    {
        uint16_t dmem = (uint16_t)w1;
        uint32_t count = st.filter_count;
        int16_t* l6 = (int16_t*)RAM(st.filter_lut[0]);
        int16_t* l5;
        int16_t x[16];
        static int16_t outbuf[0x800];
        int16_t* in2 = P16(dmem);
        int16_t* out = outbuf;
        uint32_t n;
        int k, j;

        st.filter_lut[1] = addr + 0x10;
        l5 = (int16_t*)RAM(st.filter_lut[1]);
        for (k = 0; k < 8; ++k) {
            int32_t v = (l5[k] + l6[k]) >> 1;
            l5[k] = l6[k] = (int16_t)v;
        }
        if (count > sizeof outbuf) {
            count = sizeof outbuf;
        }
        memcpy(x, RAM(addr), 16); /* as 8 amostras anteriores */
        for (n = 0; n < count; n += 16) {
            memcpy(x + 8, in2, 16);
            for (j = 0; j < 8; ++j) {
                int32_t v = 0;
                for (k = 0; k < 8; ++k) {
                    v += x[j + 1 + k] * l6[7 - k];
                }
                out[j] = (int16_t)((v + 0x4000) >> 15);
            }
            memcpy(x, x + 8, 16);
            in2 += 8;
            out += 8;
        }
        if (count != 0) {
            memcpy(RAM(addr), in2 - 8, 16);
        }
        memcpy(DMEM + dmem, outbuf, count);
    }
}

/* ---------------------------------------------------------------- despacho */

void audio_hle_report(char* buf, int size) {
    snprintf(buf, (size_t)size,
             "%u tarefas; comandos: adpcm %u resample %u envmixer %u (com volume %u) savebuff %u loadbuff %u; "
             "picos: decodificado %d, entrada do mixer %d, salvo %d; quadros adpcm %u (preditor invalido %u), "
             "livro %u valores [%d %d %d %d %d %d %d %d]",
             g_tasks, g_cnt[1], g_cnt[5], g_cnt[19], g_envNonzero, g_cnt[21], g_cnt[20], g_peakDec, g_peakEnvIn,
             g_peakSave, g_frames, g_badPred, g_bookEntries, g_book0[0], g_book0[1], g_book0[2], g_book0[3], g_book0[4],
             g_book0[5], g_book0[6], g_book0[7]);
    g_frames = g_badPred = 0;
    memset(g_cnt, 0, sizeof g_cnt);
    g_tasks = g_envNonzero = 0;
    g_peakDec = g_peakEnvIn = g_peakSave = 0;
}

void audio_hle_run(const uint32_t* alist, uint32_t nbytes) {
    const uint32_t* end = alist + (nbytes >> 2);
    g_tasks++;
    while (alist + 1 < end) {
        uint32_t w1 = alist[0], w2 = alist[1];
        alist += 2;
        g_cnt[(w1 >> 24) & 0x1f]++;
        switch ((w1 >> 24) & 0x7f) {
            case 1: cmd_adpcm(w1, w2); break;
            case 2: cmd_clearbuff(w1, w2); break;
            case 4: cmd_addmixer(w1, w2); break;
            case 5: cmd_resample(w1, w2); break;
            case 6: cmd_resample_zoh(w1, w2); break;
            case 7: cmd_filter(w1, w2); break;
            case 8: cmd_setbuff(w1, w2); break;
            case 9: cmd_duplicate(w1, w2); break;
            case 10: cmd_dmemmove(w1, w2); break;
            case 11: cmd_loadadpcm(w1, w2); break;
            case 12: cmd_mixer(w1, w2); break;
            case 13: cmd_interleave(w1, w2); break;
            case 14: cmd_hilogain(w1, w2); break;
            case 15: cmd_setloop(w1, w2); break;
            case 16: cmd_copy_blocks(w1, w2); break;
            case 17: cmd_interl(w1, w2); break;
            case 18: cmd_envsetup1(w1, w2); break;
            case 19: cmd_envmixer(w1, w2); break;
            case 20: cmd_loadbuff(w1, w2); break;
            case 21: cmd_savebuff(w1, w2); break;
            case 22: cmd_envsetup2(w1, w2); break;
            case 23: cmd_s8dec(w1, w2); break;
            case 0:
                break;
            default:
                audio_hle_unknown++;
                break;
        }
    }
}

#ifdef AUDIO_HLE_TEST
uint8_t* audio_hle_dmem_for_test(void) {
    return DMEM;
}
#endif
