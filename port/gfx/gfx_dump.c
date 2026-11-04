/* gfx_dump.c -- grava um framebuffer RGBA5551 (bytes na ordem do N64) como imagem (diagnostico).
 * PNG simples (sem compressao) e PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_priv.h"

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    size_t i;
    if (!crc_ready) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++) {
                v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            }
            crc_table[k] = v;
        }
        crc_ready = 1;
    }
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    }
    return c;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8], t[4];
    uint32_t c;
    put32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) {
        fwrite(data, 1, n, f);
    }
    c = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    c = crc32_update(c, data, n);
    put32(t, ~c);
    fwrite(t, 1, 4, f);
}

static void rgb_of(const uint8_t *fb, int w, int x, int y, uint8_t *o) {
    const uint8_t *p = fb + ((size_t)y * w + x) * 2;
    unsigned c = ((unsigned)p[0] << 8) | p[1];
    int r = (int)((c >> 11) & 31), g = (int)((c >> 6) & 31), b = (int)((c >> 1) & 31);
    o[0] = (uint8_t)((r << 3) | (r >> 2));
    o[1] = (uint8_t)((g << 3) | (g >> 2));
    o[2] = (uint8_t)((b << 3) | (b >> 2));
}

int gx_dump_png(const uint8_t *fb, int w, int h, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t rawLen = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw, *z, *p;
    size_t zlen, pos = 0, off = 0, nblk;
    uint32_t a = 1, b = 0;
    int x, y;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13];
    if (f == NULL) {
        return 1;
    }
    raw = (uint8_t *)malloc(rawLen);
    nblk = (rawLen + 65534) / 65535;
    zlen = 2 + rawLen + nblk * 5 + 4;
    z = (uint8_t *)malloc(zlen);
    if (raw == NULL || z == NULL) {
        free(raw);
        free(z);
        fclose(f);
        return 1;
    }
    for (y = 0; y < h; y++) {
        raw[pos++] = 0;
        for (x = 0; x < w; x++) {
            rgb_of(fb, w, x, y, raw + pos);
            pos += 3;
        }
    }
    for (pos = 0; pos < rawLen; pos++) {
        a = (a + raw[pos]) % 65521u;
        b = (b + a) % 65521u;
    }
    p = z;
    *p++ = 0x78;
    *p++ = 0x01;
    while (off < rawLen) {
        size_t n = rawLen - off > 65535 ? 65535 : rawLen - off;
        *p++ = (off + n == rawLen) ? 1 : 0;
        *p++ = (uint8_t)n;
        *p++ = (uint8_t)(n >> 8);
        *p++ = (uint8_t)~n;
        *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        p += n;
        off += n;
    }
    put32(p, (b << 16) | a);
    p += 4;
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (size_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

int gx_dump_ppm(const uint8_t *fb, int w, int h, const char *path) {
    FILE *f = fopen(path, "wb");
    int x, y;
    if (f == NULL) {
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t o[3];
            rgb_of(fb, w, x, y, o);
            fwrite(o, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}
