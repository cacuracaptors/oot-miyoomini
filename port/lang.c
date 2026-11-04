/*
 * lang.c -- pacotes de idioma (opcoes.txt: idioma).
 *
 * Um pacote (idiomas/XX.oot, gerado por tools/oot-idioma.py) traz so o que uma traducao muda:
 *   - trechos de arquivos de dados do cartucho (fonte, texturas com texto, creditos), por
 *     indice do arquivo na dmadata; cada trecho tem o crc32 dos bytes da ROM 1.1 que ele
 *     substitui. Se o cartucho nao tiver exatamente esses bytes, o trecho e recusado.
 *   - a tabela de mensagens (id, tipo/posicao, deslocamento) e o bloco de texto inteiro.
 * Os trechos sao aplicados por cima de cada leitura da ROM (gancho rom_read), sem mexer no
 * oot.cart. O texto e servido da memoria (Port_LangMsg, chamado pelo z_message.c ajustado).
 * Texturas, fonte e textos ficam como bytes originais no cartucho (o mapa de conversao nao
 * mexe neles), por isso os bytes do pacote valem sem conversao.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os_hw.h"

int port_opt_idioma = 0; /* 0 = original (ingles), 1 = portugues do Brasil, 2 = japones, 3 = alemao, 4 = frances */

extern char _dmadataSegmentRomStart[];
extern char _dmadataSegmentRomEnd[];

typedef struct {
    uint32_t start, end; /* no cartucho */
    const uint8_t* data;
} LangRange;

/* mesmo formato do MessageTableEntry do jogo (include/message_data_static.h) */
typedef struct {
    uint16_t textId;
    uint8_t typePos;
    const char* segment;
} LangMsgEntry;

static uint8_t* g_pack;
static LangRange* g_ranges;
static int g_nranges;
static LangMsgEntry* g_msgTable;
static const uint8_t* g_msgBlob;
static uint32_t g_msgLen;
static int (*g_prevRead)(uint32_t, void*, uint32_t);
/* texturas extras do pacote (ia8, 16 de altura): ficam aqui, na memoria do programa, onde o desenho as acha */
#define LANG_XTEX_N 4
static uint8_t g_xtex[LANG_XTEX_N][2048] __attribute__((aligned(8)));
static uint16_t g_xtexW[LANG_XTEX_N];

/* z_kaleido_scope.c ajustado: a textura extra 'id' do idioma (e a largura), ou a do jogo */
void* Port_LangExtraTex(int id, void* def, unsigned short* width) {
    if (id < 0 || id >= LANG_XTEX_N || g_xtexW[id] == 0) {
        return def;
    }
    *width = g_xtexW[id];
    return g_xtex[id];
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t lang_crc32(const uint8_t* d, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu, i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

static int cmp_range(const void* a, const void* b) {
    const LangRange* x = (const LangRange*)a;
    const LangRange* y = (const LangRange*)b;
    return x->start < y->start ? -1 : (x->start > y->start ? 1 : 0);
}

static int lang_rom_read(uint32_t off, void* dst, uint32_t n) {
    int r = g_prevRead(off, dst, n);
    uint32_t end = off + n;
    int lo = 0, hi = g_nranges;
    /* primeiro trecho que termina depois de off */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_ranges[mid].end <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (; lo < g_nranges && g_ranges[lo].start < end; lo++) {
        const LangRange* g = &g_ranges[lo];
        uint32_t a = g->start > off ? g->start : off;
        uint32_t b = g->end < end ? g->end : end;
        memcpy((uint8_t*)dst + (a - off), g->data + (a - g->start), b - a);
    }
    return r;
}

/* texto da mensagem: 1 = servido pelo pacote, 0 = o jogo le da ROM como sempre */
int Port_LangMsg(void* dst, uint32_t off, uint32_t len) {
    if (g_msgBlob == NULL) {
        return 0;
    }
    if (off > g_msgLen) {
        off = g_msgLen;
    }
    if (len > g_msgLen - off) {
        memset((uint8_t*)dst + (g_msgLen - off), 0, len - (g_msgLen - off));
        len = g_msgLen - off;
    }
    memcpy(dst, g_msgBlob + off, len);
    return 1;
}

/* tabela de mensagens do idioma (NULL = a do jogo) */
void* Port_LangNesTable(void) {
    return g_msgTable;
}

static int fail(const char* path, const char* why) {
    fprintf(stderr, "[idioma] %s: %s; o jogo fica em ingles\n", path, why);
    free(g_pack);
    free(g_ranges);
    free(g_msgTable);
    g_pack = NULL;
    g_ranges = NULL;
    g_msgTable = NULL;
    g_msgBlob = NULL;
    g_nranges = 0;
    return -1;
}

static int lang_load(const char* path) {
    FILE* f = fopen(path, "rb");
    long size;
    const uint8_t *p, *e;
    uint32_t nfiles, i, nmsg, dmaOff = (uint32_t)(uintptr_t)_dmadataSegmentRomStart;
    uint32_t nEnt = (uint32_t)((uintptr_t)_dmadataSegmentRomEnd - (uintptr_t)_dmadataSegmentRomStart) / 16;
    int cap = 0, refused = 0, okFiles = 0;
    char name[33];

    if (f == NULL) {
        return fail(path, "arquivo nao encontrado");
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_pack = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
    if (g_pack == NULL || size < 300 || fread(g_pack, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return fail(path, "nao consegui ler");
    }
    fclose(f);
    p = g_pack;
    e = g_pack + size;
    if (memcmp(p, "OOTLNG1\0", 8) != 0 || rd32(p + 8) != 1) {
        return fail(path, "formato desconhecido");
    }
    memcpy(name, p + 12, 32);
    name[32] = 0;
    p += 12 + 32 + 224;
    nfiles = rd32(p);
    p += 4;
    for (i = 0; i < nfiles; i++) {
        uint32_t idx, osize, nr, k, vs, ve, rs;
        uint8_t ent[16];
        int bad = 0;
        if (p + 12 > e) {
            return fail(path, "pacote cortado");
        }
        idx = rd32(p);
        osize = rd32(p + 4);
        nr = rd32(p + 8);
        p += 12;
        /* onde o arquivo esta no cartucho (dmadata nativa) */
        if (idx >= nEnt || g_prevRead(dmaOff + idx * 16, ent, 16) != 0) {
            bad = 1;
            vs = ve = rs = 0;
        } else {
            vs = rd32(ent);
            ve = rd32(ent + 4);
            rs = rd32(ent + 8);
            if (ve - vs != osize || rs == 0xFFFFFFFFu) {
                bad = 1;
            }
        }
        for (k = 0; k < nr; k++) {
            uint32_t off, len, crc;
            uint8_t* cur;
            if (p + 12 > e) {
                return fail(path, "pacote cortado");
            }
            off = rd32(p);
            len = rd32(p + 4);
            crc = rd32(p + 8);
            p += 12;
            if (len == 0 || p + len > e || (uint64_t)off + len > osize) {
                return fail(path, "trecho invalido");
            }
            if (!bad) {
                /* confere se o cartucho tem os bytes que a traducao substitui */
                cur = (uint8_t*)malloc(len);
                if (cur == NULL || g_prevRead(rs + off, cur, len) != 0 ||
                    lang_crc32(cur, len) != crc) {
                    refused++;
                } else {
                    if (g_nranges == cap) {
                        cap = cap ? cap * 2 : 256;
                        g_ranges = (LangRange*)realloc(g_ranges, (size_t)cap * sizeof(LangRange));
                        if (g_ranges == NULL) {
                            free(cur);
                            return fail(path, "sem memoria");
                        }
                    }
                    g_ranges[g_nranges].start = rs + off;
                    g_ranges[g_nranges].end = rs + off + len;
                    g_ranges[g_nranges].data = p;
                    g_nranges++;
                }
                free(cur);
            } else {
                refused++;
            }
            p += (len + 3) & ~3u;
        }
        okFiles += !bad;
    }
    /* mensagens */
    if (p + 4 > e) {
        return fail(path, "pacote cortado");
    }
    nmsg = rd32(p);
    p += 4;
    if (nmsg == 0 || p + (size_t)nmsg * 8 + 4 > e) {
        return fail(path, "tabela de mensagens invalida");
    }
    {
        const uint8_t* tab = p;
        p += (size_t)nmsg * 8;
        g_msgLen = rd32(p);
        p += 4;
        if (p + g_msgLen > e) {
            return fail(path, "texto cortado");
        }
        g_msgBlob = p;
        g_msgTable = (LangMsgEntry*)calloc(nmsg + 1, sizeof(LangMsgEntry));
        if (g_msgTable == NULL) {
            return fail(path, "sem memoria");
        }
        for (i = 0; i < nmsg; i++) {
            uint32_t off = rd32(tab + i * 8 + 4);
            if (off > g_msgLen) {
                return fail(path, "mensagem fora do texto");
            }
            g_msgTable[i].textId = (uint16_t)(tab[i * 8] | (tab[i * 8 + 1] << 8));
            g_msgTable[i].typePos = tab[i * 8 + 2];
            g_msgTable[i].segment = (const char*)g_msgBlob + off;
        }
        g_msgTable[nmsg].textId = 0xFFFF;
        g_msgTable[nmsg].segment = (const char*)g_msgBlob + g_msgLen;
        p += g_msgLen;
    }
    /* secao opcional: texturas extras (rotulos mais largos que o ingles; lang_pal.c) */
    if (p + 8 <= e && !memcmp(p, "XTEX", 4)) {
        uint32_t nx = rd32(p + 4);
        p += 8;
        for (i = 0; i < nx && p + 12 <= e; i++) {
            uint32_t id = rd32(p), w = p[4] | (p[5] << 8), len = rd32(p + 8);
            p += 12;
            if (p + len > e) {
                break;
            }
            if (id < LANG_XTEX_N && len <= sizeof g_xtex[0]) {
                memcpy(g_xtex[id], p, len);
                g_xtexW[id] = (uint16_t)w;
            }
            p += (len + 3) & ~3u;
        }
    }
    qsort(g_ranges, (size_t)g_nranges, sizeof(LangRange), cmp_range);
    port_hooks.rom_read = lang_rom_read;
    fprintf(stderr, "[idioma] %s: %s; %d arquivos, %d trechos aplicados, %d recusados, %u mensagens\n", path, name,
            okFiles, g_nranges, refused, nmsg);
    return 0;
}

/* Chamar depois de abrir o cartucho e de ler o opcoes.txt. */
int port_lang_game; /* o idioma desta execucao (0 ingles, 1 portugues, 2 japones, 3 alemao, 4 frances): z_common_data.c,
                     * graph.c (so o japones muda gSaveContext.language; os outros usam as posicoes do ingles) */

void port_lang_init(void) {
    static const char* const k_files[] = { NULL, "idiomas/pt.oot", NULL, "idiomas/de.oot", "idiomas/fr.oot" };
    port_lang_game = port_opt_idioma >= 0 && port_opt_idioma <= 4 ? port_opt_idioma : 0;
    if (port_opt_idioma <= 0 || port_opt_idioma >= (int)(sizeof k_files / sizeof k_files[0]) ||
        k_files[port_opt_idioma] == NULL) {
        return;
    }
    g_prevRead = port_hooks.rom_read;
    lang_load(k_files[port_opt_idioma]);
}
