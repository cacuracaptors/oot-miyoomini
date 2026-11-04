/*
 * lang_pal.c -- alemao e frances a partir da ROM europeia.
 *
 * Se o jogador puser na pasta do jogo a ROM europeia "Legend of Zelda, The - Ocarina of Time (Europe) (En,Fr,De)
 * (Rev A)" (PAL 1.1; .z64/.n64/.v64 ou o proprio .zip), na proxima vez que o jogo abrir ele monta idiomas/de.oot e
 * idiomas/fr.oot, pacotes no mesmo formato do portugues (lang.c), e a opcao language ganha Deutsch (3) e Francais (4).
 * Depois disso a ROM europeia pode sair da pasta.
 *
 * O que entra em cada pacote (tabela em lang_pal.h, gerada por tools/oot-idioma-pal.py a partir do decomp):
 *   - texturas com texto: a alema/francesa por cima da inglesa do cartucho, quando tem a mesma medida;
 *   - a fonte da ROM europeia;
 *   - as falas: tabela de mensagens do codigo europeu (achada pelo padrao da 1a entrada) e o bloco de texto;
 *   - 3 rotulos do menu de pausa mais largos que os ingleses (secao XTEX; z_kaleido_scope.c pede a lang.c).
 * Cada trecho leva o crc32 dos bytes do cartucho que ele substitui (lang.c confere ao carregar).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cart_build.h"
#include "inflate.h"
#include "lang_pal.h"

extern char _dmadataSegmentRomStart[];
extern char _dmadataSegmentRomEnd[];

#define PAL_ROM_MIN (8u << 20)
#define PAL_ROM_MAX (64u << 20)
#define CHECKED_FILE "idiomas/rom_europeia.txt"
/* versao da tabela (lang_pal.h): mudou = os pacotes sao refeitos (v2: faltava "To Equipment" no menu de pausa) */
#define LANG_PAL_TAG "[tabela v2]"

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t le16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t crc32_buf(const uint8_t* d, uint32_t n) {
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

/* v64 (pares trocados) e n64 (palavras invertidas) -> z64 */
static void normalize(uint8_t* b, size_t n) {
    size_t i;
    if (n >= 4 && b[0] == 0x37 && b[1] == 0x80) {
        for (i = 0; i + 1 < n; i += 2) {
            uint8_t t = b[i];
            b[i] = b[i + 1];
            b[i + 1] = t;
        }
    } else if (n >= 4 && b[0] == 0x40 && b[1] == 0x12) {
        for (i = 0; i + 3 < n; i += 4) {
            uint8_t t0 = b[i], t1 = b[i + 1];
            b[i] = b[i + 3];
            b[i + 1] = b[i + 2];
            b[i + 2] = t1;
            b[i + 3] = t0;
        }
    }
}

/* cabecalho (64 bytes, ja em z64): 1 = Ocarina of Time europeu, revisao A (PAL 1.1) */
static int is_pal11(const uint8_t* h) {
    return h[0] == 0x80 && h[1] == 0x37 && (h[0x3B] == 'N' || h[0x3B] == 'C') && h[0x3C] == 'Z' && h[0x3D] == 'L' &&
           h[0x3E] == 'P' && h[0x3F] == 1;
}

/* 1 = o arquivo e uma ROM do Ocarina americano (para o port_main.c nao confundir com a europeia) */
int port_rom_is_usa(const char* path) {
    uint8_t h[64];
    FILE* f = fopen(path, "rb");
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = fread(h, 1, sizeof h, f) == sizeof h;
    fclose(f);
    if (!ok) {
        return 0;
    }
    normalize(h, sizeof h);
    return h[0x3C] == 'Z' && h[0x3D] == 'L' && h[0x3E] == 'E';
}

static uint8_t* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    long sz;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (unsigned long)sz > PAL_ROM_MAX) {
        fclose(f);
        return NULL;
    }
    b = (uint8_t*)malloc((size_t)sz);
    if (b == NULL || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* a ROM de dentro de um .zip (o primeiro .z64/.n64/.v64; guardado ou "deflate"); NULL se nao der */
static uint8_t* rom_from_zip(const char* path, size_t* romSize) {
    size_t zn = 0, i, cd, ncd, k;
    uint8_t* z = read_all(path, &zn);
    uint8_t* out = NULL;
    if (z == NULL || zn < 22) {
        free(z);
        return NULL;
    }
    /* fim do diretorio central */
    for (i = zn - 22;; i--) {
        if (le32(z + i) == 0x06054B50u) {
            break;
        }
        if (i == 0 || zn - i > 0x10000 + 22) {
            free(z);
            return NULL;
        }
    }
    ncd = le16(z + i + 10);
    cd = le32(z + i + 16);
    for (k = 0; k < ncd && cd + 46 <= zn; k++) {
        const uint8_t* c = z + cd;
        uint32_t method = le16(c + 10), csize = le32(c + 20), usize = le32(c + 24), lho = le32(c + 42);
        uint32_t nl = le16(c + 28), el = le16(c + 30), cl = le16(c + 32);
        char name[256];
        if (le32(c) != 0x02014B50u || cd + 46 + nl > zn) {
            break;
        }
        snprintf(name, sizeof name, "%.*s", (int)(nl < 255 ? nl : 255), (const char*)c + 46);
        cd += 46 + nl + el + cl;
        if (!(has_ext(name, ".z64") || has_ext(name, ".n64") || has_ext(name, ".v64")) || usize < PAL_ROM_MIN ||
            usize > PAL_ROM_MAX || (size_t)lho + 30 > zn || le32(z + lho) != 0x04034B50u) {
            continue;
        }
        {
            const size_t data = (size_t)lho + 30 + le16(z + lho + 26) + le16(z + lho + 28);
            if (data + csize > zn) {
                continue;
            }
            out = (uint8_t*)malloc(usize);
            if (out == NULL) {
                break;
            }
            if ((method == 0 && csize == usize && memcpy(out, z + data, usize) != NULL) ||
                (method == 8 && port_inflate(z + data, csize, out, usize) == 0)) {
                *romSize = usize;
                break;
            }
            free(out);
            out = NULL;
        }
    }
    free(z);
    return out;
}

/* a ROM americana de dentro de um .zip, gravada em 'out' (para o cart_build); 0 = ok */
int port_rom_zip_extract_usa(const char* zip, const char* out) {
    size_t n = 0;
    uint8_t* rom = rom_from_zip(zip, &n);
    FILE* f;
    int ok;
    if (rom == NULL) {
        return -1;
    }
    {
        uint8_t h[64];
        memcpy(h, rom, sizeof h);
        normalize(h, sizeof h);
        if (!(h[0x3C] == 'Z' && h[0x3D] == 'L' && h[0x3E] == 'E')) {
            free(rom);
            return -1;
        }
    }
    f = fopen(out, "wb");
    ok = f != NULL && fwrite(rom, 1, n, f) == n;
    if (f != NULL) {
        ok = (fclose(f) == 0) && ok;
    }
    free(rom);
    if (!ok) {
        unlink(out);
    }
    return ok ? 0 : -1;
}

/* ---- arquivos ja olhados que nao eram a ROM europeia (nao abrir o .zip de novo a cada vez) ---- */
static int checked_before(const char* name, const struct stat* st) {
    char line[600], want[600];
    FILE* f = fopen(CHECKED_FILE, "r");
    int found = 0;
    if (f == NULL) {
        return 0;
    }
    snprintf(want, sizeof want, "%lld %lld %s\n", (long long)st->st_size, (long long)st->st_mtime, name);
    while (!found && fgets(line, sizeof line, f) != NULL) {
        found = !strcmp(line, want);
    }
    fclose(f);
    return found;
}

static void mark_checked(const char* name, const struct stat* st) {
    FILE* f = fopen(CHECKED_FILE, "a");
    if (f != NULL) {
        fprintf(f, "%lld %lld %s\n", (long long)st->st_size, (long long)st->st_mtime, name);
        fclose(f);
    }
}

/* procura a ROM europeia na pasta do jogo; devolve a ROM (z64) ou NULL */
static uint8_t* find_pal_rom(size_t* romSize, char* found, size_t foundsz, CartProgress cb, void* ud) {
    DIR* d = opendir(".");
    struct dirent* e;
    uint8_t* rom = NULL;
    if (d == NULL) {
        return NULL;
    }
    while (rom == NULL && (e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        struct stat st;
        if (stat(n, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1024) {
            continue;
        }
        if (has_ext(n, ".z64") || has_ext(n, ".n64") || has_ext(n, ".v64")) {
            uint8_t h[64];
            FILE* f = fopen(n, "rb");
            int ok = f != NULL && fread(h, 1, sizeof h, f) == sizeof h;
            if (f != NULL) {
                fclose(f);
            }
            if (!ok) {
                continue;
            }
            normalize(h, sizeof h);
            if (!is_pal11(h)) {
                continue;
            }
            if (cb != NULL) {
                cb(ud, "ROM", 5);
            }
            rom = read_all(n, romSize);
            if (rom != NULL) {
                normalize(rom, *romSize);
            }
        } else if (has_ext(n, ".zip") && !checked_before(n, &st)) {
            if (cb != NULL) {
                cb(ud, "ZIP", 5);
            }
            rom = rom_from_zip(n, romSize);
            if (rom != NULL) {
                normalize(rom, *romSize);
                if (*romSize < 0x8000 || !is_pal11(rom)) {
                    free(rom);
                    rom = NULL;
                }
            }
            if (rom == NULL) {
                mark_checked(n, &st);
            }
        }
        if (rom != NULL) {
            snprintf(found, foundsz, "%s", n);
        }
    }
    closedir(d);
    return rom;
}

/* ---- arquivos da ROM europeia (descomprimidos, guardados enquanto monta os pacotes) ---- */
typedef struct {
    const uint8_t* rom;
    size_t romSize;
    uint8_t* file[2048];
    uint32_t len[2048];
} Pal;

static const uint8_t* pal_file(Pal* p, unsigned idx, uint32_t* len) {
    const uint8_t* e;
    uint32_t vs, ve, ps, pe;
    if (idx >= 2048 || LANG_PAL_DMADATA + (idx + 1) * 16 > p->romSize) {
        return NULL;
    }
    if (p->file[idx] != NULL) {
        *len = p->len[idx];
        return p->file[idx];
    }
    e = p->rom + LANG_PAL_DMADATA + idx * 16;
    vs = be32(e);
    ve = be32(e + 4);
    ps = be32(e + 8);
    pe = be32(e + 12);
    if (ve <= vs || ve - vs > (16u << 20) || ps == 0xFFFFFFFFu) {
        return NULL;
    }
    p->file[idx] = (uint8_t*)malloc(ve - vs);
    if (p->file[idx] == NULL) {
        return NULL;
    }
    if (pe == 0) {
        if ((size_t)ps + (ve - vs) > p->romSize) {
            free(p->file[idx]);
            p->file[idx] = NULL;
            return NULL;
        }
        memcpy(p->file[idx], p->rom + ps, ve - vs);
    } else if (pe <= ps || pe > p->romSize || yaz0_decode(p->rom + ps, pe - ps, p->file[idx], ve - vs) != 0) {
        free(p->file[idx]);
        p->file[idx] = NULL;
        return NULL;
    }
    p->len[idx] = ve - vs;
    *len = ve - vs;
    return p->file[idx];
}

/* ---- pacote em memoria ---- */
typedef struct {
    uint8_t* b;
    size_t n, cap;
    int bad;
} Buf;

static void put(Buf* o, const void* d, size_t n) {
    if (o->bad) {
        return;
    }
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : (1u << 20);
        uint8_t* nb;
        while (c < o->n + n) {
            c *= 2;
        }
        nb = (uint8_t*)realloc(o->b, c);
        if (nb == NULL) {
            o->bad = 1;
            return;
        }
        o->b = nb;
        o->cap = c;
    }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}
static void put32(Buf* o, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(o, b, 4);
}
static void put16(Buf* o, uint32_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(o, b, 2);
}
static void pad4(Buf* o) {
    static const uint8_t z[4] = { 0 };
    put(o, z, (4 - (o->n & 3)) & 3);
}

/* entrada da dmadata do cartucho (nativa): inicio e tamanho do arquivo idx */
static int cart_file(FILE* cart, unsigned idx, uint32_t* start, uint32_t* size) {
    uint8_t e[16];
    const uint32_t dmaOff = (uint32_t)(uintptr_t)_dmadataSegmentRomStart;
    const uint32_t nEnt = (uint32_t)((uintptr_t)_dmadataSegmentRomEnd - (uintptr_t)_dmadataSegmentRomStart) / 16;
    if (idx >= nEnt || fseek(cart, (long)(dmaOff + idx * 16), SEEK_SET) != 0 || fread(e, 1, 16, cart) != 16) {
        return -1;
    }
    if (le32(e + 8) == 0xFFFFFFFFu || le32(e + 4) <= le32(e)) {
        return -1;
    }
    *start = le32(e + 8);
    *size = le32(e + 4) - le32(e);
    return 0;
}

static int cmp_copy(const void* a, const void* b) {
    const LangPalCopy* x = *(const LangPalCopy* const*)a;
    const LangPalCopy* y = *(const LangPalCopy* const*)b;
    if (x->nf != y->nf) {
        return x->nf < y->nf ? -1 : 1;
    }
    return x->noff < y->noff ? -1 : (x->noff > y->noff ? 1 : 0);
}

/* monta o pacote do idioma L (0 alemao, 1 frances); 0 = ok */
static int build_pack(Pal* p, FILE* cart, int L, const char* outPath) {
    enum { NCOPY = sizeof k_langPalCopy / sizeof k_langPalCopy[0] };
    const LangPalCopy* sorted[NCOPY];
    Buf o = { 0 };
    uint32_t i, nfiles = 0, codeLen = 0, blobLen = 0;
    const uint8_t *code, *blob;
    uint8_t* cur = NULL;
    char tmp[256];
    FILE* f;
    static const uint8_t first[8] = { 0x00, 0x01, 0x23, 0x00, 0x07, 0x00, 0x00, 0x00 };
    size_t ti, nn = 0;

    for (i = 0; i < NCOPY; i++) {
        sorted[i] = &k_langPalCopy[i];
    }
    qsort(sorted, NCOPY, sizeof sorted[0], cmp_copy);
    put(&o, "OOTLNG1\0", 8);
    put32(&o, 1);
    {
        char name[32] = { 0 }, cred[224] = { 0 };
        snprintf(name, sizeof name, "%s", L == 0 ? "Deutsch" : "Francais");
        snprintf(cred, sizeof cred, "The Legend of Zelda: Ocarina of Time (Europe) (Rev A), %s " LANG_PAL_TAG,
                 L == 0 ? "deutsche Fassung" : "version francaise");
        put(&o, name, 32);
        put(&o, cred, 224);
    }
    /* arquivos (quantos tem pelo menos 1 trecho neste idioma) */
    for (i = 0; i < NCOPY;) {
        uint32_t j = i, any = 0;
        while (j < NCOPY && sorted[j]->nf == sorted[i]->nf) {
            any |= sorted[j]->poff[L] >= 0;
            j++;
        }
        nfiles += any;
        i = j;
    }
    put32(&o, nfiles);
    for (i = 0; i < NCOPY;) {
        uint32_t j = i, nr = 0, cstart, csize;
        while (j < NCOPY && sorted[j]->nf == sorted[i]->nf) {
            nr += sorted[j]->poff[L] >= 0;
            j++;
        }
        if (nr == 0) {
            i = j;
            continue;
        }
        if (cart_file(cart, sorted[i]->nf, &cstart, &csize) != 0) {
            free(o.b);
            return -1;
        }
        put32(&o, sorted[i]->nf);
        put32(&o, csize);
        put32(&o, nr);
        for (; i < j; i++) {
            const LangPalCopy* c = sorted[i];
            uint32_t plen = 0;
            const uint8_t* pf;
            if (c->poff[L] < 0) {
                continue;
            }
            pf = pal_file(p, c->pf, &plen);
            if (pf == NULL || (uint32_t)c->poff[L] + c->len > plen || c->noff + c->len > csize) {
                free(o.b);
                return -2;
            }
            cur = (uint8_t*)realloc(cur, c->len);
            if (cur == NULL || fseek(cart, (long)(cstart + c->noff), SEEK_SET) != 0 || fread(cur, 1, c->len, cart) != c->len) {
                free(cur);
                free(o.b);
                return -3;
            }
            put32(&o, c->noff);
            put32(&o, c->len);
            put32(&o, crc32_buf(cur, c->len));
            put(&o, pf + c->poff[L], c->len);
            pad4(&o);
        }
    }
    free(cur);

    /* falas: tabela inglesa do codigo europeu (id, tipo/posicao, endereco) e, logo depois, as do alemao e do
     * frances (so os enderecos, na mesma ordem, terminadas em 0) */
    code = pal_file(p, LANG_PAL_F_CODE, &codeLen);
    blob = pal_file(p, L == 0 ? LANG_PAL_F_GER_MESSAGE_DATA_STATIC : LANG_PAL_F_FRA_MESSAGE_DATA_STATIC, &blobLen);
    if (code == NULL || blob == NULL) {
        free(o.b);
        return -4;
    }
    for (ti = 0; ti + 8 <= codeLen; ti += 4) {
        if (!memcmp(code + ti, first, 8)) {
            break;
        }
    }
    if (ti + 8 > codeLen) {
        free(o.b);
        return -5;
    }
    while (ti + (nn + 1) * 8 <= codeLen && be32(code + ti + nn * 8) >> 16 != 0xFFFF) {
        nn++;
    }
    {
        const size_t lang0 = ti + (nn + 1) * 8, lang1 = lang0 + nn * 4, tab = L == 0 ? lang0 : lang1;
        uint32_t prev = 0;
        if (nn < 100 || lang1 + nn * 4 > codeLen || be32(code + lang0 + (nn - 1) * 4) != 0 ||
            be32(code + lang1 + (nn - 1) * 4) != 0 || be32(code + lang0) != 0x07000000u || be32(code + lang1) != 0x07000000u) {
            free(o.b);
            return -6;
        }
        put32(&o, (uint32_t)nn);
        for (i = 0; i < nn; i++) {
            const uint8_t* ne = code + ti + i * 8;
            uint32_t off = i + 1 < nn ? be32(code + tab + i * 4) - 0x07000000u : blobLen; /* (a ultima: 0xFFFD, o fim) */
            if (off > blobLen || off < prev) {
                free(o.b);
                return -7;
            }
            prev = off;
            put16(&o, ((uint32_t)ne[0] << 8) | ne[1]);
            put(&o, ne + 2, 1);
            put(&o, "\0", 1);
            put32(&o, off);
        }
        put32(&o, blobLen);
        put(&o, blob, blobLen);
    }
    /* rotulos do menu de pausa (secao extra, lang.c) */
    put(&o, "XTEX", 4);
    put32(&o, (uint32_t)(sizeof k_langPalExtra / sizeof k_langPalExtra[0]));
    for (i = 0; i < sizeof k_langPalExtra / sizeof k_langPalExtra[0]; i++) {
        const LangPalExtra* x = &k_langPalExtra[i];
        uint32_t plen = 0, len = (uint32_t)x->w[L] * 16u;
        const uint8_t* pf = pal_file(p, x->pf[L], &plen);
        if (pf == NULL || x->poff[L] + len > plen) {
            free(o.b);
            return -8;
        }
        put32(&o, i);
        put16(&o, x->w[L]);
        put16(&o, 16);
        put32(&o, len);
        put(&o, pf + x->poff[L], len);
        pad4(&o);
    }
    if (o.bad) {
        free(o.b);
        return -9;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", outPath);
    f = fopen(tmp, "wb");
    if (f == NULL || fwrite(o.b, 1, o.n, f) != o.n) {
        if (f != NULL) {
            fclose(f);
        }
        unlink(tmp);
        free(o.b);
        return -10;
    }
    fclose(f);
    free(o.b);
    return rename(tmp, outPath) == 0 ? 0 : -11;
}

/* 1 = o pacote existe e foi feito por esta versao da tabela (LANG_PAL_TAG no texto dos creditos); um pacote
 * antigo e refeito quando a ROM europeia ainda esta na pasta (sem ela, o antigo continua valendo) */
static int pack_current(const char* path) {
    char h[12 + 32 + 224 + 1];
    FILE* f = fopen(path, "rb");
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = fread(h, 1, sizeof h - 1, f) == sizeof h - 1;
    fclose(f);
    h[sizeof h - 1] = 0;
    return ok && memmem(h + 44, 224, LANG_PAL_TAG, strlen(LANG_PAL_TAG)) != NULL;
}

/* Chamar depois do cartucho pronto e antes de port_lang_init. 1 = montou os pacotes agora, 0 = nada a fazer,
 * -1 = achou a ROM europeia mas falhou (motivo no log). */
int port_lang_pal_prepare(const char* cartPath, CartProgress cb, void* ud) {
    Pal* p;
    FILE* cart;
    size_t romSize = 0;
    uint8_t* rom;
    char found[256] = "";
    int L, rc = 1;
    if (pack_current("idiomas/de.oot") && pack_current("idiomas/fr.oot")) {
        return 0;
    }
    mkdir("idiomas", 0755);
    rom = find_pal_rom(&romSize, found, sizeof found, cb, ud);
    if (rom == NULL) {
        return 0;
    }
    fprintf(stderr, "[idioma] ROM europeia achada (%s): montando o alemao e o frances\n", found);
    cart = fopen(cartPath, "rb");
    p = (Pal*)calloc(1, sizeof *p);
    if (cart == NULL || p == NULL) {
        if (cart != NULL) {
            fclose(cart);
        }
        free(p);
        free(rom);
        return -1;
    }
    p->rom = rom;
    p->romSize = romSize;
    for (L = 0; L < 2; L++) {
        const char* out = L == 0 ? "idiomas/de.oot" : "idiomas/fr.oot";
        int r;
        if (cb != NULL) {
            cb(ud, L == 0 ? "Deutsch" : "Francais", 30 + 35 * L);
        }
        r = build_pack(p, cart, L, out);
        if (r != 0) {
            fprintf(stderr, "[idioma] %s: falhou (%d)\n", out, r);
            rc = -1;
        } else {
            fprintf(stderr, "[idioma] %s pronto\n", out);
        }
    }
    if (cb != NULL) {
        cb(ud, "OK", 100);
    }
    fclose(cart);
    for (L = 0; L < 2048; L++) {
        free(p->file[L]);
    }
    free(p);
    free(rom);
    return rc;
}
