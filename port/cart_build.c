#define _GNU_SOURCE
#include "cart_build.h"
#include "swapmap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Posicao da dmadata na ROM (vem do ligador: oot-syms.ld, versao ntsc-1.1). */
extern char _dmadataSegmentRomStart[];
extern char _dmadataSegmentRomEnd[];

/* mensagens na lingua do options.txt (port/lang.c; fraco: o teste no PC nao tem): ingles, ou portugues com language=1 */
extern int port_opt_idioma __attribute__((weak));
#define TR(en, pt) ((&port_opt_idioma != NULL && port_opt_idioma == 1) ? (pt) : (en))

static void seterr(char* err, size_t n, const char* fmt, ...) {
    va_list ap;
    if (err == NULL || n == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int yaz0_decode(const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstlen) {
    size_t si = 16, di = 0;
    if (srclen < 16 || memcmp(src, "Yaz0", 4) != 0) {
        return -1;
    }
    if (rd32be(src + 4) != dstlen) {
        return -2;
    }
    while (di < dstlen) {
        unsigned code, bit;
        if (si >= srclen) {
            return -3;
        }
        code = src[si++];
        for (bit = 0x80; bit != 0 && di < dstlen; bit >>= 1) {
            if (code & bit) {
                if (si >= srclen) {
                    return -3;
                }
                dst[di++] = src[si++];
            } else {
                unsigned b1, b2, n;
                size_t dist;
                if (si + 1 >= srclen) {
                    return -3;
                }
                b1 = src[si++];
                b2 = src[si++];
                dist = (((b1 & 0xF) << 8) | b2) + 1;
                n = b1 >> 4;
                if (n == 0) {
                    if (si >= srclen) {
                        return -3;
                    }
                    n = src[si++] + 0x12;
                } else {
                    n += 2;
                }
                if (dist > di) {
                    return -4;
                }
                while (n-- != 0 && di < dstlen) {
                    dst[di] = dst[di - dist];
                    di++;
                }
            }
        }
    }
    return 0;
}

/* Le o arquivo todo e normaliza para z64 (big-endian). */
static uint8_t* load_rom(const char* path, size_t* size, char* err, size_t errsz) {
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    long n;
    size_t i;
    if (f == NULL) {
        seterr(err, errsz, TR("Could not open the ROM: %s", "Não consegui abrir a ROM: %s"), path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x100000 || n > 0x4000000) {
        fclose(f);
        seterr(err, errsz, TR("Unexpected ROM size (%ld bytes). The USA Rev A ROM (32 MB) is needed.", "Tamanho de ROM inesperado (%ld bytes). Preciso da ROM USA Rev A (32 MB)."), n);
        return NULL;
    }
    b = (uint8_t*)malloc((size_t)n);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        seterr(err, errsz, TR("Could not read the ROM (memory or disk).", "Falha ao ler a ROM (memória ou disco)."));
        return NULL;
    }
    fclose(f);
    if (b[0] == 0x37 && b[1] == 0x80) { /* v64: pares de bytes trocados */
        for (i = 0; i + 1 < (size_t)n; i += 2) {
            uint8_t t = b[i];
            b[i] = b[i + 1];
            b[i + 1] = t;
        }
    } else if (b[0] == 0x40 && b[1] == 0x12) { /* n64: palavras invertidas */
        for (i = 0; i + 3 < (size_t)n; i += 4) {
            uint8_t t0 = b[i], t1 = b[i + 1];
            b[i] = b[i + 3];
            b[i + 1] = b[i + 2];
            b[i + 2] = t1;
            b[i + 3] = t0;
        }
    } else if (!(b[0] == 0x80 && b[1] == 0x37)) {
        free(b);
        seterr(err, errsz, TR("This file does not look like a Nintendo 64 ROM.", "Este arquivo não parece uma ROM de Nintendo 64."));
        return NULL;
    }
    *size = (size_t)n;
    return b;
}

static void progress(CartProgress cb, void* ud, const char* stage, int pct) {
    if (cb != NULL) {
        cb(ud, stage, pct);
    }
}

int cart_build(const char* rom_path, const char* out_path, const char* swapmap_path, CartProgress cb, void* ud,
               char* err, size_t errsz) {
    size_t romSize = 0, total, dmaOff, dmaLen, nEnt, i, newpos, nsf = 0;
    uint8_t* mapRaw = NULL;
    size_t mapLen = 0;
    uint32_t mapId = 0;
    SwapFile* sf = NULL;
    uint8_t* rom;
    uint8_t* out = NULL;
    uint32_t* newRomStart = NULL;
    char tmp[1024];
    int fd = -1, rc = 1;

    progress(cb, ud, TR("Reading the conversion map", "Lendo o mapa de conversão"), 0);
    if (swapmap_load(swapmap_path, &mapRaw, &mapLen, &mapId, err, errsz) != 0) {
        return 1;
    }
    progress(cb, ud, TR("Reading the ROM", "Lendo a ROM"), 1);
    rom = load_rom(rom_path, &romSize, err, errsz);
    if (rom == NULL) {
        free(mapRaw);
        return 1;
    }
    if (!(rom[0x3B] == 'N' || rom[0x3B] == 'C') || rom[0x3C] != 'Z' || rom[0x3D] != 'L' || rom[0x3E] != 'E') {
        seterr(err, errsz, TR("This ROM is not Ocarina of Time (USA). Code found: %c%c%c%c", "Esta ROM não é o Ocarina of Time (USA). Código encontrado: %c%c%c%c"), rom[0x3B], rom[0x3C],
               rom[0x3D], rom[0x3E]);
        goto done;
    }
    if (rom[0x3F] != 1) {
        seterr(err, errsz, TR("This ROM is version %d. The USA Rev A (v1.1) is needed.", "Esta ROM é a versão %d. Preciso da USA Rev A (v1.1)."), rom[0x3F]);
        goto done;
    }

    dmaOff = (size_t)(uintptr_t)_dmadataSegmentRomStart;
    dmaLen = (size_t)((uintptr_t)_dmadataSegmentRomEnd - (uintptr_t)_dmadataSegmentRomStart);
    if (dmaOff == 0 || dmaLen < 32 || dmaOff + dmaLen > romSize) {
        seterr(err, errsz, TR("File table (dmadata) outside the ROM: port build error.", "Tabela de arquivos (dmadata) fora da ROM: erro de compilação do port."));
        goto done;
    }
    nEnt = dmaLen / 16;

    /* 1) mede: quantos arquivos, e quanto espaco os comprimidos ocupam depois de descomprimidos */
    newRomStart = (uint32_t*)calloc(nEnt + 1, sizeof(uint32_t));
    sf = (SwapFile*)calloc(nEnt + 1, sizeof(SwapFile));
    if (newRomStart == NULL || sf == NULL) {
        seterr(err, errsz, TR("Out of memory.", "Sem memória."));
        goto done;
    }
    newpos = (romSize + 0xFFF) & ~(size_t)0xFFF;
    total = newpos;
    for (i = 0; i < nEnt; i++) {
        const uint8_t* e = rom + dmaOff + i * 16;
        uint32_t vs = rd32be(e), ve = rd32be(e + 4), rs = rd32be(e + 8), re = rd32be(e + 12);
        if (ve == 0) {
            break; /* fim da tabela */
        }
        if (ve < vs) {
            seterr(err, errsz, TR("Corrupt dmadata (entry %u).", "dmadata corrompida (entrada %u)."), (unsigned)i);
            goto done;
        }
        if (rs == 0xFFFFFFFFu) { /* arquivo inexistente */
            newRomStart[i] = rs;
            continue;
        }
        if (re == 0) { /* nao comprimido: fica onde esta */
            if ((size_t)rs + (ve - vs) > romSize) {
                seterr(err, errsz, TR("File %u outside the ROM.", "Arquivo %u fora da ROM."), (unsigned)i);
                goto done;
            }
            newRomStart[i] = rs;
        } else { /* comprimido: vai para o fim do cartucho */
            if (re < rs || (size_t)re > romSize) {
                seterr(err, errsz, TR("Compressed file %u outside the ROM.", "Arquivo comprimido %u fora da ROM."), (unsigned)i);
                goto done;
            }
            newRomStart[i] = (uint32_t)total;
            total += (ve - vs + 0xF) & ~(size_t)0xF;
        }
    }
    total = (total + 0xFFF) & ~(size_t)0xFFF;
    total += CART_TRAILER_SIZE;

    /* 2) cria o arquivo de saida (num temporario; so renomeia no fim) */
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        seterr(err, errsz, TR("Could not create %s (%s). Is the SD card full?", "Não consegui criar %s (%s). Falta espaço no cartão?"), tmp, strerror(errno));
        goto done;
    }
    out = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out == MAP_FAILED) {
        out = NULL;
        seterr(err, errsz, TR("Cartridge mmap failed: %s", "mmap do cartucho falhou: %s"), strerror(errno));
        goto done;
    }

    progress(cb, ud, TR("Copying the ROM", "Copiando a ROM"), 5);
    memcpy(out, rom, romSize);

    /* 3) descomprime os arquivos comprimidos */
    for (i = 0; i < nEnt; i++) {
        const uint8_t* e = rom + dmaOff + i * 16;
        uint32_t vs = rd32be(e), ve = rd32be(e + 4), rs = rd32be(e + 8), re = rd32be(e + 12);
        int r;
        if (ve == 0) {
            break;
        }
        if (rs == 0xFFFFFFFFu || re == 0 || ve == vs) {
            continue;
        }
        r = yaz0_decode(rom + rs, re - rs, out + newRomStart[i], ve - vs);
        if (r != 0) {
            seterr(err, errsz, TR("Could not decompress file %u (code %d).", "Falha ao descomprimir o arquivo %u (código %d)."), (unsigned)i, r);
            goto done;
        }
        progress(cb, ud, "Descomprimindo", 5 + (int)(80 * (i + 1) / nEnt));
    }

    /* 3b) converte os dados de assets para o formato nativo (little-endian) com o mapa.
     *     So arquivos com tamanho > 0 entram na lista (uma entrada vazia na mesma posicao
     *     nao pode esconder o arquivo de verdade). */
    progress(cb, ud, TR("Converting the data", "Convertendo os dados"), 90);
    for (i = 0; i < nEnt; i++) {
        const uint8_t* e = rom + dmaOff + i * 16;
        uint32_t vs = rd32be(e), ve = rd32be(e + 4), rs = rd32be(e + 8), re = rd32be(e + 12);
        if (ve == 0) {
            break;
        }
        if (rs == 0xFFFFFFFFu || ve == vs) {
            continue;
        }
        sf[nsf].vrom_start = vs;
        sf[nsf].vrom_end = ve;
        sf[nsf].cart_off = (re == 0) ? rs : newRomStart[i];
        nsf++;
    }
    if (swapmap_apply(mapRaw, mapLen, out, total - CART_TRAILER_SIZE, sf, nsf, err, errsz) != 0) {
        goto done;
    }

    /* 4) dmadata nativa (little-endian), tudo "nao comprimido" */
    progress(cb, ud, TR("Writing the file table", "Escrevendo a tabela de arquivos"), 92);
    for (i = 0; i < nEnt; i++) {
        const uint8_t* e = rom + dmaOff + i * 16;
        uint32_t vs = rd32be(e), ve = rd32be(e + 4), rs = rd32be(e + 8);
        uint8_t* d = out + dmaOff + i * 16;
        if (ve == 0) {
            memset(d, 0, 16);
            break;
        }
        wr32le(d, vs);
        wr32le(d + 4, ve);
        wr32le(d + 8, (rs == 0xFFFFFFFFu) ? 0xFFFFFFFFu : newRomStart[i]);
        wr32le(d + 12, (rs == 0xFFFFFFFFu) ? 0xFFFFFFFFu : 0u);
    }

    /* 5) carimbo no fim */
    memset(out + total - CART_TRAILER_SIZE, 0, CART_TRAILER_SIZE);
    memcpy(out + total - CART_TRAILER_SIZE, CART_MAGIC, sizeof(CART_MAGIC) - 1);
    wr32le(out + total - 16, CART_FORMAT_VERSION);
    wr32le(out + total - 12, (uint32_t)total);
    wr32le(out + total - 8, mapId); /* qual mapa de conversao gerou este cartucho */

    progress(cb, ud, TR("Saving", "Gravando"), 96);
    if (msync(out, total, MS_SYNC) != 0) {
        seterr(err, errsz, TR("Could not write the cartridge: %s", "Falha ao gravar o cartucho: %s"), strerror(errno));
        goto done;
    }
    munmap(out, total);
    out = NULL;
    close(fd);
    fd = -1;
    if (rename(tmp, out_path) != 0) {
        seterr(err, errsz, TR("Could not rename the cartridge: %s", "Não consegui renomear o cartucho: %s"), strerror(errno));
        goto done;
    }
    progress(cb, ud, TR("Done", "Pronto"), 100);
    rc = 0;

done:
    if (out != NULL) {
        munmap(out, total);
    }
    if (fd >= 0) {
        close(fd);
        if (rc != 0) {
            unlink(tmp);
        }
    }
    free(newRomStart);
    free(sf);
    free(mapRaw);
    free(rom);
    return rc;
}

int cart_is_valid(const char* path, const char* swapmap_path) {
    uint8_t t[CART_TRAILER_SIZE];
    struct stat st;
    FILE* f = fopen(path, "rb");
    int ok = 0;
    uint32_t mapId = 0;
    if (f == NULL) {
        return 0;
    }
    if (swapmap_read_id(swapmap_path, &mapId) != 0) {
        fclose(f);
        return 0;
    }
    if (fstat(fileno(f), &st) == 0 && st.st_size > CART_TRAILER_SIZE &&
        fseek(f, -CART_TRAILER_SIZE, SEEK_END) == 0 && fread(t, 1, sizeof(t), f) == sizeof(t)) {
        uint32_t ver = t[16] | (t[17] << 8) | (t[18] << 16) | ((uint32_t)t[19] << 24);
        uint32_t sz = t[20] | (t[21] << 8) | (t[22] << 16) | ((uint32_t)t[23] << 24);
        uint32_t id = t[24] | (t[25] << 8) | (t[26] << 16) | ((uint32_t)t[27] << 24);
        ok = memcmp(t, CART_MAGIC, sizeof(CART_MAGIC) - 1) == 0 && ver == CART_FORMAT_VERSION &&
             sz == (uint32_t)st.st_size && id == mapId;
    }
    fclose(f);
    return ok;
}
