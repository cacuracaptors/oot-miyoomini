/*
 * romdata.c -- refaz, ao abrir, os dados do jogo que o executavel publicado nao traz.
 *
 * O release (tools/oot-release.sh) zera no executavel os dados que vem da ROM (texturas, modelos e tabelas dos
 * atores, tabelas do codigo...) e grava ao lado a receita oot_dados.rcp (tools/oot-romdata.c): para cada regiao,
 * de onde vem cada pedaco no cartucho oot.cart (que o jogo ja faz a partir da ROM do jogador), com a ordem dos bytes,
 * mais os bytes que sao do proprio port (ponteiros, mudancas do port). Nenhum dado do jogo vai no release.
 * Formato: ver tools/oot-romdata.c. Cada regiao refeita e conferida pelo crc32 antes de entrar no lugar.
 *
 * Chamado no comeco do main, com o cartucho pronto e antes de qualquer codigo do jogo rodar (e antes da copia dos
 * dados iniciais dos overlays, ovl_reset.c).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* marca do tipo de executavel: o tools/oot-release.sh troca "OOTDEV1" por "OOTREL1" no executavel publicado (os
 * dados do jogo zerados: sem a receita ele nao roda) */
__attribute__((used)) volatile const char port_build_kind[8] = "OOTDEV1";

#define CART_TRAILER 32
static const uint32_t k_mask[4] = { 0, 1, 3, 7 };

static uint32_t crc32b(const uint8_t* d, size_t n) {
    static uint32_t t[256];
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (t[1] == 0) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t c = k;
            for (int j = 0; j < 8; j++) {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            }
            t[k] = c;
        }
    }
    for (i = 0; i < n; i++) {
        crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* segmentos carregados do proprio executavel (para liberar a escrita na .rodata e devolver a protecao depois) */
typedef struct {
    uintptr_t lo, hi;
    int prot;
} Seg;
static Seg g_seg[8];
static int g_nseg;

static int seg_cb(struct dl_phdr_info* info, size_t sz, void* ud) {
    (void)sz;
    (void)ud;
    if (info->dlpi_name != NULL && info->dlpi_name[0] != 0) {
        return 0; /* so o executavel (nome vazio) */
    }
    for (int i = 0; i < info->dlpi_phnum && g_nseg < 8; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            g_seg[g_nseg].lo = info->dlpi_addr + ph->p_vaddr;
            g_seg[g_nseg].hi = g_seg[g_nseg].lo + ph->p_memsz;
            g_seg[g_nseg].prot = ((ph->p_flags & PF_R) ? PROT_READ : 0) | ((ph->p_flags & PF_W) ? PROT_WRITE : 0) |
                                 ((ph->p_flags & PF_X) ? PROT_EXEC : 0);
            g_nseg++;
        }
    }
    return 1;
}

/* teste no PC (tools): em vez de escrever no lugar, entrega cada regiao refeita a esta funcao */
static void (*g_sink)(uint32_t addr, const uint8_t* d, uint32_t n);
void port_romdata_test_sink(void (*sink)(uint32_t addr, const uint8_t* d, uint32_t n)) {
    g_sink = sink;
}

/* devolve 0 = ok (ou nada a fazer), != 0 = erro (mensagem em err) */
int port_romdata_apply(const char* cartPath, const char* rcpPath, char* err, size_t errsz) {
    const int release = g_sink != NULL || !memcmp((const char*)port_build_kind, "OOTREL1", 7);
    FILE* f;
    uint8_t* rcp = NULL;
    const uint8_t *p, *end;
    long rn;
    int fd = -1;
    struct stat st;
    const uint8_t* cart = MAP_FAILED;
    uint32_t cartN, nreg, k;
    uint8_t* tmp = NULL;
    size_t tmpCap = 0;
    long page = sysconf(_SC_PAGESIZE);
    int rc = 1;

    f = fopen(rcpPath, "rb");
    if (f == NULL) {
        if (release) {
            snprintf(err, errsz, "The file %s is missing\nfrom the game folder.", rcpPath);
            return 1;
        }
        return 0; /* executavel de desenvolvimento: ja tem tudo */
    }
    fseek(f, 0, SEEK_END);
    rn = ftell(f);
    fseek(f, 0, SEEK_SET);
    rcp = (uint8_t*)malloc(rn > 0 ? (size_t)rn : 1);
    if (rcp == NULL || rn < 20 || fread(rcp, 1, (size_t)rn, f) != (size_t)rn) {
        fclose(f);
        free(rcp);
        snprintf(err, errsz, "Could not read %s.", rcpPath);
        return 1;
    }
    fclose(f);
    if (!release) {
        free(rcp);
        return 0; /* (receita ao lado de um executavel de desenvolvimento: ele ja tem os dados) */
    }
    if (memcmp(rcp, "OOTRCP01", 8) != 0) {
        snprintf(err, errsz, "%s is not a recipe of this port.", rcpPath);
        goto done;
    }
    fd = open(cartPath, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= CART_TRAILER) {
        snprintf(err, errsz, "Could not open %s.", cartPath);
        goto done;
    }
    cartN = (uint32_t)(st.st_size - CART_TRAILER);
    if (cartN != rd32(rcp + 12)) {
        snprintf(err, errsz, "The cartridge (%s) does not match this version.\nDelete it and copy the ROM to the game "
                             "folder again.", cartPath);
        goto done;
    }
    cart = (const uint8_t*)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (cart == MAP_FAILED) {
        snprintf(err, errsz, "Could not map %s.", cartPath);
        goto done;
    }
    dl_iterate_phdr(seg_cb, NULL);
    nreg = rd32(rcp + 16);
    p = rcp + 20;
    end = rcp + rn;
    for (k = 0; k < nreg; k++) {
        uint32_t addr, size, crc, nops, o = 0, j;
        if (p + 16 > end) {
            break;
        }
        addr = rd32(p);
        size = rd32(p + 4);
        crc = rd32(p + 8);
        nops = rd32(p + 12);
        p += 16;
        if (size > tmpCap) {
            free(tmp);
            tmpCap = size;
            tmp = (uint8_t*)malloc(tmpCap);
            if (tmp == NULL) {
                snprintf(err, errsz, "Out of memory.");
                goto done;
            }
        }
        for (j = 0; j < nops && p < end; j++) {
            const uint8_t op = *p++;
            uint32_t a, n;
            if (op == 0) {
                n = rd32(p);
                if (o + n > size || p + 4 + n > end) {
                    break;
                }
                memcpy(tmp + o, p + 4, n);
                p += 4 + n;
            } else if (op >= 1 && op <= 4) {
                const uint32_t m = k_mask[op - 1];
                a = rd32(p);
                n = rd32(p + 4);
                p += 8;
                if (o + n > size || a + n + 8 > cartN) {
                    break;
                }
                if (m == 0) {
                    memcpy(tmp + o, cart + a, n);
                } else {
                    for (uint32_t i = 0; i < n; i++) {
                        tmp[o + i] = cart[(a + i) ^ m];
                    }
                }
            } else if (op == 5) {
                n = rd32(p + 1);
                if (o + n > size) {
                    break;
                }
                memset(tmp + o, p[0], n);
                p += 5;
            } else {
                break;
            }
            o += n;
        }
        if (j != nops || o != size || crc32b(tmp, size) != crc) {
            snprintf(err, errsz, "The game data could not be rebuilt from\nthe cartridge (%s, region %u).\nDelete it "
                                 "and copy the ROM to the game folder again.", cartPath, (unsigned)k);
            goto done;
        }
        if (g_sink != NULL) {
            g_sink(addr, tmp, size);
            continue;
        }
        {
            /* no lugar (a .rodata fica num trecho so de leitura: libera a escrita e devolve a protecao) */
            const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)(page - 1);
            const uintptr_t hi = ((uintptr_t)addr + size + page - 1) & ~(uintptr_t)(page - 1);
            int prot = PROT_READ | PROT_WRITE;
            for (int s = 0; s < g_nseg; s++) {
                if ((uintptr_t)addr >= g_seg[s].lo && (uintptr_t)addr < g_seg[s].hi) {
                    prot = g_seg[s].prot;
                }
            }
            if (!(prot & PROT_WRITE) && mprotect((void*)lo, hi - lo, prot | PROT_WRITE) != 0) {
                snprintf(err, errsz, "mprotect failed (region %u).", (unsigned)k);
                goto done;
            }
            memcpy((void*)(uintptr_t)addr, tmp, size);
            if (!(prot & PROT_WRITE)) {
                mprotect((void*)lo, hi - lo, prot);
            }
        }
    }
    if (k != nreg) {
        snprintf(err, errsz, "%s is damaged.", rcpPath);
        goto done;
    }
    fprintf(stderr, "[dados] %u regioes refeitas a partir de %s\n", (unsigned)nreg, cartPath);
    rc = 0;
done:
    if (cart != MAP_FAILED) {
        munmap((void*)cart, (size_t)st.st_size);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(tmp);
    free(rcp);
    return rc;
}
