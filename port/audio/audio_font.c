/*
 * audio_font.c -- converte uma soundfont recem-carregada da ROM (big-endian) para o
 * endian nativo, antes do jogo reloca-la (AudioLoad_RelocateFont, src/audio/internal/
 * load.c: o oot-patch.py poe a chamada no comeco dessa funcao).
 *
 * Uma soundfont e um bloco com estruturas ligadas por deslocamentos (a partir do
 * inicio do bloco):
 *   u32 [0] lista de baterias, [1] lista de efeitos, [2..] um por instrumento
 *   Drum        { u8 x3, pad; TunedSample { Sample* (u32), f32 tuning }; EnvelopePoint* }
 *   SoundEffect { TunedSample }
 *   Instrument  { u8 x4; EnvelopePoint*; TunedSample low, normal, high }
 *   Sample      { bits: codec 4, medium 2, unk 1, isRelocated 1, size 24; u8* sampleAddr;
 *                 AdpcmLoop*; AdpcmBook* }
 *   AdpcmLoop   { u32 start, end, count, unk; s16 predictorState[16] se count != 0 }
 *   AdpcmBook   { s32 order, numPredictors; s16 book[8 * order * numPredictors] }
 *   EnvelopePoint[] { s16 delay, s16 arg }: NAO sao convertidos. Envelopes tambem moram dentro
 *                 das sequencias (que nao tem estrutura para converter); entao todos ficam em
 *                 big-endian e o leitor (effects.c, ajustado pelo oot-patch.py) troca ao ler.
 * Uma mesma estrutura pode ser apontada por varias (uma amostra usada por varios
 * instrumentos): cada uma e convertida uma vez so (tabela de "ja convertidos").
 *
 * O campo de bits da Sample: no N64 o codec ocupa os 4 bits ALTOS da palavra; com o
 * GCC em little-endian o primeiro campo ocupa os bits BAIXOS. A palavra e remontada.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_DONE 8192

static uint8_t* g_base;
static uint32_t g_done[MAX_DONE]; /* deslocamento + 1 (0 = vaga) */
static int g_overflow;
/* diagnostico: amostras e livros com valores impossiveis (sinal de conversao errada) */
static int g_nSamples, g_badCodec, g_badBook, g_badLoop, g_firstOrder, g_firstPred;
static uint32_t g_firstSize, g_firstAddr;

static int mark(uint32_t off) { /* 1 = primeira vez */
    uint32_t h = (off * 2654435761u) % MAX_DONE;
    int n;
    for (n = 0; n < MAX_DONE; n++) {
        if (g_done[h] == off + 1) {
            return 0;
        }
        if (g_done[h] == 0) {
            g_done[h] = off + 1;
            return 1;
        }
        h = (h + 1) % MAX_DONE;
    }
    g_overflow = 1;
    return 0;
}

static uint32_t sw32(uint32_t off) { /* troca e devolve o valor nativo */
    uint8_t* p = g_base + off;
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    memcpy(p, &v, 4);
    return v;
}

static int16_t sw16(uint32_t off) {
    uint8_t* p = g_base + off;
    uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
    memcpy(p, &v, 2);
    return (int16_t)v;
}

static void conv_loop(uint32_t off) {
    int i;
    uint32_t count;
    if (off == 0 || !mark(off)) {
        return;
    }
    {
        uint32_t s = sw32(off), e = sw32(off + 4);
        if (s > e) {
            g_badLoop++;
        }
    }
    count = sw32(off + 8);
    sw32(off + 12);
    if (count != 0) {
        for (i = 0; i < 16; i++) {
            sw16(off + 16 + i * 2);
        }
    }
}

static void conv_book(uint32_t off) {
    int32_t order, npred, n, i;
    if (off == 0 || !mark(off)) {
        return;
    }
    order = (int32_t)sw32(off);
    npred = (int32_t)sw32(off + 4);
    if (order != 2 || npred < 1 || npred > 8) {
        g_badBook++;
    }
    if (g_firstOrder == 0) {
        g_firstOrder = order;
        g_firstPred = npred;
    }
    n = 8 * order * npred;
    if (n < 0 || n > 8 * 16 * 16) {
        n = 0;
    }
    for (i = 0; i < n; i++) {
        sw16(off + 8 + i * 2);
    }
}

static void conv_sample(uint32_t off) {
    uint32_t v, loop, book;
    if (off == 0 || !mark(off)) {
        return;
    }
    v = sw32(off);
    g_nSamples++;
    if ((v >> 28) > 3) {
        g_badCodec++;
    }
    if (g_firstSize == 0) {
        g_firstSize = v & 0xFFFFFF;
        g_firstAddr = ((uint32_t)g_base[off + 4] << 24) | ((uint32_t)g_base[off + 5] << 16) |
                      ((uint32_t)g_base[off + 6] << 8) | g_base[off + 7];
    }
    v = ((v >> 28) & 0xF) | (((v >> 26) & 3) << 4) | (((v >> 25) & 1) << 6) | (((v >> 24) & 1) << 7) |
        ((v & 0xFFFFFF) << 8);
    memcpy(g_base + off, &v, 4);
    sw32(off + 4);
    loop = sw32(off + 8);
    book = sw32(off + 12);
    conv_loop(loop);
    conv_book(book);
}

static void conv_tuned(uint32_t off, int follow) {
    uint32_t sample = sw32(off);
    sw32(off + 4); /* f32 */
    if (follow) {
        conv_sample(sample);
    }
}

void port_audio_font_native(void* fontData, int numDrums, int numInstruments, int numSfx) {
    uint32_t drums, sfx;
    int i;

    g_base = (uint8_t*)fontData;
    memset(g_done, 0, sizeof g_done);
    g_overflow = 0;
    g_nSamples = g_badCodec = g_badBook = g_badLoop = g_firstOrder = g_firstPred = 0;
    g_firstSize = g_firstAddr = 0;
    if (numInstruments > 126) {
        numInstruments = 126;
    }

    drums = sw32(0);
    sfx = sw32(4);
    for (i = 0; i < numInstruments; i++) {
        sw32(8 + i * 4);
    }

    if (drums != 0 && numDrums != 0 && mark(drums)) {
        for (i = 0; i < numDrums; i++) {
            uint32_t d = sw32(drums + i * 4);
            if (d != 0 && mark(d)) {
                conv_tuned(d + 4, 1);
                sw32(d + 12); /* envelope: os pontos ficam em big-endian (veja o topo) */
            }
        }
    }

    if (sfx != 0 && numSfx != 0) {
        for (i = 0; i < numSfx; i++) {
            uint32_t e = sfx + i * 8;
            if (mark(e)) {
                conv_tuned(e, 1);
            }
        }
    }

    for (i = 0; i < numInstruments; i++) {
        uint32_t o;
        memcpy(&o, g_base + 8 + i * 4, 4);
        if (o != 0 && mark(o)) {
            uint8_t lo = g_base[o + 1], hi = g_base[o + 2];
            sw32(o + 4); /* envelope */
            conv_tuned(o + 8, lo != 0);
            conv_tuned(o + 16, 1);
            conv_tuned(o + 24, hi != 0x7F);
        }
    }
    fprintf(stderr,
            "[som] fonte em %p: %d instrumentos, %d baterias, %d efeitos; %d amostras (codec invalido %d, "
            "livro invalido %d, loop invalido %d); 1a amostra: tamanho %u, endereco %08x, livro ordem %d preditores %d\n",
            fontData, numInstruments, numDrums, numSfx, g_nSamples, g_badCodec, g_badBook, g_badLoop, g_firstSize,
            g_firstAddr, g_firstOrder, g_firstPred);
}
