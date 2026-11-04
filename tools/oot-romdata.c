/*
 * oot-romdata.c -- ferramenta do release (roda no WSL): tira do executavel os dados que vem da ROM do jogo.
 *
 *   oot-romdata make  CARTUCHO EXE REGIOES RECEITA EXE_PUBLICADO [RELATORIO]
 *       REGIOES: arquivo de texto (tools/oot-release.sh, a partir do mapa da ligacao) com uma regiao por linha:
 *       "endereco tamanho deslocamento_no_arquivo nome". Para cada regiao acha no cartucho (oot.cart, feito da ROM
 *       do jogador) os pedacos iguais, lidos direto ou com os bytes invertidos de 2 em 2, 4 em 4 ou 8 em 8 (o port
 *       guarda quase tudo na ordem do processador). Escreve a RECEITA (sem dados do jogo: so de onde vem cada pedaco
 *       e os bytes proprios do port, como ponteiros) e o EXE_PUBLICADO, com essas regioes zeradas. Confere: refaz
 *       tudo a partir do cartucho e compara com o executavel original.
 *   oot-romdata apply CARTUCHO EXE_PUBLICADO RECEITA SAIDA
 *       o que o jogo faz ao abrir (port/romdata.c), no PC: para conferir.
 *
 * Receita (little-endian): "OOTRCP01", u32 crc do cartucho (sem o carimbo do fim), u32 tamanho do cartucho,
 * u32 numero de regioes; por regiao: u32 endereco, u32 tamanho, u32 crc dos bytes certos, u32 numero de operacoes;
 * operacoes: u8 0 + u32 n + n bytes (bytes do port); u8 1..4 + u32 posicao no cartucho + u32 n (copia; 1 = como
 * esta, 2/3/4 = bytes invertidos de 2/4/8 em 8); u8 5 + u8 valor + u32 n (n bytes iguais).
 *
 * Compilar: gcc -O2 -o oot-romdata tools/oot-romdata.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k_mask[4] = { 0, 1, 3, 7 };

static uint32_t crc32b(uint32_t crc, const uint8_t* d, size_t n) {
    static uint32_t t[256];
    size_t i;
    if (t[1] == 0) {
        uint32_t k, c;
        for (k = 0; k < 256; k++) {
            c = k;
            for (int j = 0; j < 8; j++) {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            }
            t[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static uint8_t* read_file(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    uint8_t* b;
    long s;
    if (f == NULL) {
        fprintf(stderr, "nao consegui abrir %s\n", p);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    s = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)s + 16);
    if (b == NULL || fread(b, 1, (size_t)s, f) != (size_t)s) {
        fprintf(stderr, "falha lendo %s\n", p);
        exit(1);
    }
    fclose(f);
    *n = (size_t)s;
    return b;
}

static void write_file(const char* p, const uint8_t* b, size_t n) {
    FILE* f = fopen(p, "wb");
    if (f == NULL || fwrite(b, 1, n, f) != n) {
        fprintf(stderr, "falha gravando %s\n", p);
        exit(1);
    }
    fclose(f);
}

/* ---- o cartucho e o indice (janelas de 8 bytes em cada posicao multipla de 4, nas 4 vistas) ---- */
#define CART_TRAILER 32
static uint8_t* g_cart;
static uint32_t g_cartN; /* sem o carimbo */
#define HB 24
#define WIN 8
static uint32_t *g_head, *g_next, g_nEnt;

static inline uint8_t view_byte(int v, uint32_t q) {
    return g_cart[q ^ k_mask[v]];
}

static inline uint32_t hash8(const uint8_t* b) {
    uint64_t x;
    memcpy(&x, b, 8);
    return (uint32_t)((x * 0x9E3779B97F4A7C15ull) >> (64 - HB));
}

static int all_same(const uint8_t* b, int n) {
    for (int i = 1; i < n; i++) {
        if (b[i] != b[0]) {
            return 0;
        }
    }
    return 1;
}

/* vista e posicao de cada janela do indice */
static uint32_t* g_ent;
static void build_index2(void) {
    uint32_t per = g_cartN / 4, q, k = 0;
    g_head = malloc(sizeof(uint32_t) << HB);
    g_next = malloc(sizeof(uint32_t) * (size_t)per * 4 + 4);
    g_ent = malloc(sizeof(uint32_t) * (size_t)per * 4 + 4);
    memset(g_head, 0xFF, sizeof(uint32_t) << HB);
    for (int v = 0; v < 4; v++) {
        for (q = 0; q + WIN + 8 <= g_cartN; q += 4) {
            uint8_t w[WIN];
            uint32_t h;
            for (int j = 0; j < WIN; j++) {
                w[j] = view_byte(v, q + j);
            }
            if (all_same(w, WIN)) {
                continue;
            }
            h = hash8(w);
            g_ent[k] = (uint32_t)v << 30 | (q >> 2);
            g_next[k] = g_head[h];
            g_head[h] = k;
            k++;
        }
    }
    g_nEnt = k;
    fprintf(stderr, "indice: %u janelas\n", k);
}

/* segundo indice: janelas de 4 bytes (ancora para estruturas que misturam campos de tamanhos diferentes, em que
 * nenhum trecho de 8 bytes fica numa vista so: tabelas de mensagens, de cenas, da camera...) */
#define HB4 24
static uint32_t *g_head4, *g_next4, *g_ent4;
static inline uint32_t hash4(const uint8_t* b) {
    uint32_t x;
    memcpy(&x, b, 4);
    return (uint32_t)((x * 0x9E3779B1u) >> (32 - HB4));
}
static void build_index4(void) {
    uint32_t per = g_cartN / 4, q, k = 0;
    g_head4 = malloc(sizeof(uint32_t) << HB4);
    g_next4 = malloc(sizeof(uint32_t) * (size_t)per * 4 + 4);
    g_ent4 = malloc(sizeof(uint32_t) * (size_t)per * 4 + 4);
    memset(g_head4, 0xFF, sizeof(uint32_t) << HB4);
    for (int v = 0; v < 4; v++) {
        for (q = 0; q + 16 <= g_cartN; q += 4) {
            uint8_t w[4];
            for (int j = 0; j < 4; j++) {
                w[j] = view_byte(v, q + j);
            }
            if (all_same(w, 4)) {
                continue;
            }
            {
                uint32_t h = hash4(w);
                g_ent4[k] = (uint32_t)v << 30 | (q >> 2);
                g_next4[k] = g_head4[h];
                g_head4[h] = k;
                k++;
            }
        }
    }
    fprintf(stderr, "indice de 4 bytes: %u janelas\n", k);
}

/* quanto de b (a partir de i) sai do cartucho a partir de q, trocando de vista a cada campo (pedacos de 2+ bytes) */
static uint32_t struct_score(const uint8_t* b, uint32_t i, uint32_t size, uint32_t q) {
    uint32_t got = 0, misses = 0;
    while (i < size && q < g_cartN && misses < 5) { /* (um ponteiro de 4 bytes no meio passa) */
        uint32_t best = 0;
        for (int v = 0; v < 4; v++) {
            uint32_t n = 0;
            while (i + n < size && q + n < g_cartN && view_byte(v, q + n) == b[i + n]) {
                n++;
            }
            if (n > best) {
                best = n;
            }
        }
        if (best >= 2) {
            got += best;
            i += best;
            q += best;
            misses = 0;
        } else {
            i++;
            q++;
            misses++;
        }
    }
    return got;
}

/* ---- receita ---- */
typedef struct {
    uint8_t* b;
    size_t n, cap;
} Buf;
static void put(Buf* o, const void* d, size_t n) {
    if (o->n + n > o->cap) {
        o->cap = (o->n + n) * 2 + 4096;
        o->b = realloc(o->b, o->cap);
    }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}
static void put32(Buf* o, uint32_t v) {
    put(o, &v, 4);
}
static void put8(Buf* o, uint8_t v) {
    put(o, &v, 1);
}

typedef struct {
    uint32_t addr, size, off;
    char name[200];
} Reg;

/* literal pendente */
static void flush_lit(Buf* ops, uint32_t* nops, const uint8_t* b, uint32_t from, uint32_t to, uint64_t* litBytes) {
    uint32_t i = from;
    while (i < to) {
        uint32_t j = i;
        while (j < to && b[j] == b[i]) {
            j++;
        }
        if (j - i >= 8) { /* sequencia de bytes iguais (zeros quase sempre) */
            put8(ops, 5);
            put8(ops, b[i]);
            put32(ops, j - i);
            (*nops)++;
            i = j;
            continue;
        }
        /* literal ate a proxima sequencia longa */
        {
            uint32_t k = i, s = i;
            while (k < to) {
                uint32_t r = k;
                while (r < to && b[r] == b[k]) {
                    r++;
                }
                if (r - k >= 8) {
                    break;
                }
                k = r;
            }
            put8(ops, 0);
            put32(ops, k - s);
            put(ops, b + s, k - s);
            *litBytes += k - s;
            (*nops)++;
            i = k;
        }
    }
}

typedef struct {
    uint32_t len, addr;
    char name[200];
} Lit;
static Lit g_lits[64];
static void note_lit(const char* name, uint32_t addr, uint32_t len) {
    int lo = 0;
    for (int i = 1; i < 64; i++) {
        if (g_lits[i].len < g_lits[lo].len) {
            lo = i;
        }
    }
    if (len > g_lits[lo].len) {
        g_lits[lo].len = len;
        g_lits[lo].addr = addr;
        snprintf(g_lits[lo].name, sizeof g_lits[lo].name, "%s", name);
    }
}

static void make_region(Buf* out, const Reg* r, const uint8_t* b, uint64_t* copied, uint64_t* lit, FILE* rep) {
    Buf ops = { 0 };
    uint32_t nops = 0, i = 0, litFrom = 0, nextQ = 0xFFFFFFFFu;
    uint64_t c0 = *copied, l0 = *lit;
    while (i < r->size) {
        uint32_t bestLen = 0, bestQ = 0, bestV = 0;
        /* continuacao: o pedaco seguinte no cartucho, em qualquer vista (estruturas que misturam campos de 1, 2 e 4
         * bytes: vertices, tabelas de mensagens, de cenas...) */
        if (nextQ != 0xFFFFFFFFu) {
            for (uint32_t v = 0; v < 4; v++) {
                uint32_t n = 0;
                while (i + n < r->size && nextQ + n < g_cartN && view_byte((int)v, nextQ + n) == b[i + n]) {
                    n++;
                }
                if (n > bestLen) {
                    bestLen = n;
                    bestQ = nextQ;
                    bestV = v;
                }
            }
            if (bestLen < 2) {
                bestLen = 0;
            }
        }
        if (bestLen < WIN && i + WIN <= r->size && !all_same(b + i, WIN)) {
            uint32_t h = hash8(b + i), e = g_head[h];
            int tries = 0;
            while (e != 0xFFFFFFFFu && tries < 200) {
                uint32_t v = g_ent[e] >> 30, q = (g_ent[e] & 0x3FFFFFFF) << 2, n = 0;
                while (i + n < r->size && q + n < g_cartN && view_byte((int)v, q + n) == b[i + n]) {
                    n++;
                }
                if (n > bestLen) {
                    bestLen = n;
                    bestQ = q;
                    bestV = v;
                }
                e = g_next[e];
                tries++;
            }
        }
        if (bestLen < WIN && !(bestLen >= 2 && bestQ == nextQ) && i + 4 <= r->size && !all_same(b + i, 4)) {
            /* ancora de 4 bytes, se dali em diante a estrutura continua saindo do cartucho */
            uint32_t bestS = 0, bestA = 0;
            /* padroes: os 4 bytes numa vista so; ou 2 campos de 2 bytes, so um deles invertido (procurados como estao
             * no cartucho, na vista direta) */
            for (int pat = 0; pat < 3; pat++) {
                uint8_t w[4] = { b[i], b[i + 1], b[i + 2], b[i + 3] };
                uint32_t e;
                int tries = 0;
                if (pat == 1) {
                    w[2] = b[i + 3];
                    w[3] = b[i + 2];
                } else if (pat == 2) {
                    w[0] = b[i + 1];
                    w[1] = b[i];
                }
                if (pat > 0 && (all_same(w, 2) || all_same(w + 2, 2))) {
                    continue; /* (um dos campos com os 2 bytes iguais: e o mesmo que o padrao 0) */
                }
                for (e = g_head4[hash4(w)]; e != 0xFFFFFFFFu && tries < 64; e = g_next4[e], tries++) {
                    uint32_t v = g_ent4[e] >> 30, q = (g_ent4[e] & 0x3FFFFFFF) << 2;
                    if ((pat > 0 && v != 0) || view_byte((int)v, q) != w[0] || view_byte((int)v, q + 1) != w[1] ||
                        view_byte((int)v, q + 2) != w[2] || view_byte((int)v, q + 3) != w[3]) {
                        continue;
                    }
                    {
                        uint32_t sc = struct_score(b, i, r->size, q);
                        if (sc > bestS) {
                            bestS = sc;
                            bestA = q;
                        }
                    }
                }
            }
            if (bestS >= 12) {
                nextQ = bestA;
                continue;
            }
        }
        if (bestLen >= WIN || (bestLen >= 2 && bestQ == nextQ)) {
            if (litFrom < i) {
                uint64_t before = *lit;
                flush_lit(&ops, &nops, b, litFrom, i, lit);
                if (*lit - before >= 16) {
                    note_lit(r->name, r->addr + litFrom, (uint32_t)(*lit - before));
                }
            }
            put8(&ops, (uint8_t)(1 + bestV));
            put32(&ops, bestQ);
            put32(&ops, bestLen);
            nops++;
            *copied += bestLen;
            i += bestLen;
            litFrom = i;
            nextQ = bestQ + bestLen;
        } else {
            i++;
            if (nextQ != 0xFFFFFFFFu) {
                nextQ++; /* (pula 1 byte nos dois lados: um ponteiro no meio de uma estrutura) */
            }
        }
    }
    if (litFrom < r->size) {
        uint64_t before = *lit;
        flush_lit(&ops, &nops, b, litFrom, r->size, lit);
        if (*lit - before >= 16) {
            note_lit(r->name, r->addr + litFrom, (uint32_t)(*lit - before));
        }
    }
    put32(out, r->addr);
    put32(out, r->size);
    put32(out, crc32b(0, b, r->size));
    put32(out, nops);
    put(out, ops.b, ops.n);
    free(ops.b);
    if (rep != NULL) {
        fprintf(rep, "%8u copiados %8u do port %6u bytes  %s\n", (unsigned)(*copied - c0), (unsigned)(*lit - l0),
                r->size, r->name);
    }
}

/* refaz uma regiao a partir da receita (o mesmo que port/romdata.c faz no aparelho) */
static const uint8_t* apply_region(const uint8_t* p, const uint8_t* end, uint8_t* dst, uint32_t size, uint32_t nops) {
    uint32_t o = 0;
    for (uint32_t k = 0; k < nops; k++) {
        uint8_t op;
        uint32_t a, n;
        if (p >= end) {
            return NULL;
        }
        op = *p++;
        if (op == 0) {
            memcpy(&n, p, 4);
            p += 4;
            if (o + n > size) {
                return NULL;
            }
            memcpy(dst + o, p, n);
            p += n;
        } else if (op >= 1 && op <= 4) {
            memcpy(&a, p, 4);
            memcpy(&n, p + 4, 4);
            p += 8;
            if (o + n > size || a + n > g_cartN) {
                return NULL;
            }
            for (uint32_t j = 0; j < n; j++) {
                dst[o + j] = g_cart[(a + j) ^ k_mask[op - 1]];
            }
        } else if (op == 5) {
            uint8_t v = p[0];
            memcpy(&n, p + 1, 4);
            p += 5;
            if (o + n > size) {
                return NULL;
            }
            memset(dst + o, v, n);
        } else {
            return NULL;
        }
        o += n;
    }
    return o == size ? p : NULL;
}

static int apply_all(const uint8_t* rcp, size_t rn, uint8_t* exe, const Reg* regs, int nr) {
    const uint8_t *p = rcp + 20, *end = rcp + rn;
    uint32_t nreg;
    (void)regs;
    (void)nr;
    if (rn < 20 || memcmp(rcp, "OOTRCP01", 8) != 0) {
        return -1;
    }
    memcpy(&nreg, rcp + 16, 4);
    for (uint32_t k = 0; k < nreg; k++) {
        uint32_t addr, size, crc, nops;
        memcpy(&addr, p, 4);
        memcpy(&size, p + 4, 4);
        memcpy(&crc, p + 8, 4);
        memcpy(&nops, p + 12, 4);
        p += 16;
        {
            uint8_t* tmp = malloc(size + 1);
            uint32_t off = 0;
            /* acha o deslocamento no arquivo pelo endereco (tabela de regioes) */
            int found = 0;
            for (int j = 0; j < nr; j++) {
                if (regs[j].addr == addr && regs[j].size == size) {
                    off = regs[j].off;
                    found = 1;
                    break;
                }
            }
            p = apply_region(p, end, tmp, size, nops);
            if (p == NULL || !found || crc32b(0, tmp, size) != crc) {
                fprintf(stderr, "regiao %08x: falhou\n", addr);
                free(tmp);
                return -2;
            }
            memcpy(exe + off, tmp, size);
            free(tmp);
        }
    }
    return 0;
}

static Reg* read_regs(const char* path, int* nr) {
    FILE* f = fopen(path, "r");
    Reg* r = NULL;
    int n = 0, cap = 0;
    char line[512];
    if (f == NULL) {
        fprintf(stderr, "nao consegui abrir %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        Reg x;
        memset(&x, 0, sizeof x);
        if (sscanf(line, "%x %x %x %199s", &x.addr, &x.size, &x.off, x.name) != 4) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            r = realloc(r, sizeof(Reg) * (size_t)cap);
        }
        r[n++] = x;
    }
    fclose(f);
    *nr = n;
    return r;
}

int main(int argc, char** argv) {
    size_t cartN, exeN;
    if (argc >= 6 && !strcmp(argv[1], "make")) {
        int nr;
        Reg* regs;
        uint8_t *exe, *pub;
        Buf out = { 0 };
        uint64_t copied = 0, lit = 0, total = 0;
        FILE* rep = argc > 7 ? fopen(argv[7], "w") : NULL;
        g_cart = read_file(argv[2], &cartN);
        g_cartN = (uint32_t)(cartN - CART_TRAILER);
        exe = read_file(argv[3], &exeN);
        regs = read_regs(argv[4], &nr);
        build_index2();
        build_index4();
        put(&out, "OOTRCP01", 8);
        put32(&out, crc32b(0, g_cart, g_cartN));
        put32(&out, g_cartN);
        put32(&out, (uint32_t)nr);
        for (int k = 0; k < nr; k++) {
            if ((size_t)regs[k].off + regs[k].size > exeN) {
                fprintf(stderr, "regiao fora do executavel: %s\n", regs[k].name);
                return 1;
            }
            make_region(&out, &regs[k], exe + regs[k].off, &copied, &lit, rep);
            total += regs[k].size;
        }
        pub = malloc(exeN);
        memcpy(pub, exe, exeN);
        for (int k = 0; k < nr; k++) {
            memset(pub + regs[k].off, 0, regs[k].size);
        }
        /* confere: refaz a partir do cartucho e compara */
        {
            uint8_t* chk = malloc(exeN);
            memcpy(chk, pub, exeN);
            if (apply_all(out.b, out.n, chk, regs, nr) != 0 || memcmp(chk, exe, exeN) != 0) {
                fprintf(stderr, "ERRO: a receita nao refaz o executavel\n");
                return 1;
            }
            free(chk);
        }
        write_file(argv[5], out.b, out.n);
        write_file(argv[6], pub, exeN);
        fprintf(stderr, "%d regioes, %llu bytes: %llu vem do cartucho, %llu sao do port (ponteiros etc.), o resto bytes "
                        "repetidos; receita %zu bytes. Conferido: refaz o executavel igual.\n",
                nr, (unsigned long long)total, (unsigned long long)copied, (unsigned long long)lit, out.n);
        if (rep != NULL) {
            fprintf(rep, "\nmaiores trechos que nao vieram do cartucho (vao na receita como bytes do port):\n");
            for (int a = 0; a < 64; a++) {
                for (int b = a + 1; b < 64; b++) {
                    if (g_lits[b].len > g_lits[a].len) {
                        Lit t = g_lits[a];
                        g_lits[a] = g_lits[b];
                        g_lits[b] = t;
                    }
                }
            }
            for (int a = 0; a < 64 && g_lits[a].len; a++) {
                fprintf(rep, "%6u bytes em %08x  %s\n", g_lits[a].len, g_lits[a].addr, g_lits[a].name);
            }
            fclose(rep);
        }
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "apply")) {
        fprintf(stderr, "(use port/romdata.c no jogo; aqui so o make confere)\n");
        return 1;
    }
    fprintf(stderr, "uso: %s make CARTUCHO EXE REGIOES RECEITA EXE_PUBLICADO [RELATORIO]\n", argv[0]);
    return 2;
}
