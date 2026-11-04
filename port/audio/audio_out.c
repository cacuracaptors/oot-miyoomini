/*
 * audio_out.c -- liga o audio do jogo ao port:
 *   - a tarefa de audio do RSP passa a ser executada pelo HLE (audio_hle.c);
 *   - cada bloco que o jogo entrega ao AI (osAiSetNextBuffer) segue o caminho de
 *     sempre (o AI simulado de os_hw.c, que da o ritmo ao jogo) e tambem vai para a
 *     saida de som do aparelho e/ou para um arquivo WAV (teste no PC: --wav).
 */
#include <stdio.h>
#include <string.h>

#include "audio_hle.h"
#include "os_hw.h"

void audio_out_install(const char* wav_path, void (*sink)(const int16_t*, uint32_t, uint32_t));

static int (*orig_submit)(const void*, uint32_t);
static void (*orig_set_freq)(uint32_t);
static void (*g_sink)(const int16_t*, uint32_t, uint32_t);
static uint32_t g_freq = 32000;
static FILE* g_wav;
static uint32_t g_wav_bytes;

static int hook_audio_task(OSTask* t) {
    audio_hle_run((const uint32_t*)t->t.data_ptr, t->t.data_size);
    return 1;
}

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wav_header(void) {
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + g_wav_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1; /* PCM */
    h[21] = 0;
    h[22] = 2; /* estereo */
    h[23] = 0;
    put32(h + 24, g_freq);
    put32(h + 28, g_freq * 4);
    h[32] = 4;
    h[33] = 0;
    h[34] = 16;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, g_wav_bytes);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, 44, g_wav);
    fseek(g_wav, 0, SEEK_END);
}

static int hook_ai_submit(const void* pcm, uint32_t n) {
    int r = orig_submit(pcm, n);
    if (r == 0) {
        if (g_sink != NULL) {
            g_sink((const int16_t*)pcm, n / 4, g_freq);
        }
        if (g_wav != NULL) {
            fwrite(pcm, 1, n & ~3u, g_wav); /* (amostras nativas: little-endian no ARM e no PC) */
            g_wav_bytes += n & ~3u;
            wav_header();
            fflush(g_wav);
        }
    }
    return r;
}

static void hook_ai_set_freq(uint32_t hz) {
    g_freq = hz;
    orig_set_freq(hz);
    if (g_wav != NULL) {
        wav_header();
    }
}

void audio_out_install(const char* wav_path, void (*sink)(const int16_t*, uint32_t, uint32_t)) {
    port_hooks.audio_task = hook_audio_task;
    orig_submit = port_hooks.ai_submit;
    orig_set_freq = port_hooks.ai_set_freq;
    port_hooks.ai_submit = hook_ai_submit;
    port_hooks.ai_set_freq = hook_ai_set_freq;
    g_sink = sink;
    if (wav_path != NULL) {
        g_wav = fopen(wav_path, "wb");
        if (g_wav == NULL) {
            fprintf(stderr, "audio: nao consegui criar %s\n", wav_path);
        } else {
            wav_header();
            fprintf(stderr, "audio: gravando o som em %s\n", wav_path);
        }
    }
}
