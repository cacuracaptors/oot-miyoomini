/*
 * gfx_rdp.c -- comandos do RDP: tiles, carga de TMEM/paleta, decodificacao de texturas (com cache),
 * retangulos (fill / texrect).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <link.h>
#endif

#include "gfx_priv.h"

/* ------------------------------------------------------------------ leitura segura da origem das cargas
 * No N64 uma carga de textura pode ler alguns bytes alem do fim da imagem (lixo inofensivo). Aqui as
 * texturas do jogo ficam dentro do executavel, e entre os pedacos dele (codigo+dados constantes / dados)
 * ha um buraco sem memoria: uma textura no fim do primeiro pedaco fazia a carga ler o buraco e o jogo
 * caia (sinal 11 ao abrir o menu com START; dependia so de onde a textura caia no executavel).
 * src_ok devolve quantos dos n bytes a partir de p podem ser lidos; o resto e lido como zero. */
static uintptr_t g_hole[8][2]; /* [inicio, fim) sem memoria, dentro da faixa 0x80000000..0x81000000 */
static int g_nhole = -1;
#ifdef __linux__
static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    uintptr_t seg[16][2];
    int n = 0, i, j;
    (void)sz;
    (void)data;
    if (info->dlpi_name != NULL && info->dlpi_name[0] != 0) {
        return 0; /* so o executavel (o primeiro, sem nome) */
    }
    for (i = 0; i < info->dlpi_phnum && n < 16; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            uintptr_t a = (uintptr_t)(info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)4095;
            uintptr_t b = ((uintptr_t)(info->dlpi_addr + ph->p_vaddr + ph->p_memsz) + 4095) & ~(uintptr_t)4095;
            seg[n][0] = a;
            seg[n][1] = b;
            n++;
        }
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (seg[j][0] < seg[i][0]) {
                uintptr_t t0 = seg[i][0], t1 = seg[i][1];
                seg[i][0] = seg[j][0];
                seg[i][1] = seg[j][1];
                seg[j][0] = t0;
                seg[j][1] = t1;
            }
        }
    }
    g_nhole = 0;
    for (i = 0; i < n; i++) {
        /* buraco depois de cada pedaco (ate o proximo, ou ate 0x81000000 depois do ultimo), so na faixa do jogo */
        uintptr_t a = seg[i][1], b = i + 1 < n ? seg[i + 1][0] : (uintptr_t)0x81000000u;
        if (a < (uintptr_t)0x80000000u || a >= (uintptr_t)0x81000000u || b <= a || g_nhole >= 8) {
            continue;
        }
        g_hole[g_nhole][0] = a;
        g_hole[g_nhole][1] = b;
        g_nhole++;
    }
    return 1;
}
#endif
static size_t src_ok(const void *p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    int i;
    if (g_nhole < 0) {
        g_nhole = 0;
#ifdef __linux__
        dl_iterate_phdr(phdr_cb, NULL);
#endif
        for (i = 0; i < g_nhole; i++) {
            fprintf(stderr, "[gfx] memoria sem nada (cargas de textura leem zero ali): %08lx..%08lx\n",
                    (unsigned long)g_hole[i][0], (unsigned long)g_hole[i][1]);
        }
    }
    for (i = 0; i < g_nhole; i++) {
        if (a >= g_hole[i][0] && a < g_hole[i][1]) {
            return 0;
        }
        if (a < g_hole[i][0] && a + n > g_hole[i][0]) {
            n = g_hole[i][0] - a;
        }
    }
    return n;
}
/* origem de n bytes para uma carga: a propria, ou (se passar de um buraco) uma copia com zeros no fim */
static uint8_t g_bounce[16384];
static const uint8_t *src_safe(const uint8_t *p, size_t n) {
    size_t ok;
    if (p == NULL) {
        return p;
    }
    if (n > sizeof g_bounce) {
        /* (nao acontece com comandos validos: a TMEM tem 4 KB) */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[gfx] carga de textura grande demais (%u bytes de %p): lida como zero\n", (unsigned)n, (const void *)p);
        }
        memset(g_bounce, 0, sizeof g_bounce);
        return g_bounce;
    }
    ok = src_ok(p, n);
    if (ok == n) {
        return p;
    }
    memset(g_bounce, 0, n);
    if (ok > 0) {
        memcpy(g_bounce, p, ok);
    }
    return g_bounce;
}

/* ------------------------------------------------------------------ cache de texturas */
#define TEX_CACHE_MAX 384
#define TEX_CACHE_BYTES (12u << 20)
int gx_hd_use = 2; /* opcao texturas_novas: 1 = usa o pacote de texturas (gfx_hd.c) se existir; 2 = so na logo da abertura */
int gx_hd_on = 0;  /* 1 quando ha pacote de texturas novas carregado (so entao anota a origem da TMEM) */
int gx_tmem_note = 0; /* gfx_hires.c: anota a origem da TMEM tambem sem o pacote (Link do menu em 2x) */
uint32_t gx_tex_cache_bytes = TEX_CACHE_BYTES; /* gfx_hd.c aumenta com o pacote de texturas */

static GxTex *g_cache[TEX_CACHE_MAX];
static int g_ncache;
static uint32_t g_cacheBytes;
static uint32_t g_stamp = 1;
/* texturas novas (gfx_hd.c; fraco: a ferramenta do PC liga sem ele) */
GxTex *gx_hd_lookup(GxTex *tx, const GxTile *tl, const uint8_t *src, unsigned stride) __attribute__((weak));
int gx_hd_retry(GxTex *tx) __attribute__((weak));
void gx_hd_free(GxTex *h) __attribute__((weak));
void gx_tex_acct(int64_t delta) {
    g_cacheBytes = (uint32_t)((int64_t)g_cacheBytes + delta);
}

static uint32_t tex_bytes(const GxTex *t) {
    /* memoria contada na cache: a textura (e a nova, com a copia com borda e o filtro pre-calculado) */
    uint32_t b = (uint32_t)(t->w * t->h * 4) + t->pfBytes + t->pf4Bytes;
    if (t->hd != NULL) {
        b += (uint32_t)(t->hd->w * t->hd->h * 4) * 6u;
    }
    return b;
}
static void tex_free(GxTex *t) {
    if (t->hd != NULL && gx_hd_free) {
        gx_hd_free(t->hd);
    }
    free(t->px);
    free(t->pp);
    free(t->pf);
    free(t->pf4);
    free(t);
}
static uint32_t g_epoch = 1; /* muda sempre que alguma textura sai da cache (invalida g_memo) */
/* textura nova que estava sendo carregada em segundo plano: pendura quando ficar pronta */
static inline void hd_poll(GxTex *t) {
    if (t->hdPend && gx_hd_retry) {
        uint32_t b0 = tex_bytes(t);
        if (gx_hd_retry(t)) {
            g_cacheBytes += tex_bytes(t) - b0;
        }
    }
}
/* atalho da busca na cache: a ultima textura vista com cada pedaco da chave (so aponta para texturas que
 * estao em g_cache; quem tira uma textura da cache limpa o atalho dela) */
#define TEX_HINT 1024
static GxTex *g_hint[TEX_HINT];
unsigned gx_stN[4];
uint64_t gx_stNs[3];
uint64_t gx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static unsigned hint_ix(uint64_t key) {
    return (unsigned)((key ^ (key >> 21) ^ (key >> 42)) & (TEX_HINT - 1));
}
static void hint_drop(const GxTex *tx) {
    if (g_hint[hint_ix(tx->key)] == tx) {
        g_hint[hint_ix(tx->key)] = NULL;
    }
}

void gx_rdp_init(void) {
    memset(&gx.omL, 0, sizeof gx.omL);
    gx.omL = 0;
    gx.omH = 0;
    gx.scissorX0 = gx.scissorY0 = 0;
    gx.scissorX1 = 320;
    gx.scissorY1 = 240;
    gx.scRaw[0] = gx.scRaw[1] = 0;
    gx.scRaw[2] = 320u << 2;
    gx.scRaw[3] = 240u << 2;
    gx.rs = 1.0f;
    gx.texDirty = 1;
}

/* opcao brightness: brilho (gamma) da imagem do jogo, sem custo por pixel. A curva e aplicada nas cores que
 * entram no desenho - texels (quando a textura entra na cache), cores dos vertices, cor primitiva, de ambiente e da
 * neblina. O N64 quase sempre multiplica textura por cor, e (a*b)^g = a^g * b^g: o resultado fica praticamente igual
 * a clarear a imagem pronta (os escuros sobem mais que os claros, como o gamma do Quake/Hexen II). 0 = original. */
int gx_brilho = 1;
int gx_gamma_on;
uint8_t gx_gamma[256];
float gx_gammaf[256];
static int g_gammaLevel = 0;

void gx_tex_flush(void);
void gx_gamma_update(void) {
    int i, lv = gx_brilho < 0 ? 0 : gx_brilho > 10 ? 10 : gx_brilho;
    if (lv == g_gammaLevel && (gx_gamma_on || lv == 0)) {
        return;
    }
    for (i = 0; i < 256; i++) {
        double e = 1.0 / (1.0 + 0.08 * lv); /* 10 = expoente 0,56 */
        double v = 255.0 * pow(i / 255.0, e);
        gx_gamma[i] = (uint8_t)(v > 255.0 ? 255 : (int)(v + 0.5));
        gx_gammaf[i] = (float)gx_gamma[i];
    }
    gx_gamma_on = lv != 0;
    if (lv != g_gammaLevel) {
        g_gammaLevel = lv;
        gx_tex_flush(); /* texturas na cache com a curva antiga */
    }
}

static inline uint32_t gamma_px(uint32_t px) {
    return (px & 0xFF000000u) | ((uint32_t)gx_gamma[(px >> 16) & 0xFF] << 16) | ((uint32_t)gx_gamma[(px >> 8) & 0xFF] << 8) |
           gx_gamma[px & 0xFF];
}

void gx_tex_flush(void) {
    int i;
    gx_batch_flush(); /* a fila pode usar estas texturas */
    for (i = 0; i < g_ncache; i++) {
        tex_free(g_cache[i]);
    }
    g_ncache = 0;
    g_cacheBytes = 0;
    g_epoch++;
    memset(g_hint, 0, sizeof g_hint);
    gx.rtex[0] = gx.rtex[1] = NULL;
}

/* ------------------------------------------------------------------ leitura da TMEM */
static inline int bits_of(int siz) {
    return 4 << siz;
}

/* Byte da TMEM para (linha t, deslocamento em bytes dentro da linha) */
static inline uint8_t tmem_at(const GxTile *tl, int t, int off, int bank) {
    unsigned a = (unsigned)tl->tmem * 8u + (unsigned)t * tl->line * 8u + (unsigned)off;
    if (t & 1) {
        a ^= 4u;
    }
    return gx.tmem[(a + (bank ? 0x800u : 0u)) & 0xFFFu];
}

static inline uint32_t pack(int r, int g, int b, int a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static inline uint32_t rgba16_px(unsigned c) {
    int r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31, a = (c & 1) ? 255 : 0;
    return pack((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a);
}

static inline uint32_t ia16_px(unsigned c) {
    int i = (c >> 8) & 255, a = c & 255;
    return pack(i, i, i, a);
}

static inline uint32_t tlut_px(unsigned idx, int pal, int is4, int tt) {
    unsigned a = 0x800u + (unsigned)(is4 ? (pal * 16 + (int)idx) : (int)idx) * 8u;
    unsigned c = ((unsigned)gx.tmem[a & 0xFFFu] << 8) | gx.tmem[(a + 1) & 0xFFFu];
    return tt == 3 ? ia16_px(c) : rgba16_px(c);
}

static inline uint64_t mix64(uint64_t h, uint64_t w) {
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/* palavras de 64 bits da TMEM (indice em palavras, da a volta em 4 KB) */
static inline uint64_t tmem_w(unsigned wi) {
    uint64_t w;
    memcpy(&w, gx.tmem + ((wi & 511u) << 3), 8);
    return w;
}

/* Hash de nw palavras de 64 bits da TMEM a partir da palavra w0 (da a volta em 512 palavras), somado a h.
 * 4 pistas de 32 bits, 16 bytes por passo: s = rotl((s ^ x) * K, 13) em cada pista (para x fixo e para s
 * fixo o passo e bijetivo: uma palavra diferente sempre muda a pista). No fim as pistas passam por mix64.
 * No ARM com NEON (4 pistas por instrucao); no PC as mesmas contas em C (o resultado e o mesmo). */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
static uint64_t hash_run(uint64_t h, const uint8_t *p, unsigned nw) {
    uint32_t st[4];
    unsigned i;
    st[0] = (uint32_t)h;
    st[1] = (uint32_t)(h >> 32);
    st[2] = (uint32_t)h ^ 0x6A09E667u;
    st[3] = (uint32_t)(h >> 32) ^ 0xBB67AE85u;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
        /* 4 acumuladores independentes (64 bytes por volta): a multiplicacao de um nao espera a do outro (com 1 so,
         * cada volta esperava a anterior: ~3x mais lento). Igual a referencia abaixo. */
        uint32x4_t v = vld1q_u32(st);
        uint32x4_t v1 = veorq_u32(v, vdupq_n_u32(0x3C6EF372u)), v2 = veorq_u32(v, vdupq_n_u32(0xA54FF53Au)),
                   v3 = veorq_u32(v, vdupq_n_u32(0x510E527Fu));
        const uint32x4_t K = vdupq_n_u32(0x9E3779B1u);
        for (i = 0; i + 8 <= nw; i += 8, p += 64) {
            v = vmulq_u32(veorq_u32(v, vreinterpretq_u32_u8(vld1q_u8(p))), K);
            v1 = vmulq_u32(veorq_u32(v1, vreinterpretq_u32_u8(vld1q_u8(p + 16))), K);
            v2 = vmulq_u32(veorq_u32(v2, vreinterpretq_u32_u8(vld1q_u8(p + 32))), K);
            v3 = vmulq_u32(veorq_u32(v3, vreinterpretq_u32_u8(vld1q_u8(p + 48))), K);
            v = vsriq_n_u32(vshlq_n_u32(v, 13), v, 19);
            v1 = vsriq_n_u32(vshlq_n_u32(v1, 13), v1, 19);
            v2 = vsriq_n_u32(vshlq_n_u32(v2, 13), v2, 19);
            v3 = vsriq_n_u32(vshlq_n_u32(v3, 13), v3, 19);
        }
        for (; i + 2 <= nw; i += 2, p += 16) {
            v = vmulq_u32(veorq_u32(v, vreinterpretq_u32_u8(vld1q_u8(p))), K);
            v = vsriq_n_u32(vshlq_n_u32(v, 13), v, 19);
        }
        /* junta (com a posicao de cada um: trocar dois blocos de 16 bytes muda a chave) */
        v = vmulq_u32(veorq_u32(v, vsriq_n_u32(vshlq_n_u32(v1, 7), v1, 25)), K);
        v = vmulq_u32(veorq_u32(v, vsriq_n_u32(vshlq_n_u32(v2, 11), v2, 21)), K);
        v = vmulq_u32(veorq_u32(v, vsriq_n_u32(vshlq_n_u32(v3, 17), v3, 15)), K);
        vst1q_u32(st, v);
    }
#else
    {
        uint32_t s1[4], s2[4], s3[4];
        int k;
        for (k = 0; k < 4; k++) {
            s1[k] = st[k] ^ 0x3C6EF372u;
            s2[k] = st[k] ^ 0xA54FF53Au;
            s3[k] = st[k] ^ 0x510E527Fu;
        }
#define HR_STEP(S, Q)                                                                                                  \
        for (k = 0; k < 4; k++) {                                                                                      \
            uint32_t x;                                                                                                \
            memcpy(&x, (Q) + 4 * k, 4);                                                                                \
            x = ((S)[k] ^ x) * 0x9E3779B1u;                                                                            \
            (S)[k] = (x << 13) | (x >> 19);                                                                            \
        }
        for (i = 0; i + 8 <= nw; i += 8, p += 64) {
            HR_STEP(st, p)
            HR_STEP(s1, p + 16)
            HR_STEP(s2, p + 32)
            HR_STEP(s3, p + 48)
        }
        for (; i + 2 <= nw; i += 2, p += 16) {
            HR_STEP(st, p)
        }
#undef HR_STEP
        for (k = 0; k < 4; k++) {
            st[k] = (st[k] ^ ((s1[k] << 7) | (s1[k] >> 25))) * 0x9E3779B1u;
            st[k] = (st[k] ^ ((s2[k] << 11) | (s2[k] >> 21))) * 0x9E3779B1u;
            st[k] = (st[k] ^ ((s3[k] << 17) | (s3[k] >> 15))) * 0x9E3779B1u;
        }
    }
#endif
    if (i < nw) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = mix64(h, w);
    }
    h = mix64(h, (uint64_t)st[0] | ((uint64_t)st[1] << 32));
    return mix64(h, (uint64_t)st[2] | ((uint64_t)st[3] << 32));
}
static uint64_t hash_tmem(uint64_t h, unsigned w0, unsigned nw) {
    while (nw > 0) {
        unsigned st = w0 & 511u, run = 512u - st < nw ? 512u - st : nw;
        h = hash_run(h, gx.tmem + st * 8u, run);
        w0 += run;
        nw -= run;
    }
    return h;
}

/* Chave da textura: descricao da tile + todos os bytes da TMEM que o decodificador pode ler
 * (linhas ate a mascara, nao so ate a area carregada) + paleta. */
static uint64_t tile_key(const GxTile *tl, int tw, int th, int tt) {
    uint64_t h = 0xCBF29CE484222325ull;
    int dw = tl->masks ? (1 << tl->masks) : tw;
    int dh = tl->maskt ? (1 << tl->maskt) : th;
    unsigned rowBytes, span, nw, i;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > 512) dw = 512;
    if (dh > 512) dh = 512;
    h = mix64(h, (uint64_t)tl->fmt | ((uint64_t)tl->siz << 8) | ((uint64_t)tl->line << 16) | ((uint64_t)tl->tmem << 32) |
                     ((uint64_t)tl->palette << 48) | ((uint64_t)(tl->cms | (tl->cmt << 2)) << 56));
    h = mix64(h, (uint64_t)tl->masks | ((uint64_t)tl->maskt << 8) | ((uint64_t)tw << 16) | ((uint64_t)th << 32) |
                     ((uint64_t)tt << 48));
    rowBytes = (unsigned)(dw * bits_of(tl->siz) + 7) / 8u;
    /* (a troca de metades nas linhas impares le o endereco ^ 4: fica na mesma palavra de 8 bytes, entao basta ir ate o
     * fim da palavra da ultima linha. Antes eram +8 bytes a mais: com uma textura que ocupa a TMEM ate o fim da linha
     * (o meio da barra de magia, 24x16 IA8), esses 8 bytes eram restos de cargas anteriores e a chave mudava sem a
     * textura mudar: no acelerador 2D, a barra era refeita e alternava com os nucleos (piscando no Lago Hylia) */
    span = (unsigned)(dh - 1) * tl->line * 8u + ((rowBytes + 7u) & ~7u);
    if (span > 4096u) {
        span = 4096u;
    }
    nw = (span + 7u) / 8u;
    h = hash_tmem(h, tl->tmem, nw);
    if (tl->siz == 3) {
        h = hash_tmem(h, tl->tmem + 256u, nw);
    }
    if (tl->fmt == 2 || tt != 0) {
        int is4 = tl->siz == 0;
        unsigned base = 256u + (is4 ? (unsigned)tl->palette * 16u : 0u);
        unsigned len = is4 ? 16u : 256u;
        h = hash_tmem(h, base, len);
    }
    (void)i;
    return h;
}

static GxTex *tex_decode(const GxTile *tl, int tw, int th, uint64_t key, int tt) {
    int dw = tl->masks ? (1 << tl->masks) : tw;
    int dh = tl->maskt ? (1 << tl->maskt) : th;
    GxTex *tx;
    int s, t;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > 512) dw = 512;
    if (dh > 512) dh = 512;
    const int solid = (tt >> 2) & 3;
    tt &= 3;
    tx = (GxTex *)calloc(1, sizeof *tx);
    if (tx == NULL) {
        return NULL;
    }
    tx->px = (uint32_t *)malloc((size_t)dw * dh * 4);
    if (tx->px == NULL) {
        free(tx);
        return NULL;
    }
    tx->solid = solid;
    tx->w = dw;
    tx->h = dh;
    tx->clipw = tw < dw ? tw : dw;
    tx->cliph = th < dh ? th : dh;
    if (tx->clipw < 1) tx->clipw = 1;
    if (tx->cliph < 1) tx->cliph = 1;
    tx->key = key;
    for (t = 0; t < dh; t++) {
        uint32_t *row = tx->px + (size_t)t * dw;
        for (s = 0; s < dw; s++) {
            uint32_t px = 0;
            switch (tl->siz) {
                case 0: { /* 4 bits */
                    uint8_t b = tmem_at(tl, t, s >> 1, 0);
                    unsigned n = (s & 1) ? (b & 15u) : (b >> 4);
                    if (tl->fmt == 2) {
                        px = tlut_px(n, tl->palette, 1, tt);
                    } else if (tl->fmt == 3) {
                        int i3 = (int)(n >> 1) & 7;
                        int i = (i3 << 5) | (i3 << 2) | (i3 >> 1);
                        px = pack(i, i, i, (n & 1) ? 255 : 0);
                    } else {
                        int i = (int)n * 17;
                        px = pack(i, i, i, i);
                    }
                    break;
                }
                case 1: { /* 8 bits */
                    unsigned b = tmem_at(tl, t, s, 0);
                    if (tl->fmt == 2) {
                        px = tlut_px(b, 0, 0, tt);
                    } else if (tl->fmt == 3) {
                        int i = (int)(b >> 4) * 17, a = (int)(b & 15) * 17;
                        px = pack(i, i, i, a);
                    } else {
                        px = pack((int)b, (int)b, (int)b, (int)b);
                    }
                    break;
                }
                case 2: { /* 16 bits */
                    unsigned c = ((unsigned)tmem_at(tl, t, s * 2, 0) << 8) | tmem_at(tl, t, s * 2 + 1, 0);
                    if (tl->fmt == 2) {
                        px = tt == 3 ? ia16_px(c) : rgba16_px(c);
                        /* CI16 nao existe de verdade; trata como cor direta */
                    } else if (tl->fmt == 3) {
                        px = ia16_px(c);
                    } else if (tl->fmt == 1) {
                        px = pack(128, 128, 128, 255);
                    } else if (tl->fmt == 4) {
                        int i = (int)(c >> 8);
                        px = pack(i, i, i, i);
                    } else {
                        px = rgba16_px(c);
                    }
                    break;
                }
                default: { /* 32 bits: RG no banco baixo, BA no alto */
                    int r = tmem_at(tl, t, s * 2, 0), g = tmem_at(tl, t, s * 2 + 1, 0);
                    int b = tmem_at(tl, t, s * 2, 1), a = tmem_at(tl, t, s * 2 + 1, 1);
                    px = pack(r, g, b, a);
                    break;
                }
            }
            if (solid == 1) {
                if ((px >> 24) >= 16u) {
                    px |= 0xFF000000u; /* menu opaco: o que tem algum alfa fica solido */
                }
            } else if (solid == 2) {
                /* logo do escudo: corte duro em 96 (o que e mais claro que isso e a borda macia: sem o corte vira um contorno branco) */
                px = (px >> 24) >= 96u ? (px | 0xFF000000u) : (px & 0x00FFFFFFu);
            }
            if (gx_gamma_on) {
                px = gamma_px(px);
            }
            row[s] = px;
        }
    }
    return tx;
}

/* memoria por tile: se a TMEM nao mudou (tmemGen) e a tile e a mesma, reaproveita a textura sem
 * recalcular a chave. g_epoch muda sempre que alguma textura sai da cache. */
static struct {
    GxTile tl;
    int tt;
    uint32_t gen, epoch;
    GxTex *tx;
} g_memo[8];

int gx_hd_font_ok; /* 1 dentro das marcas 0x5458543E/F (gfx_hd.c): letras novas so no PRESS START */
int gx_solid_alpha, gx_menu_opaco = 0, gx_logo_opaca = 0; /* gx_solid_alpha: 1 = menu de pausa, 2 = logo do escudo */

GxTex *gx_tex_get(int ti) {
    const GxTile *tl = &gx.tile[ti & 7];
    int tw = (int)(((int)tl->lrs - (int)tl->uls) >> 2) + 1;
    int th = (int)(((int)tl->lrt - (int)tl->ult) >> 2) + 1;
    int tt = (int)((gx.omH >> 14) & 3) | (gx_solid_alpha << 2) | (gx_hd_font_ok ? 16 : 0); /* bits 2-3: alfa solido (1 = menu, 2 = logo); bit 4: letra nova liberada */
    uint64_t key;
    int i;
    GxTex *tx;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw > 512) tw = 512;
    if (th > 512) th = 512;
    {
        int m = ti & 7;
        if (g_memo[m].tx != NULL && g_memo[m].gen == gx.tmemGen && g_memo[m].epoch == g_epoch && g_memo[m].tt == tt &&
            memcmp(&g_memo[m].tl, tl, sizeof *tl) == 0) {
            g_memo[m].tx->use = g_stamp;
            hd_poll(g_memo[m].tx);
            return g_memo[m].tx;
        }
    }
    key = tile_key(tl, tw, th, tt);
    {
        GxTex *h = g_hint[hint_ix(key)];
        if (h != NULL && h->key == key) {
            int m = ti & 7;
            h->use = g_stamp;
            g_memo[m].tl = *tl;
            g_memo[m].tt = tt;
            g_memo[m].gen = gx.tmemGen;
            g_memo[m].epoch = g_epoch;
            g_memo[m].tx = h;
            hd_poll(h);
            return h;
        }
    }
    for (i = 0; i < g_ncache; i++) {
        if (g_cache[i]->key == key) {
            int m = ti & 7;
            g_hint[hint_ix(key)] = g_cache[i];
            g_cache[i]->use = g_stamp;
            g_memo[m].tl = *tl;
            g_memo[m].tt = tt;
            g_memo[m].gen = gx.tmemGen;
            g_memo[m].epoch = g_epoch;
            g_memo[m].tx = g_cache[i];
            hd_poll(g_cache[i]);
            return g_cache[i];
        }
    }
    /* abre espaco: remove os menos usados recentemente que nao estejam em uso neste quadro */
    while (g_ncache >= TEX_CACHE_MAX || g_cacheBytes > gx_tex_cache_bytes) {
        int best = -1;
        uint32_t bu = 0xFFFFFFFFu;
        for (i = 0; i < g_ncache; i++) {
            if (g_cache[i]->use < g_stamp && g_cache[i]->use < bu) {
                bu = g_cache[i]->use;
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        gx_batch_flush(); /* a fila pode usar a textura que vai sair */
        g_cacheBytes -= tex_bytes(g_cache[best]);
        g_epoch++;
        hint_drop(g_cache[best]);
        tex_free(g_cache[best]);
        g_cache[best] = g_cache[--g_ncache];
    }
    {
        const uint64_t t0 = gx_now_ns();
        tx = tex_decode(tl, tw, th, key, tt);
        gx_stN[0]++;
        gx_stNs[0] += gx_now_ns() - t0;
    }
    if (tx == NULL) {
        return NULL;
    }
    tx->hdk = 1;
    if (gx_hd_lookup) {
        /* textura nova do pacote, se esta for uma textura conhecida do jogo */
        tx->hd = gx_hd_lookup(tx, tl, gx.tmemPtr[tl->tmem & 511u], gx.tmemStride[tl->tmem & 511u]);
        tx->hdTried = 1;
    }
    if (tx->hd == NULL && gx_tmem_note) {
        /* o Link da tela de equipamentos desenhado em 2x (gfx_hires.c) */
        tx->hd = gx_hr_prv_lookup(tx, tl, gx.tmemPtr[tl->tmem & 511u], gx.tmemStride[tl->tmem & 511u]);
    }
    tx->use = g_stamp;
    {
        int m = ti & 7;
        g_memo[m].tl = *tl;
        g_memo[m].tt = tt;
        g_memo[m].gen = gx.tmemGen;
        g_memo[m].epoch = g_epoch;
        g_memo[m].tx = tx;
    }
    if (g_ncache < TEX_CACHE_MAX) {
        g_cache[g_ncache++] = tx;
        g_cacheBytes += tex_bytes(tx);
        g_hint[hint_ix(key)] = tx;
    } else {
        /* cache cheio de itens em uso: guarda assim mesmo (vaza ate o proximo flush) */
        static GxTex *spill[2];
        static int sp;
        if (spill[sp] != NULL) {
            gx_batch_flush();
            g_epoch++;
            tex_free(spill[sp]);
        }
        spill[sp] = tx;
        sp ^= 1;
    }
    return tx;
}

/* chamado antes de cada primitiva: refaz as texturas se algo mudou */
void gx_tex_resolve(void) {
    if (gx.texDirty) {
        int t0 = gx.texTile & 7;
        g_stamp++;
        gx.rtex[0] = gx_tex_get(t0);
        gx.rtex[1] = gx_tex_get((t0 + 1) & 7);
        gx.texDirty = 0;
    }
}

/* ------------------------------------------------------------------ cargas */
static void cmd_settile(uint32_t w0, uint32_t w1) {
    int ti = (int)((w1 >> 24) & 7);
    GxTile *tl = &gx.tile[ti];
    tl->fmt = (uint8_t)((w0 >> 21) & 7);
    tl->siz = (uint8_t)((w0 >> 19) & 3);
    tl->line = (uint16_t)((w0 >> 9) & 0x1FF);
    tl->tmem = (uint16_t)(w0 & 0x1FF);
    tl->palette = (uint8_t)((w1 >> 20) & 15);
    tl->cmt = (uint8_t)((w1 >> 18) & 3);
    tl->maskt = (uint8_t)((w1 >> 14) & 15);
    tl->shiftt = (uint8_t)((w1 >> 10) & 15);
    tl->cms = (uint8_t)((w1 >> 8) & 3);
    tl->masks = (uint8_t)((w1 >> 4) & 15);
    tl->shifts = (uint8_t)(w1 & 15);
    gx.texDirty = 1;
}

/* quadros intermediarios (gfx_hook.c): G_SETTILESIZE trocado (texturas que rolam; NULL = nenhum) */
int (*gx_tile_lookup)(const void *cmd, uint32_t *w0, uint32_t *w1);

static void cmd_settilesize(uint32_t w0, uint32_t w1) {
    GxTile *tl = &gx.tile[(w1 >> 24) & 7];
    tl->uls = (uint16_t)((w0 >> 12) & 0xFFF);
    tl->ult = (uint16_t)(w0 & 0xFFF);
    tl->lrs = (uint16_t)((w1 >> 12) & 0xFFF);
    tl->lrt = (uint16_t)(w1 & 0xFFF);
    gx.texDirty = 1;
}

/* escreve 8 bytes na TMEM (palavra 'w' = indice de 64 bits), trocando as metades se 'swap' */
static inline void tmem_word(unsigned w, const uint8_t *src, int swap) {
    unsigned a = (w * 8u) & 0xFFFu;
    if (swap) {
        memcpy(gx.tmem + a, src + 4, 4);
        memcpy(gx.tmem + a + 4, src, 4);
    } else {
        memcpy(gx.tmem + a, src, 8);
    }
}

/* texturas novas: de onde veio cada palavra da TMEM que a carga escreveu (uma tile pode comecar no meio de
 * uma carga: duas texturas carregadas juntas, ou um pedaco de uma textura grande) */
static void tmem_note(unsigned t0, unsigned rows, unsigned line, unsigned rowWords, const uint8_t *src, size_t stride,
                      unsigned kind) { /* kind: 0 bloco, senao a distancia entre linhas na memoria */
    unsigned r, c;
    if (!gx_hd_on && !gx_tmem_note) {
        return;
    }
    if (rows * line > 512u) {
        rows = (512u + line - 1u) / (line ? line : 1u);
    }
    for (r = 0; r < rows; r++) {
        for (c = 0; c < rowWords && c < 512u; c++) {
            const unsigned w = (t0 + r * line + c) & 511u;
            gx.tmemPtr[w] = src ? src + r * stride + c * 8u : NULL;
            gx.tmemStride[w] = (uint16_t)kind;
        }
    }
}

static void cmd_loadblock(uint32_t w0, uint32_t w1) {
    const GxTile *tl = &gx.tile[(w1 >> 24) & 7];
    unsigned uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    unsigned lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
    const uint8_t *src = gx.timg.addr;
    unsigned ntexels = lrs - uls + 1;
    unsigned bpt = (unsigned)bits_of(tl->siz) / 8u; /* bytes por texel (4b conta como 0 -> 1/2) */
    if (lrs < uls || ntexels > 2048u) {
        /* comando invalido (no RDP no maximo 2048 texels): uma display list sendo trocada pelo jogo no meio
         * de um desenho (quadro intermediario) chegou a ler o fim do executavel e derrubar o jogo */
        static int warned;
        if (warned < 4) {
            warned++;
            fprintf(stderr, "[gfx] LOADBLOCK invalido ignorado: uls=%u lrs=%u dxt=%u tile=%u timg=%p (dl %08x)\n", uls, lrs, dxt,
                    (unsigned)((w1 >> 24) & 7), (const void *)gx.timg.addr, gx.curCmd);
        }
        return;
    }
    unsigned nbytes, nw, i;
    int tbits = bits_of(gx.timg.siz);
    size_t off;
    gx.nTexLoad++;
    gx.texDirty = 1;
    gx.tmemSrc[tl->tmem & 511u] = gx.timgRaw;
    gx.tmemPtr[tl->tmem & 511u] = NULL;
    gx.tmemGen++;
    if (src == NULL) {
        return;
    }
    off = ((size_t)ult * gx.timg.width + uls) * (size_t)tbits / 8u;
    src += off;
    if (tl->siz == 0) {
        nbytes = (ntexels + 1) / 2;
    } else {
        nbytes = ntexels * bpt;
    }
    {
        const unsigned words = tl->siz == 3 ? (ntexels * 4u + 7u) / 8u : (nbytes + 7u) / 8u;
        tmem_note((unsigned)tl->tmem, 1, words, words, tl->siz == 3 ? NULL : src, 0, 0);
    }
    /* depois da anotacao: a 1a palavra sempre aponta para o inicio da carga (32 bits inclusive) */
    gx.tmemPtr[tl->tmem & 511u] = src;
    gx.tmemStride[tl->tmem & 511u] = 0;
    gx_touch(src, tl->siz == 3 ? (size_t)ntexels * 4u : (size_t)nbytes);
    gx_batch_reading(src, tl->siz == 3 ? (size_t)ntexels * 4u : (size_t)nbytes);
    src = src_safe(src, tl->siz == 3 ? (size_t)ntexels * 4u : ((size_t)nbytes + 7u) & ~(size_t)7u);
    if (tl->siz == 3) {
        /* 32 bits: cada texel RGBA vai metade para cada banco */
        unsigned n = ntexels;
        for (i = 0; i < n; i++) {
            unsigned word = i >> 1;
            int swap = (int)((((unsigned long)word * dxt) >> 11) & 1);
            unsigned a_lo = ((unsigned)tl->tmem * 8u + i * 2u);
            unsigned a_hi = a_lo + 0x800u;
            unsigned row_base = (unsigned)tl->tmem * 8u + (word / 4u) * 8u; /* nao usado */
            (void)row_base;
            if (swap) {
                a_lo ^= 4u;
                a_hi ^= 4u;
            }
            gx.tmem[a_lo & 0xFFFu] = src[i * 4];
            gx.tmem[(a_lo + 1) & 0xFFFu] = src[i * 4 + 1];
            gx.tmem[a_hi & 0xFFFu] = src[i * 4 + 2];
            gx.tmem[(a_hi + 1) & 0xFFFu] = src[i * 4 + 3];
        }
        return;
    }
    nw = (nbytes + 7) / 8;
    i = 0;
    if (dxt == 0) {
        /* sem troca de metades (dxt 0): as palavras inteiras vao num bloco so (com a volta da TMEM) */
        unsigned a = ((unsigned)tl->tmem * 8u) & 0xFFFu, len = (nbytes / 8u) * 8u;
        const uint8_t *sp = src;
        while (len > 0) {
            unsigned chunk = len < 0x1000u - a ? len : 0x1000u - a;
            memcpy(gx.tmem + a, sp, chunk);
            sp += chunk;
            len -= chunk;
            a = 0;
        }
        i = nbytes / 8u;
    } else {
        /* a troca de metades ((i*dxt)>>11 & 1) muda so a cada linha da textura: copia trechos inteiros
         * com a mesma troca (a TMEM nao da a volta no meio: senao palavra a palavra) */
        const unsigned end = nbytes / 8u;
        while (i < end) {
            const unsigned q = (unsigned)(((unsigned long)i * dxt) >> 11);
            unsigned j = (unsigned)((((unsigned long)(q + 1u) << 11) + dxt - 1u) / dxt);
            const unsigned a = (((unsigned)tl->tmem + i) * 8u) & 0xFFFu;
            if (j > end) {
                j = end;
            }
            if (j <= i) {
                j = i + 1u;
            }
            if (a + (j - i) * 8u > 0x1000u) {
                for (; i < j; i++) {
                    tmem_word((unsigned)tl->tmem + i, src + i * 8, (int)(q & 1u));
                }
                continue;
            }
            if (!(q & 1u)) {
                memcpy(gx.tmem + a, src + i * 8, (size_t)(j - i) * 8u);
            } else {
                uint8_t *d = gx.tmem + a;
                const uint8_t *sp = src + i * 8;
                unsigned k = 0, len = (j - i) * 8u;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                for (; k + 16u <= len; k += 16u) {
                    vst1q_u8(d + k, vreinterpretq_u8_u32(vrev64q_u32(vreinterpretq_u32_u8(vld1q_u8(sp + k)))));
                }
#endif
                for (; k < len; k += 8u) {
                    memcpy(d + k, sp + k + 4, 4);
                    memcpy(d + k + 4, sp + k, 4);
                }
            }
            i = j;
        }
    }
    for (; i < nw; i++) {
        uint8_t tmp[8];
        unsigned k, n = nbytes - i * 8 < 8 ? nbytes - i * 8 : 8;
        int swap = (int)((((unsigned long)i * dxt) >> 11) & 1);
        memset(tmp, 0, 8);
        for (k = 0; k < n; k++) {
            tmp[k] = src[i * 8 + k];
        }
        tmem_word((unsigned)tl->tmem + i, tmp, swap);
    }
}

static void cmd_loadtile(uint32_t w0, uint32_t w1) {
    const GxTile *tl = &gx.tile[(w1 >> 24) & 7];
    unsigned uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    unsigned lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    unsigned t, n = lrs - uls + 1;
    int tbits = bits_of(gx.timg.siz);
    unsigned rowBytes;
    gx.nTexLoad++;
    gx.texDirty = 1;
    gx.tmemSrc[tl->tmem & 511u] = gx.timgRaw;
    gx.tmemPtr[tl->tmem & 511u] = NULL;
    gx.tmemGen++;
    if (gx.timg.addr == NULL || lrs < uls || lrt < ult) {
        return;
    }
    rowBytes = (n * (unsigned)bits_of(tl->siz) + 7) / 8;
    {
        const uint8_t *a0 = gx.timg.addr + (((size_t)ult * gx.timg.width + uls) * (size_t)tbits) / 8u;
        const unsigned stb = gx.timg.width * (unsigned)tbits / 8u;
        if (tl->line > 0) {
            tmem_note((unsigned)tl->tmem, lrt - ult + 1u, tl->line, tl->siz == 3 ? tl->line : (rowBytes + 7u) / 8u,
                      tl->siz == 3 ? NULL : a0, stb, stb ? stb : 1u);
        }
        /* depois da anotacao: a 1a palavra sempre aponta para o inicio da carga (32 bits inclusive: o logo) */
        gx.tmemPtr[tl->tmem & 511u] = a0;
        gx.tmemStride[tl->tmem & 511u] = (uint16_t)stb;
    }
    {
        /* tudo o que esta carga le (da 1a a ultima linha) */
        const uint8_t *a = gx.timg.addr + (((size_t)ult * gx.timg.width + uls) * (size_t)tbits) / 8u;
        const uint8_t *b = gx.timg.addr + (((size_t)lrt * gx.timg.width + uls) * (size_t)tbits) / 8u +
                           (tl->siz == 3 ? (size_t)n * 4u : (size_t)rowBytes);
        gx_batch_reading(a, (size_t)(b - a));
    }
    for (t = ult; t <= lrt; t++) {
        const uint8_t *src = gx.timg.addr + (((size_t)t * gx.timg.width + uls) * (size_t)tbits) / 8u;
        gx_touch(src, tl->siz == 3 ? (size_t)n * 4u : (size_t)rowBytes);
        src = src_safe(src, tl->siz == 3 ? (size_t)n * 4u : (size_t)rowBytes);
        unsigned rt = t - ult;
        unsigned k;
        if (tl->siz == 3) {
            k = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            {
                /* RG de cada texel vao seguidos para o banco baixo e BA para o alto (4 texels = 8 bytes por
                 * banco); linha impar troca as metades de 4 bytes (vrev64). Igual ao laco abaixo. */
                unsigned a0 = ((unsigned)tl->tmem * 8u + rt * tl->line * 8u) & 0xFFFu;
                if (a0 + n * 2u <= 0x800u) {
                    uint8_t *lo = gx.tmem + a0, *hi = gx.tmem + a0 + 0x800u;
                    for (; k + 4 <= n; k += 4) {
                        uint16x4x2_t v = vld2_u16((const uint16_t *)(const void *)(src + k * 4));
                        uint32x2_t rg = vreinterpret_u32_u16(v.val[0]), ba = vreinterpret_u32_u16(v.val[1]);
                        if (rt & 1) {
                            rg = vrev64_u32(rg);
                            ba = vrev64_u32(ba);
                        }
                        vst1_u8(lo + k * 2u, vreinterpret_u8_u32(rg));
                        vst1_u8(hi + k * 2u, vreinterpret_u8_u32(ba));
                    }
                }
            }
#endif
            for (; k < n; k++) {
                unsigned a_lo = (unsigned)tl->tmem * 8u + rt * tl->line * 8u + k * 2u;
                unsigned a_hi = a_lo + 0x800u;
                if (rt & 1) {
                    a_lo ^= 4u;
                    a_hi ^= 4u;
                }
                gx.tmem[a_lo & 0xFFFu] = src[k * 4];
                gx.tmem[(a_lo + 1) & 0xFFFu] = src[k * 4 + 1];
                gx.tmem[a_hi & 0xFFFu] = src[k * 4 + 2];
                gx.tmem[(a_hi + 1) & 0xFFFu] = src[k * 4 + 3];
            }
        } else {
            unsigned a0 = (unsigned)tl->tmem * 8u + rt * tl->line * 8u;
            if ((a0 & 0xFFFu) + rowBytes <= 0x1000u) {
                /* sem dar a volta na TMEM: linha par = copia direta; impar = metades de 4 bytes trocadas
                 * (o mesmo que o laco byte a byte abaixo, com a ^= 4) */
                uint8_t *d = gx.tmem + (a0 & 0xFFFu);
                if (!(rt & 1)) {
                    memcpy(d, src, rowBytes);
                } else {
                    for (k = 0; k + 8 <= rowBytes; k += 8) {
                        memcpy(d + k + 4, src + k, 4);
                        memcpy(d + k, src + k + 4, 4);
                    }
                    for (; k < rowBytes; k++) {
                        d[k ^ 4u] = src[k];
                    }
                }
            } else {
                for (k = 0; k < rowBytes; k++) {
                    unsigned a = a0 + k;
                    if (rt & 1) {
                        a ^= 4u;
                    }
                    gx.tmem[a & 0xFFFu] = src[k];
                }
            }
        }
    }
}

static void cmd_loadtlut(uint32_t w0, uint32_t w1) {
    const GxTile *tl = &gx.tile[(w1 >> 24) & 7];
    unsigned count = (((w1 >> 12) & 0xFFF) >> 2) + 1;
    unsigned uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    const uint8_t *src = gx.timg.addr;
    unsigned i;
    gx.nTexLoad++;
    gx.texDirty = 1;
    gx.tmemPtr[tl->tmem & 511u] = NULL;
    gx.tmemSrc[tl->tmem & 511u] = gx.timgRaw;
    gx.tmemGen++;
    if (src == NULL) {
        return;
    }
    src += (((size_t)ult * gx.timg.width + uls) * 2u);
    tmem_note((unsigned)tl->tmem, 1, count, count, NULL, 0, 0);
    gx_touch(src, (size_t)count * 2u);
    gx_batch_reading(src, (size_t)count * 2u);
    src = src_safe(src, (size_t)count * 2u);
    i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
        /* cada cor de 16 bits repetida 4 vezes numa palavra de 8 bytes; 4 cores por vez se nao da a volta */
        const unsigned n = count < 256u ? count : 256u;
        const unsigned a0 = ((unsigned)tl->tmem * 8u) & 0xFFFu;
        if (a0 + n * 8u <= 0x1000u) {
            uint8_t *d = gx.tmem + a0;
            for (; i + 4u <= n; i += 4u) {
                const uint16x4_t v = vreinterpret_u16_u8(vld1_u8(src + i * 2u));
                vst1q_u16((uint16_t *)(d + i * 8u), vcombine_u16(vdup_lane_u16(v, 0), vdup_lane_u16(v, 1)));
                vst1q_u16((uint16_t *)(d + i * 8u + 16u), vcombine_u16(vdup_lane_u16(v, 2), vdup_lane_u16(v, 3)));
            }
        }
    }
#endif
    for (; i < count && i < 256; i++) {
        unsigned a = ((unsigned)tl->tmem * 8u + i * 8u) & 0xFFFu;
        gx.tmem[a] = gx.tmem[a + 2] = gx.tmem[a + 4] = gx.tmem[a + 6] = src[i * 2];
        gx.tmem[a + 1] = gx.tmem[a + 3] = gx.tmem[a + 5] = gx.tmem[a + 7] = src[i * 2 + 1];
    }
}

/* ------------------------------------------------------------------ retangulos */
static inline void clip_rect(int *x0, int *y0, int *x1, int *y1) {
    if (*y1 > gx.maxYc) *y1 = gx.maxYc;
    if (*x1 > (int)gx.cimg.width - 1) *x1 = (int)gx.cimg.width - 1;
    if (*x0 < (int)gx.scissorX0) *x0 = (int)gx.scissorX0;
    if (*y0 < (int)gx.scissorY0) *y0 = (int)gx.scissorY0;
    if (*x1 > (int)gx.scissorX1 - 1) *x1 = (int)gx.scissorX1 - 1;
    if (*y1 > (int)gx.scissorY1 - 1) *y1 = (int)gx.scissorY1 - 1;
}

/* resolucao maior: o retangulo que o jogo pediu (em 1/4 de pixel do 320x240) no alvo escalado. Inclusivo
 * (copia/preenchimento): as bordas arredondadas (vizinhos encostam sem fresta); 1/2 ciclos: como o RDP,
 * pixels com centro dentro */
static void hr_rect(int cyc, int ulx, int uly, int lrx, int lry, int *x0, int *y0, int *x1, int *y1) {
    const float r = gx.rs;
    if (!gx.hrActive) {
        return;
    }
    if (cyc >= GX_CYC_COPY) {
        *x0 = (int)((float)(ulx >> 2) * r + 0.5f);
        *y0 = (int)((float)(uly >> 2) * r + 0.5f);
        *x1 = (int)((float)((lrx >> 2) + 1) * r + 0.5f) - 1;
        *y1 = (int)((float)((lry >> 2) + 1) * r + 0.5f) - 1;
    } else {
        *x0 = (int)ceilf((float)ulx * r * 0.25f - 0.0001f);
        *y0 = (int)ceilf((float)uly * r * 0.25f - 0.0001f);
        *x1 = (int)ceilf((float)lrx * r * 0.25f - 0.0001f) - 1;
        *y1 = (int)ceilf((float)lry * r * 0.25f - 0.0001f) - 1;
    }
}

static inline int cycle_type(void) {
    return (int)((gx.omH >> GX_CYC_SHIFT) & 3);
}

void gx_fillrect(uint32_t w0, uint32_t w1) {
    int lrx = (int)((w0 >> 12) & 0xFFF), lry = (int)(w0 & 0xFFF);
    int ulx = (int)((w1 >> 12) & 0xFFF), uly = (int)(w1 & 0xFFF);
    int cyc = cycle_type();
    int x0, y0, x1, y1, x, y;
    if (gx_ge_other((ulx >> 2) * 2 - 2, (uly >> 2) * 2 - 2, (lrx >> 2) * 2 + 4, (lry >> 2) * 2 + 4)) {
        return; /* (varredura do 2D, gfx_ge.c) */
    }
    gx.nRect++;
    if (gx.cimg.addr == NULL) {
        return;
    }
    gx_pipe_begin();
    gx.drawKind = 'R';
    if (cyc >= GX_CYC_COPY) { /* inclusivo */
        x0 = ulx >> 2;
        y0 = uly >> 2;
        x1 = lrx >> 2;
        y1 = lry >> 2;
    } else {
        x0 = (ulx + 3) >> 2;
        y0 = (uly + 3) >> 2;
        x1 = ((lrx + 3) >> 2) - 1;
        y1 = ((lry + 3) >> 2) - 1;
    }
    hr_rect(cyc, ulx, uly, lrx, lry, &x0, &y0, &x1, &y1);
    if (cyc < GX_CYC_COPY) {
        /* Tela inteira em 1/2 ciclos: o jogo pede (0, 0, 319, 239), e nesses modos o canto de baixo/direita nao entra:
         * a ultima linha e a ultima coluna ficavam de fora dos escurecimentos (transicoes, cortinas do L1). No N64 a
         * TV corta essa borda; aqui ela aparece, entao o retangulo que chega na borda vai ate o fim da imagem. */
        const int rw = gx.hrActive ? (int)gx.cimgRawW : (int)gx.cimg.width;
        const int rh = gx.hrActive ? (int)((float)gx.hrH / gx.rs + 0.5f) : 240;
        if (lrx == (rw - 1) * 4) {
            x1 = (gx.hrActive ? gx.hrW : (int)gx.cimg.width) - 1;
        }
        if (lry == (rh - 1) * 4) {
            y1 = (gx.hrActive ? gx.hrH : rh) - 1;
        }
    }
    clip_rect(&x0, &y0, &x1, &y1);
    if (x1 < x0 || y1 < y0) {
        return;
    }
    if (cyc == GX_CYC_FILL && gx_fill_fast(x0, y0, x1, y1)) {
        /* na fila do caminho rapido */
    } else if (cyc == GX_CYC_FILL && gx.cimg.siz == 2 && !gx.nProbe) {
        /* 16 bits: pixel par recebe a metade alta da cor de preenchimento, impar a baixa (como gx_fill_px) */
        uint8_t c[4];
        c[0] = (uint8_t)(gx.fillColor >> 24);
        c[1] = (uint8_t)(gx.fillColor >> 16);
        c[2] = (uint8_t)(gx.fillColor >> 8);
        c[3] = (uint8_t)gx.fillColor;
        if (gx.cle) { /* (imagem maior na ordem do processador) */
            uint8_t t0 = c[0], t2 = c[2];
            c[0] = c[1];
            c[1] = t0;
            c[2] = c[3];
            c[3] = t2;
        }
        for (y = y0; y <= y1; y++) {
            uint8_t *p = gx.cimg.addr + ((size_t)y * gx.cimg.width + (size_t)x0) * 2u;
            for (x = x0; x <= x1; x++, p += 2) {
                const uint8_t *s = c + ((x & 1) << 1);
                p[0] = s[0];
                p[1] = s[1];
            }
        }
    } else if (cyc == GX_CYC_FILL) {
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                gx_fill_px(x, y);
            }
        }
    } else {
        static const int noShade[4] = { 0, 0, 0, 255 };
        GxPrim P;
        gx_tex_resolve();
        gx_pipe_begin();
        gx_prim_const(&P, noShade);
        gx_draw_rect_prim(&P, x0, y0, x1, y1);
    }
}

void gx_texrect(uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, int flip) {
    if (gx_hr_skip_rect()) {
        return; /* fundo da pausa: a imagem maior guardada ja foi desenhada (gfx_hires.c) */
    }
    int lrx = (int)((w0 >> 12) & 0xFFF), lry = (int)(w0 & 0xFFF);
    int tile = (int)((w1 >> 24) & 7);
    int ulx = (int)((w1 >> 12) & 0xFFF), uly = (int)(w1 & 0xFFF);
    float s0 = (float)(int16_t)(h1 >> 16), t0 = (float)(int16_t)(h1 & 0xFFFF);
    float dsdx = (float)(int16_t)(h2 >> 16) / 1024.0f, dtdy = (float)(int16_t)(h2 & 0xFFFF) / 1024.0f;
    int cyc = cycle_type();
    int x0, y0, x1, y1, x, y;
    float fx0 = (float)ulx * 0.25f * gx.rs, fy0 = (float)uly * 0.25f * gx.rs;
    static const int noShade[4] = { 255, 255, 255, 255 };
    if (gx_ge_texrect(w0, w1, h1, h2, flip)) {
        return; /* varredura do 2D, ou foi para o acelerador (gfx_ge.c) */
    }
    gx.nRect++;
    gx.drawKind = 'R';
    if (gx.cimg.addr == NULL) {
        return;
    }
    gx_pipe_begin();
    if (cyc >= GX_CYC_COPY) {
        x0 = ulx >> 2;
        y0 = uly >> 2;
        x1 = lrx >> 2;
        y1 = lry >> 2;
        dsdx *= 0.25f; /* no modo copia o passo e 4 texels por pixel para 1:1 */
    } else {
        x0 = (ulx + 3) >> 2;
        y0 = (uly + 3) >> 2;
        x1 = ((lrx + 3) >> 2) - 1;
        y1 = ((lry + 3) >> 2) - 1;
    }
    if (gx.hrActive) {
        hr_rect(cyc, ulx, uly, lrx, lry, &x0, &y0, &x1, &y1);
        dsdx /= gx.rs; /* passo da textura por pixel do alvo maior */
        dtdy /= gx.rs;
    }
    {
        /* escolhe a tile de renderizacao */
        int old = gx.texTile;
        if (gx.texTile != tile) {
            gx.texTile = tile;
            gx.texDirty = 1;
        }
        (void)old;
    }
    clip_rect(&x0, &y0, &x1, &y1);
    if (x1 < x0 || y1 < y0) {
        return;
    }
    gx_tex_resolve();
    if (cyc >= GX_CYC_COPY) {
        gx_batch_flush(); /* o modo copia desenha na hora */
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                float dx = (float)x - fx0, dy = (float)y - fy0;
                float S, T;
                if (!flip) {
                    S = s0 + dx * dsdx * 32.0f;
                    T = t0 + dy * dtdy * 32.0f;
                } else {
                    S = s0 + dy * dtdy * 32.0f;
                    T = t0 + dx * dsdx * 32.0f;
                }
                gx_copy_px(x, y, S, T);
            }
        }
    } else {
        GxPrim P;
        gx_pipe_begin();
        gx_prim_const(&P, noShade);
        P.hasTex = 1;
        P.fx0 = fx0;
        P.fy0 = fy0;
        P.f0[0] = s0;
        P.f0[1] = t0;
        if (!flip) {
            P.fdx[0] = dsdx * 32.0f;
            P.fdy[1] = dtdy * 32.0f;
        } else {
            P.fdy[0] = dtdy * 32.0f;
            P.fdx[1] = dsdx * 32.0f;
        }
        gx_draw_rect_prim(&P, x0, y0, x1, y1);
    }
}

/* ------------------------------------------------------------------ fundos (S2DEX2) */
/* uObjBg / uObjScaleBg (gs2dex.h) como o jogo os escreve (CPU nativa; ponteiro de 32 bits) */
typedef struct {
    uint16_t imageX, imageW;
    int16_t frameX;
    uint16_t frameW;
    uint16_t imageY, imageH;
    int16_t frameY;
    uint16_t frameH;
    uint32_t imagePtr;
    uint16_t imageLoad;
    uint8_t imageFmt, imageSiz;
    uint16_t imagePal, imageFlip;
    uint16_t scaleW, scaleH; /* so no uObjScaleBg (G_BG_1CYC) */
    int32_t imageYorig;
} GxObjBg;

/* opcao bg_accelerator: os fundos JPEG (praca do mercado, frente do Templo do Tempo, entrada da cidade...) sao uma imagem
 * 2D colada na tela inteira: o chip 2D do aparelho a amplia direto na imagem do desenho (video_miyoo.c ge_bg_blit),
 * com suavizacao e sem os nucleos copiarem pixel a pixel. Vale quando a imagem do desenho esta na memoria do chip
 * (desenho direto, cor direta) e o fundo e o caso simples (RGBA16, sem espelhar, sem dar a volta na imagem, sem teste
 * de alfa); senao, como antes. */
int gx_bg_ge = 1;
int (*gx_ge_bg)(uint8_t *dst, int dstW, int dstH, const uint8_t *img, int iw, int ih, int sx, int sy, int sw, int sh,
                int dx, int dy, int dw, int dh);
unsigned gx_bg_stat[2]; /* (diagnostico) pelo chip, pela CPU */

/* gSPBgRectCopy / gSPBgRect1Cyc: desenha a imagem de fundo (os JPEGs das salas pre-renderizadas,
 * ja decodificados pelo jogo em RGBA16) direto no framebuffer, com a mesma geometria do S2DEX:
 * moldura (frameX/Y/W/H) na tela, imagem a partir de (imageX, imageY), dando a volta na imagem. */
void gx_bg_rect(uint32_t w1, int copy) {
    const GxObjBg *bg = (const GxObjBg *)gx_resolve(w1);
    const uint8_t *img;
    int iw, ih, fx, fy, fw, fh, x0, y0, x1, y1, x, y, is4, bpp;
    uint32_t stepS = 1024, stepT = 1024, s0, t0;
    if (gx_ge_other(0, 0, 640, 480)) {
        return; /* (varredura do 2D, gfx_ge.c) */
    }
    if (bg == NULL || gx.cimg.addr == NULL) {
        return;
    }
    gx_touch(bg, sizeof *bg);
    img = (const uint8_t *)gx_resolve(bg->imagePtr);
    iw = bg->imageW >> 2;
    ih = bg->imageH >> 2;
    if (img == NULL || iw <= 0 || ih <= 0 || iw > 1024 || ih > 1024) {
        return;
    }
    is4 = bg->imageSiz == 0;
    bpp = bg->imageSiz == 2 ? 2 : 1; /* bytes por texel (4 bits: meio byte) */
    gx_touch(img, is4 ? (size_t)iw * ih / 2 : (size_t)iw * ih * bpp);
    fx = bg->frameX >> 2;
    fy = bg->frameY >> 2;
    fw = bg->frameW >> 2;
    fh = bg->frameH >> 2;
    if (!copy && bg->scaleW != 0 && bg->scaleH != 0) {
        stepS = bg->scaleW;
        stepT = bg->scaleH;
    }
    s0 = (uint32_t)(bg->imageX >> 5) << 10;
    t0 = (uint32_t)(bg->imageY >> 5) << 10;
    if (gx.hrActive) {
        const float r = gx.rs;
        const int X0 = (int)((float)fx * r + 0.5f), Y0 = (int)((float)fy * r + 0.5f);
        fw = (int)((float)(fx + fw) * r + 0.5f) - X0;
        fh = (int)((float)(fy + fh) * r + 0.5f) - Y0;
        fx = X0;
        fy = Y0;
        stepS = (uint32_t)((float)stepS / r + 0.5f);
        stepT = (uint32_t)((float)stepT / r + 0.5f);
    }
    x0 = fx;
    y0 = fy;
    x1 = fx + fw - 1;
    y1 = fy + fh - 1;
    clip_rect(&x0, &y0, &x1, &y1);
    if (x1 < x0 || y1 < y0) {
        return;
    }
    gx.nRect++;
    gx_batch_flush(); /* desenha na hora, como o modo copia */
    if (gx_bg_ge && gx_ge_bg != NULL && gx.cle && !gx_capturing && bg->imageFmt == 0 && bpp == 2 && !(bg->imageFlip & 1) &&
        !(copy && (gx.omL & 3))) {
        /* o retangulo da imagem que cai na moldura (x0..x1, y0..y1): sem dar a volta, o chip 2D faz */
        const uint32_t ss0 = s0 + (uint32_t)(x0 - fx) * stepS, tt0 = t0 + (uint32_t)(y0 - fy) * stepT;
        const int sx = (int)(ss0 >> 10), sy = (int)(tt0 >> 10);
        const int sw = (int)(((uint64_t)(x1 - x0 + 1) * stepS + 512u) >> 10), sh = (int)(((uint64_t)(y1 - y0 + 1) * stepT + 512u) >> 10);
        if (sx + sw <= iw && sy + sh <= ih &&
            gx_ge_bg(gx.cimg.addr, (int)gx.cimg.width, y1 + 1, img, iw, ih, sx, sy, sw, sh, x0, y0, x1 - x0 + 1, y1 - y0 + 1)) {
            gx_bg_stat[0]++;
            return;
        }
    }
    gx_bg_stat[1]++;
    for (y = y0; y <= y1; y++) {
        uint32_t tt = t0 + (uint32_t)(y - fy) * stepT;
        int ty = (int)((tt >> 10) % (uint32_t)ih);
        uint8_t *dst = gx.cimg.addr + ((size_t)y * gx.cimg.width) * 2u;
        for (x = x0; x <= x1; x++) {
            uint32_t ss = s0 + (uint32_t)(x - fx) * stepS;
            int tx = (int)((ss >> 10) % (uint32_t)iw);
            unsigned c;
            if (bg->imageFlip & 1) {
                tx = iw - 1 - tx;
            }
            if (bg->imageFmt == 2) { /* CI: cor pela paleta carregada na TMEM */
                unsigned idx, a;
                if (is4) {
                    uint8_t b = img[((size_t)ty * iw + tx) >> 1];
                    idx = (tx & 1) ? (b & 15u) : (b >> 4);
                    idx += (unsigned)(bg->imagePal & 15) * 16u;
                } else {
                    idx = img[(size_t)ty * iw + tx];
                }
                a = 0x800u + idx * 8u;
                c = ((unsigned)gx.tmem[a & 0xFFFu] << 8) | gx.tmem[(a + 1) & 0xFFFu];
            } else if (bpp == 2) {
                const uint8_t *p = img + ((size_t)ty * iw + tx) * 2u;
                c = ((unsigned)p[0] << 8) | p[1];
            } else { /* I8/IA8 nao usados pelo jogo: cinza */
                unsigned i = img[(size_t)ty * iw + tx] >> 3;
                c = (i << 11) | (i << 6) | (i << 1) | 1;
            }
            if (copy) {
                if ((gx.omL & 3) && !(c & 1)) {
                    continue; /* teste de alfa do modo copia */
                }
            } else {
                c |= 1; /* combinador do jogo: cor da textura, alfa 1 */
            }
            if (gx.cle) {
                dst[x * 2] = (uint8_t)c;
                dst[x * 2 + 1] = (uint8_t)(c >> 8);
            } else {
                dst[x * 2] = (uint8_t)(c >> 8);
                dst[x * 2 + 1] = (uint8_t)c;
            }
        }
    }
}

/* ------------------------------------------------------------------ despacho */
void gx_rdp_cmd(uint32_t op, uint32_t w0, uint32_t w1) {
    switch (op) {
        case GX_SETCIMG:
            gx.cimg.fmt = (uint8_t)((w0 >> 21) & 7);
            gx.cimg.siz = (uint8_t)((w0 >> 19) & 3);
            gx.cimgRawW = (uint16_t)((w0 & 0xFFF) + 1);
            gx.cimgRaw = (uint8_t *)(uintptr_t)gx_resolve(w1);
            gx_touch(gx.cimgRaw, (size_t)gx.cimgRawW * 240u * (gx.cimg.siz == 3 ? 4u : 2u));
            gx_hr_retarget(); /* cimg/zimg: o pedido ou o buffer maior */
            break;
        case GX_SETZIMG:
            gx.zimgRaw = (uint8_t *)(uintptr_t)gx_resolve(w1);
            gx_touch(gx.zimgRaw, 320u * 240u * 2u);
            gx_hr_retarget();
            break;
        case GX_SETTIMG:
            gx.timg.fmt = (uint8_t)((w0 >> 21) & 7);
            gx.timg.siz = (uint8_t)((w0 >> 19) & 3);
            gx.timg.width = (uint16_t)((w0 & 0xFFF) + 1);
            gx.timg.addr = (const uint8_t *)gx_resolve(w1);
            gx.timgRaw = w1;
            gx_hr_reading(gx.timg.addr); /* textura tirada da tela: a tela do N64 tem que estar em dia */
            break;
        case GX_SETCOMBINE:
            gx.combW0 = w0 & 0x00FFFFFFu;
            gx.combW1 = w1;
            break;
        case GX_SETENVCOLOR:
            gx.env[0] = (uint8_t)(w1 >> 24);
            gx.env[1] = (uint8_t)(w1 >> 16);
            gx.env[2] = (uint8_t)(w1 >> 8);
            gx.env[3] = (uint8_t)w1;
            if (gx_gamma_on) {
                gx.env[0] = gx_gamma[gx.env[0]];
                gx.env[1] = gx_gamma[gx.env[1]];
                gx.env[2] = gx_gamma[gx.env[2]];
            }
            break;
        case GX_SETPRIMCOLOR:
            if (gx_cmd_lookup != NULL) {
                const uint32_t *r = gx_cmd_lookup((const void *)(uintptr_t)gx.curCmd);
                if (r != NULL) {
                    w0 = r[0];
                    w1 = r[1];
                }
            }
            gx.primLodMin = (uint8_t)((w0 >> 8) & 0x1F);
            gx.primLodFrac = (uint8_t)w0;
            gx.prim[0] = (uint8_t)(w1 >> 24);
            gx.prim[1] = (uint8_t)(w1 >> 16);
            gx.prim[2] = (uint8_t)(w1 >> 8);
            gx.prim[3] = (uint8_t)w1;
            if (gx_gamma_on) {
                gx.prim[0] = gx_gamma[gx.prim[0]];
                gx.prim[1] = gx_gamma[gx.prim[1]];
                gx.prim[2] = gx_gamma[gx.prim[2]];
            }
            break;
        case GX_SETBLENDCOLOR:
            gx.blendc[0] = (uint8_t)(w1 >> 24);
            gx.blendc[1] = (uint8_t)(w1 >> 16);
            gx.blendc[2] = (uint8_t)(w1 >> 8);
            gx.blendc[3] = (uint8_t)w1;
            break;
        case GX_SETFOGCOLOR:
            gx.fogc[0] = (uint8_t)(w1 >> 24);
            gx.fogc[1] = (uint8_t)(w1 >> 16);
            gx.fogc[2] = (uint8_t)(w1 >> 8);
            gx.fogc[3] = (uint8_t)w1;
            if (gx_gamma_on) {
                gx.fogc[0] = gx_gamma[gx.fogc[0]];
                gx.fogc[1] = gx_gamma[gx.fogc[1]];
                gx.fogc[2] = gx_gamma[gx.fogc[2]];
            }
            break;
        case GX_SETFILLCOLOR:
            gx.fillColor = w1;
            break;
        case GX_SETPRIMDEPTH:
            gx.primZ = (uint16_t)(w1 >> 16);
            gx.primDZ = (uint16_t)w1;
            break;
        case GX_SETSCISSOR:
            if (gx_cmd_lookup != NULL) {
                /* quadro intermediario: a area das cortinas do L1 (letterbox) com o tamanho intermediario */
                const uint32_t *r = gx_cmd_lookup((const void *)(uintptr_t)gx.curCmd);
                if (r != NULL) {
                    w0 = r[0];
                    w1 = r[1];
                }
            }
            gx.scRaw[0] = (w0 >> 12) & 0xFFF;
            gx.scRaw[1] = w0 & 0xFFF;
            gx.scRaw[2] = (w1 >> 12) & 0xFFF;
            gx.scRaw[3] = w1 & 0xFFF;
            gx_hr_scissor();
            break;
        case GX_SETTILE:
            cmd_settile(w0, w1);
            break;
        case GX_SETTILESIZE:
            if (gx_tile_lookup != NULL) {
                /* quadro intermediario: textura que rola, deslocamento intermediario */
                gx_tile_lookup((const void *)(uintptr_t)gx.curCmd, &w0, &w1);
            }
            cmd_settilesize(w0, w1);
            break;
        case GX_LOADBLOCK:
            cmd_loadblock(w0, w1);
            break;
        case GX_LOADTILE:
            cmd_loadtile(w0, w1);
            break;
        case GX_LOADTLUT:
            cmd_loadtlut(w0, w1);
            break;
        case GX_FILLRECT:
            if (gx_cmd_lookup != NULL) {
                /* quadro intermediario: cortinas do L1 (letterbox) com o tamanho intermediario */
                const uint32_t *r = gx_cmd_lookup((const void *)(uintptr_t)gx.curCmd);
                if (r != NULL) {
                    w0 = r[0];
                    w1 = r[1];
                }
            }
            gx_fillrect(w0, w1);
            break;
        case GX_SETKEYGB:
        case GX_SETKEYR:
        case GX_SETCONVERT:
        default:
            break;
    }
}
