#include "swapmap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"

#define MAP_MAGIC "OOTSWP1"

static void seterr(char* err, size_t n, const char* fmt, ...) {
    va_list ap;
    if (err == NULL || n == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_calc(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

int swapmap_read_id(const char* path, uint32_t* id) {
    uint8_t h[16];
    FILE* f = fopen(path, "rb");
    int ok;
    if (f == NULL) {
        return 1;
    }
    ok = fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, MAP_MAGIC, 8) == 0;
    fclose(f);
    if (!ok) {
        return 1;
    }
    *id = rd32le(h + 12);
    return 0;
}

int swapmap_load(const char* path, uint8_t** raw, size_t* rawlen, uint32_t* id, char* err, size_t errsz) {
    FILE* f = fopen(path, "rb");
    uint8_t* buf = NULL;
    uint8_t* out = NULL;
    long n;
    size_t rl;
    uint32_t crc;
    int r;

    *raw = NULL;
    if (f == NULL) {
        seterr(err, errsz, "Nao achei o mapa de conversao (%s).", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 17) {
        fclose(f);
        seterr(err, errsz, "Mapa de conversao invalido (muito pequeno).");
        return 1;
    }
    buf = (uint8_t*)malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        seterr(err, errsz, "Falha ao ler o mapa de conversao.");
        return 1;
    }
    fclose(f);
    if (memcmp(buf, MAP_MAGIC, 8) != 0) {
        free(buf);
        seterr(err, errsz, "Mapa de conversao invalido (assinatura).");
        return 1;
    }
    rl = rd32le(buf + 8);
    crc = rd32le(buf + 12);
    if (rl < 4 || rl > (64u << 20)) {
        free(buf);
        seterr(err, errsz, "Mapa de conversao invalido (tamanho).");
        return 1;
    }
    out = (uint8_t*)malloc(rl);
    if (out == NULL) {
        free(buf);
        seterr(err, errsz, "Sem memoria.");
        return 1;
    }
    r = port_inflate(buf + 16, (size_t)n - 16, out, rl);
    free(buf);
    if (r != 0 || crc32_calc(out, rl) != crc) {
        free(out);
        seterr(err, errsz, "Mapa de conversao corrompido (codigo %d).", r);
        return 1;
    }
    *raw = out;
    *rawlen = rl;
    *id = crc;
    return 0;
}

int swapmap_apply(const uint8_t* raw, size_t rawlen, uint8_t* cart, size_t cartsz, const SwapFile* files,
                  size_t nfiles, char* err, size_t errsz) {
    uint32_t nf, k;
    size_t pos = 4, j;
    if (rawlen < 4) {
        seterr(err, errsz, "Mapa vazio.");
        return 1;
    }
    nf = rd32le(raw);
    for (k = 0; k < nf; k++) {
        uint32_t vrom, nbytes, nh, i;
        const uint8_t* packed;
        const SwapFile* sf = NULL;
        uint8_t* base;
        if (pos + 12 > rawlen) {
            seterr(err, errsz, "Mapa truncado.");
            return 1;
        }
        vrom = rd32le(raw + pos);
        nbytes = rd32le(raw + pos + 4);
        nh = rd32le(raw + pos + 8);
        pos += 12;
        if (nh > (nbytes + 1) / 2 || pos + (nh + 3) / 4 > rawlen) {
            seterr(err, errsz, "Mapa truncado (arquivo %u).", (unsigned)k);
            return 1;
        }
        packed = raw + pos;
        pos += (nh + 3) / 4;
        for (j = 0; j < nfiles; j++) {
            if (files[j].vrom_start == vrom) {
                sf = &files[j];
                break;
            }
        }
        if (sf == NULL || nbytes > sf->vrom_end - sf->vrom_start || (size_t)sf->cart_off + nbytes > cartsz) {
            seterr(err, errsz, "A ROM nao tem o arquivo %08X esperado pelo mapa. Versao errada da ROM?",
                   (unsigned)vrom);
            return 1;
        }
        base = cart + sf->cart_off;
        for (i = 0; i < nh; i++) {
            unsigned c = (packed[i >> 2] >> ((i & 3) * 2)) & 3;
            uint8_t* p = base + (size_t)i * 2;
            if (c == 0) {
                continue;
            }
            if (c == 1) {
                uint8_t t;
                if ((size_t)i * 2 + 2 > nbytes) {
                    continue; /* meia-palavra incompleta no fim do arquivo: fica como esta */
                }
                t = p[0];
                p[0] = p[1];
                p[1] = t;
            } else if (c == 2) {
                uint8_t t0 = p[0], t1 = p[1];
                if (i + 1 >= nh || ((packed[(i + 1) >> 2] >> (((i + 1) & 3) * 2)) & 3) != 3 ||
                    (size_t)(i + 1) * 2 + 2 > nbytes) {
                    seterr(err, errsz, "Mapa invalido (swap4 incompleto, arquivo %u).", (unsigned)k);
                    return 1;
                }
                p[0] = p[3];
                p[1] = p[2];
                p[2] = t1;
                p[3] = t0;
                i++;
            } else {
                seterr(err, errsz, "Mapa invalido (codigo 3 solto, arquivo %u).", (unsigned)k);
                return 1;
            }
        }
    }
    return 0;
}
