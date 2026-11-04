/* gfx_hd.c -- texturas novas (pacote texturas_hd.pak, feito por tools/hd/oot-hd-pack.py a partir de
 * pacotes do Ship of Harkinian).
 *
 * Como uma textura do jogo e reconhecida:
 *  1. toda leitura da ROM (DMA) anota de onde veio cada pagina da RAM: endereco no cartucho -> vrom
 *     (posicao do arquivo na ROM original, pela dmadata do cartucho) -> pagina da RAM;
 *  2. a carga de textura (LOADBLOCK/LOADTILE) guarda o 1o byte que leu (gx.tmemPtr);
 *  3. quando o renderizador decodifica uma textura nova na cache, o endereco do 1o byte vira vrom e e
 *     procurado no pacote; formato e tamanho tem de bater com a tile, e o CRC32 dos bytes na memoria tem de
 *     ser o da textura original (uma textura que a traducao mudou, por exemplo, fica a do jogo).
 * A textura nova fica pendurada na original (tx->hd) e o caminho rapido a usa no lugar dela, com as
 * coordenadas multiplicadas pela escala.
 * Ler do cartao e descomprimir fica numa thread separada: o desenho nunca espera. Enquanto a nova nao
 * chega, a original e desenhada; quando chega, entra no proximo uso da textura (gx_hd_retry). */
#include "gfx_priv.h"
#include "../inflate.h"
#include "../perf_trace.h"
#include "os_hw.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

extern char _dmadataSegmentRomStart[];
/* fonte das mensagens: o pacote so traz algumas letras novas; misturadas com as originais o texto fica
 * irregular (umas finas e nitidas, outras suaves). As letras ficam sempre as originais. */
extern char _nes_font_staticSegmentRomStart[];
extern char _nes_font_staticSegmentRomEnd[];
static unsigned g_nFont;
extern int gx_hd_font_ok;          /* gfx_rdp.c: letras novas so no PRESS START da abertura (z_en_mag.c) */
extern char _dmadataSegmentRomEnd[];

extern int gx_hd_use;              /* gfx_rdp.c: 2 = texturas novas so na logo da abertura, 0 = desligadas */
int port_opt_hd_logo = 1;          /* opcao hd_title_logo (options.txt): 1 = logo da abertura com as texturas novas */
extern uint32_t gx_tex_cache_bytes; /* gfx_rdp.c */
extern int gx_hd_on;                /* gfx_rdp.c: liga a anotacao da origem de cada palavra da TMEM */

typedef struct {
    uint32_t vrom, crc;
    uint8_t fmt, siz;
    uint16_t ow, oh, hw, hh, pad;
    uint32_t off, clen, rawlen;
} HdEnt; /* 32 bytes, igual ao arquivo */
_Static_assert(sizeof(HdEnt) == 32, "HdEnt deve ter 32 bytes");

typedef struct {
    uint32_t cs, ce, vs; /* no cartucho [cs, ce) = vrom vs.. */
} CartFile;

/* por pagina de 4 KB da RAM, as ultimas leituras da ROM que cairam nela (a mais nova primeiro): duas
 * leituras podem dividir uma pagina (o fim de um arquivo e o comeco do proximo, como as faces do ceu) */
#define PAGE_SLOTS 4
typedef struct {
    uint32_t ram, len, vrom; /* vrom 0 = leitura de fora dos arquivos conhecidos */
} PageSlot;
typedef struct {
    PageSlot s[PAGE_SLOTS];
} PageRec;

#define RAM_BASE 0x80000000u
#define RAM_PAGES 4096u /* 16 MB em paginas de 4 KB */

static HdEnt *g_ent;
static uint32_t g_nent;
static int g_fd = -1;
static CartFile *g_files;
static uint32_t g_nfiles;
static PageRec g_page[RAM_PAGES];
static int g_ready;
static uint32_t g_nApplied, g_nCrc, g_nShape, g_nMiss, g_nNoVrom;
/* carregamento em segundo plano (mais abaixo) */
static uint8_t *g_st;
static uint32_t **g_px;
static uint8_t *g_crcOk;
static uint32_t *g_used;
static void *loader(void *arg);
/* ultima vez (ms) que cada textura foi usada; a busca antecipada de uma sala nova pode liberar as que nao
 * sao usadas ha alguns segundos (antes era recusada quando a memoria enchia: na 1a entrada numa casa, as
 * texturas dela chegavam so quando o desenho pedia, aos pedacos) */
static uint32_t *g_usedMs;
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER; /* g_px: o desenho le, o carregador libera */
#define HD_EVICT_IDLE_MS 3000u

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_of(const uint8_t *d, uint32_t n) {
    static uint32_t tab[256];
    static int init;
    uint32_t c = 0xFFFFFFFFu, i;
    if (!init) {
        for (i = 0; i < 256; i++) {
            uint32_t v = i;
            int k;
            for (k = 0; k < 8; k++) {
                v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1)));
            }
            tab[i] = v;
        }
        init = 1;
    }
    for (i = 0; i < n; i++) {
        c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    }
    return ~c;
}

static int cmp_file(const void *a, const void *b) {
    const CartFile *x = (const CartFile *)a, *y = (const CartFile *)b;
    return x->cs < y->cs ? -1 : x->cs > y->cs;
}

/* chamado pelo port_main depois de abrir o cartucho e ler as opcoes */
void gx_hd_init(const char *path) {
    uint8_t hdr[16];
    uint32_t dmaOff = (uint32_t)(uintptr_t)_dmadataSegmentRomStart;
    uint32_t nEnt = (uint32_t)((uintptr_t)_dmadataSegmentRomEnd - (uintptr_t)_dmadataSegmentRomStart) / 16;
    uint32_t i;
    gx_hd_use = port_opt_hd_logo ? 2 : 0; /* (desde 03/11 as texturas novas so servem a logo da abertura) */
    if (!gx_hd_use) {
        fprintf(stderr, "[texturas] desligadas (opcao hd_title_logo = 0)\n");
        return;
    }
    g_fd = open(path, O_RDONLY);
    if (g_fd < 0) {
        fprintf(stderr, "[texturas] sem %s: texturas do jogo\n", path);
        return;
    }
    if (pread(g_fd, hdr, 16, 0) != 16 || memcmp(hdr, "OOTHD01\0", 8) != 0) {
        fprintf(stderr, "[texturas] %s: formato desconhecido\n", path);
        close(g_fd);
        g_fd = -1;
        return;
    }
    g_nent = rd32le(hdr + 8);
    g_ent = (HdEnt *)malloc((size_t)g_nent * sizeof(HdEnt));
    if (g_ent == NULL || pread(g_fd, g_ent, (size_t)g_nent * sizeof(HdEnt), 16) != (ssize_t)((size_t)g_nent * sizeof(HdEnt))) {
        fprintf(stderr, "[texturas] %s: nao consegui ler o indice\n", path);
        free(g_ent);
        g_ent = NULL;
        close(g_fd);
        g_fd = -1;
        return;
    }
    /* onde cada arquivo esta no cartucho (dmadata nativa) */
    g_files = (CartFile *)calloc(nEnt, sizeof(CartFile));
    for (i = 0; g_files != NULL && i < nEnt; i++) {
        uint8_t e[16];
        uint32_t vs, ve, rs;
        if (port_hooks.rom_read == NULL || port_hooks.rom_read(dmaOff + i * 16, e, 16) != 0) {
            break;
        }
        vs = rd32le(e);
        ve = rd32le(e + 4);
        rs = rd32le(e + 8);
        if (ve == 0) {
            break;
        }
        if (rs == 0xFFFFFFFFu || ve <= vs) {
            continue;
        }
        g_files[g_nfiles].cs = rs;
        g_files[g_nfiles].ce = rs + (ve - vs);
        g_files[g_nfiles].vs = vs;
        g_nfiles++;
    }
    qsort(g_files, g_nfiles, sizeof(CartFile), cmp_file);
    g_st = (uint8_t *)calloc(g_nent, 1);
    g_px = (uint32_t **)calloc(g_nent, sizeof(uint32_t *));
    g_crcOk = (uint8_t *)calloc(g_nent, 1);
    g_used = (uint32_t *)calloc(g_nent, sizeof(uint32_t));
    g_usedMs = (uint32_t *)calloc(g_nent, sizeof(uint32_t));
    g_ready = g_nfiles > 0 && g_nent > 0 && g_st != NULL && g_px != NULL && g_crcOk != NULL && g_used != NULL &&
              g_usedMs != NULL;
    if (g_ready) {
        pthread_t th;
        if (pthread_create(&th, NULL, loader, NULL) != 0) {
            g_ready = 0;
        } else {
            pthread_detach(th);
        }
    }
    gx_hd_on = g_ready && gx_hd_use != 2; /* (so a logo: liga so dentro das marcas dela, gx_hd_logo_mark) */
    if (g_ready && gx_tex_cache_bytes < (48u << 20)) {
        gx_tex_cache_bytes = 48u << 20; /* as novas sao maiores: mais espaco na cache de texturas */
    }
    fprintf(stderr, "[texturas] %s: %u texturas novas, %u arquivos no cartucho\n", path, g_nent, g_nfiles);
}

/* ---- busca antecipada ----
 * Quando o jogo le um arquivo da ROM (sala, cena, objeto), as texturas novas que estao nele ja vao para o
 * carregador: chegam enquanto a sala ainda carrega e a tela escurece/clareia. Logo depois de uma leitura
 * assim (g_freshUntil), o desenho espera pela textura nova em vez de mostrar a original e trocar depois
 * (antes, ao entrar numa casa, as paredes e o chao apareciam e iam trocando aos pedacos). */
static void enqueue_to(uint32_t i, int urgent);
static uint64_t g_freshUntil;
static uint32_t g_nPrefetch;
#define HD_WAIT_WINDOW_NS 2000000000ull /* 2 s depois da leitura */
#define HD_WAIT_FRAME_NS 150000000ull   /* no maximo 150 ms de espera num desenho */
#define HD_WAIT_FIRST_NS 800000000ull   /* mas os primeiros desenhos de uma sala nova podem esperar ate 0,8 s */
static uint64_t g_winWaited;            /* esperado desde que a busca da sala comecou */

static uint64_t hd_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t hd_now_ms(void) {
    return (uint32_t)(hd_now_ns() / 1000000ull);
}

static void prefetch(uint32_t vs, uint32_t n);
static int hd_logo_vrom(uint32_t vrom);
static int wait_loaded(uint32_t i);

/* toda leitura da ROM (os_hw.c): anota a origem das paginas da RAM */
void port_rom_note(uint32_t off, void *ram, uint32_t n) {
    uint32_t lo = 0, hi = g_nfiles, r = (uint32_t)(uintptr_t)ram, p, p1;
    const CartFile *cf = NULL;
    if (!g_ready || n == 0 || r < RAM_BASE || r - RAM_BASE >= RAM_PAGES * 4096u) {
        return;
    }
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_files[mid].ce <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < g_nfiles && g_files[lo].cs <= off) {
        cf = &g_files[lo];
    }
    p = (r - RAM_BASE) >> 12;
    p1 = (r - RAM_BASE + n - 1) >> 12;
    if (p1 >= RAM_PAGES) {
        p1 = RAM_PAGES - 1;
    }
    for (; p <= p1; p++) {
        PageRec *pg = &g_page[p];
        const uint32_t pa = RAM_BASE + (p << 12);
        PageSlot ns;
        int k;
        ns.ram = r;
        ns.len = n;
        ns.vrom = cf != NULL ? cf->vs + (off - cf->cs) : 0;
        if (r <= pa && r + n >= pa + 4096u) {
            /* cobre a pagina toda: o que havia nela saiu */
            memset(pg, 0, sizeof *pg);
        } else {
            for (k = PAGE_SLOTS - 1; k > 0; k--) {
                pg->s[k] = pg->s[k - 1];
            }
        }
        pg->s[0] = ns;
    }
    if (cf != NULL) {
        prefetch(cf->vs + (off - cf->cs), n);
    }
}

static uint32_t vrom_of(const uint8_t *ptr) {
    uint32_t a = (uint32_t)(uintptr_t)ptr, p;
    const PageRec *pr;
    int k;
    if (a < RAM_BASE || a - RAM_BASE >= RAM_PAGES * 4096u) {
        return 0;
    }
    p = (a - RAM_BASE) >> 12;
    pr = &g_page[p];
    for (k = 0; k < PAGE_SLOTS; k++) {
        const PageSlot *sl = &pr->s[k];
        if (sl->len != 0 && a >= sl->ram && a - sl->ram < sl->len) {
            return sl->vrom ? sl->vrom + (a - sl->ram) : 0; /* a mais nova que cobre o endereco */
        }
    }
    return 0;
}

static uint32_t ent_bytes(const HdEnt *e) {
    return ((uint32_t)e->ow * e->oh * (4u << e->siz) + 7u) / 8u;
}

/* a textura do pacote que contem o endereco vrom (o inicio dela ou um ponto no meio) */
static const HdEnt *find(uint32_t vrom) {
    uint32_t lo = 0, hi = g_nent;
    const HdEnt *e;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_ent[mid].vrom <= vrom) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return NULL;
    }
    e = &g_ent[lo - 1];
    return vrom - e->vrom < ent_bytes(e) ? e : NULL;
}

static uint32_t g_nLoaded;
static uint32_t g_nEvict; /* liberadas para a busca de uma sala nova */
static uint32_t g_pxBytes; /* das prontas */
static void report(void) {
    if ((g_nApplied & (g_nApplied - 1)) == 0) {
        fprintf(stderr, "[texturas] novas em uso: %u (carregadas: %u, buscadas antes: %u; diferentes da original: %u, "
                        "tile diferente: %u, fora do pacote: %u, sem origem: %u, letras da fonte: %u; na memoria %u KB; liberadas para "
                "salas novas: %u)\n",
                g_nApplied, g_nLoaded, g_nPrefetch, g_nCrc, g_nShape, g_nMiss, g_nNoVrom, g_nFont, g_pxBytes >> 10, g_nEvict);
    }
}

/* ---- carregamento em segundo plano ----
 * Por textura do pacote: estado (0 nada, 1 na fila, 2 pronta, 3 falhou), a textura nova inteira ja
 * descomprimida (g_px), e se o CRC da original ja foi conferido. A thread so le, descomprime e publica;
 * quem libera as prontas (limite de memoria) e o desenho, que e quem as usa. */
static uint8_t *g_st;
static uint32_t **g_px;
static uint8_t *g_crcOk;
static uint32_t *g_used;
static uint32_t g_useStamp;
#define HD_FULL_BYTES (20u << 20)
#define QN 8192u
static uint32_t g_q[QN];
static uint32_t g_qh, g_qt;
/* fila de quem o desenho ja esta pedindo (passa na frente das buscadas antes, na leitura da ROM) */
static uint32_t g_qu[QN];
static uint32_t g_quh, g_qut;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qc = PTHREAD_COND_INITIALIZER;

/* busca antecipada com a memoria cheia: libera as prontas que nao sao usadas ha alguns segundos (as menos
 * usadas primeiro) ate caber need bytes. Thread do carregador. */
static void evict_idle(uint32_t need) {
    const uint32_t now = hd_now_ms();
    pthread_mutex_lock(&g_pm);
    while (__atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) + need > HD_FULL_BYTES) {
        uint32_t i, best = 0xFFFFFFFFu, bu = 0xFFFFFFFFu;
        for (i = 0; i < g_nent; i++) {
            if (__atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) == 2 && g_px[i] != NULL &&
                now - g_usedMs[i] > HD_EVICT_IDLE_MS && g_used[i] < bu) {
                bu = g_used[i];
                best = i;
            }
        }
        if (best == 0xFFFFFFFFu) {
            break;
        }
        __atomic_store_n(&g_st[best], 0, __ATOMIC_RELEASE);
        free(g_px[best]);
        g_px[best] = NULL;
        __atomic_sub_fetch(&g_pxBytes, g_ent[best].rawlen, __ATOMIC_RELAXED);
        g_nEvict++;
    }
    pthread_mutex_unlock(&g_pm);
}

static void *loader(void *arg) {
    (void)arg;
    pt_thread_name("texturas");
    /* prioridade minima: descomprimir uma textura grande (~130 ms) tomava o nucleo da auxiliar do desenho; agora
     * usa so a folga dos nucleos (na troca de cena, quando a espera vale, ha folga de sobra) */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    for (;;) {
        uint32_t i;
        const HdEnt *e;
        uint8_t *buf;
        uint32_t *px;
        pthread_mutex_lock(&g_qm);
        while (g_qh == g_qt && g_quh == g_qut) {
            pthread_cond_wait(&g_qc, &g_qm);
        }
        int urgent = g_quh != g_qut;
        if (urgent) {
            i = g_qu[g_quh % QN];
            g_quh++;
        } else {
            i = g_q[g_qh % QN];
            g_qh++;
        }
        pthread_mutex_unlock(&g_qm);
        if (__atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) != 1) {
            continue; /* (ja veio pela outra fila) */
        }
        if (!urgent && __atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) + g_ent[i].rawlen > HD_FULL_BYTES) {
            evict_idle(g_ent[i].rawlen);
        }
        if (!urgent && __atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) + g_ent[i].rawlen > HD_FULL_BYTES) {
            /* busca antecipada: so ate o limite de memoria (as que faltarem vem quando forem usadas) */
            uint8_t one = 1;
            __atomic_compare_exchange_n(&g_st[i], &one, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        e = &g_ent[i];
        buf = (uint8_t *)malloc(e->clen);
        px = (uint32_t *)malloc(e->rawlen);
        if (buf == NULL || px == NULL || pread(g_fd, buf, e->clen, e->off) != (ssize_t)e->clen ||
            port_inflate(buf, e->clen, (uint8_t *)px, e->rawlen) != 0) {
            free(buf);
            free(px);
            __atomic_store_n(&g_st[i], 3, __ATOMIC_RELEASE);
            continue;
        }
        free(buf);
        pthread_mutex_lock(&g_pm);
        g_px[i] = px;
        g_usedMs[i] = hd_now_ms(); /* (recem-chegada: nao e liberada logo em seguida) */
        pthread_mutex_unlock(&g_pm);
        __atomic_add_fetch(&g_pxBytes, e->rawlen, __ATOMIC_RELAXED);
        __atomic_store_n(&g_st[i], 2, __ATOMIC_RELEASE);
        g_nLoaded++;
    }
    return NULL;
}

/* pede a textura i ao carregador. urgent: o desenho precisa dela agora. Uma textura ja na fila comum
 * (buscada na leitura da ROM) que o desenho pede passa para a fila urgente. */
static void enqueue_to(uint32_t i, int urgent) {
    uint8_t zero = 0;
    const int fresh = __atomic_compare_exchange_n(&g_st[i], &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (!fresh && !(urgent && zero == 1)) {
        return; /* ja pronta, falhou, ou ja na fila */
    }
    pthread_mutex_lock(&g_qm);
    if (urgent && g_qut - g_quh < QN) {
        g_qu[g_qut % QN] = i;
        g_qut++;
    } else if (!urgent && g_qt - g_qh < QN) {
        g_q[g_qt % QN] = i;
        g_qt++;
    } else if (fresh) {
        g_st[i] = 0; /* fila cheia */
    }
    pthread_cond_signal(&g_qc);
    pthread_mutex_unlock(&g_qm);
}

static void enqueue(uint32_t i) {
    enqueue_to(i, 1);
}

static void prefetch(uint32_t vs, uint32_t n) {
    uint32_t lo = 0, hi = g_nent, i, cnt = 0;
    if (!gx_hd_use) {
        return;
    }
    while (lo < hi) { /* a 1a textura com vrom >= vs */
        uint32_t mid = (lo + hi) / 2;
        if (g_ent[mid].vrom < vs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (i = lo; i < g_nent && g_ent[i].vrom - vs < n; i++) {
        if (gx_hd_use == 2 && !hd_logo_vrom(g_ent[i].vrom)) {
            continue; /* (so a logo) */
        }
        if (__atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) == 0) {
            enqueue_to(i, 0);
            cnt++;
        }
    }
    if (cnt > 0) {
        const uint64_t now = hd_now_ns();
        if (now > __atomic_load_n(&g_freshUntil, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_winWaited, 0, __ATOMIC_RELEASE); /* sala nova: os 1os desenhos podem esperar mais */
        }
        __atomic_store_n(&g_freshUntil, now + HD_WAIT_WINDOW_NS, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_nPrefetch, cnt, __ATOMIC_RELAXED);
    }
}

/* desenho: logo depois de uma sala carregar, espera a textura nova chegar (com limite por desenho) */
static int wait_loaded(uint32_t i) {
    static uint32_t frameTask;
    static uint64_t frameWaited;
    uint64_t t0, cap, now = hd_now_ns();
    int st;
    extern int port_scene_transition __attribute__((weak));
    if (now > __atomic_load_n(&g_freshUntil, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    if (&port_scene_transition != NULL && !port_scene_transition) {
        /* so espera durante a troca de cena (tela escurecendo/clareando): com o jogo na tela, esperar ate 150 ms
         * por desenho era um engasgo (perf: 155 ms entrando numa casa). Fora dela, a original aparece ate a nova
         * chegar. */
        return 0;
    }
    if (frameTask != gx.taskNo) {
        frameTask = gx.taskNo;
        frameWaited = 0;
    }
    {
        const uint64_t ww = __atomic_load_n(&g_winWaited, __ATOMIC_ACQUIRE);
        cap = HD_WAIT_FRAME_NS;
        if (ww < HD_WAIT_FIRST_NS && HD_WAIT_FIRST_NS - ww > cap) {
            cap = HD_WAIT_FIRST_NS - ww;
        }
    }
    if (frameWaited >= cap) {
        return 0;
    }
    t0 = now;
    for (;;) {
        st = __atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE);
        if (st != 1) {
            break;
        }
        now = hd_now_ns();
        if (frameWaited + (now - t0) >= cap) {
            break;
        }
        {
            struct timespec ts = { 0, 300000 };
            nanosleep(&ts, NULL);
        }
    }
    frameWaited += hd_now_ns() - t0;
    __atomic_add_fetch(&g_winWaited, hd_now_ns() - t0, __ATOMIC_ACQ_REL);
    return st == 2;
}


/* libera texturas inteiras prontas (as menos usadas) ate caber no limite; nunca a "keep" */
static void trim(uint32_t keep) {
    pthread_mutex_lock(&g_pm);
    while (__atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) > HD_FULL_BYTES) {
        uint32_t i, best = 0xFFFFFFFFu, bu = 0xFFFFFFFFu;
        for (i = 0; i < g_nent; i++) {
            if (i != keep && __atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) == 2 && g_used[i] < bu) {
                bu = g_used[i];
                best = i;
            }
        }
        if (best == 0xFFFFFFFFu) {
            break;
        }
        free(g_px[best]);
        g_px[best] = NULL;
        __atomic_sub_fetch(&g_pxBytes, g_ent[best].rawlen, __ATOMIC_RELAXED);
        g_st[best] = 0;
    }
    pthread_mutex_unlock(&g_pm);
}

/* a parte da textura nova que corresponde a tx: o retangulo que comeca na linha row, coluna col (inteira,
 * uma faixa de linhas, ou um pedaco de uma textura grande carregado aos poucos) */
static GxTex *build(uint32_t i, const GxTex *tx, uint32_t row, uint32_t col) {
    const HdEnt *e = &g_ent[i];
    const int k = e->hw / e->ow;
    int y;
    GxTex *h = (GxTex *)calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }
    h->w = tx->w * k;
    h->h = tx->h * k;
    h->px = (uint32_t *)malloc((size_t)h->w * h->h * 4u);
    if (h->px == NULL) {
        free(h);
        return NULL;
    }
    pthread_mutex_lock(&g_pm);
    if (__atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) != 2 || g_px[i] == NULL) {
        /* (liberada pelo carregador agora ha pouco: fica a original por enquanto) */
        pthread_mutex_unlock(&g_pm);
        free(h->px);
        free(h);
        return NULL;
    }
    for (y = 0; y < h->h; y++) {
        memcpy(h->px + (size_t)y * h->w, g_px[i] + ((size_t)row * k + y) * e->hw + (size_t)col * k, (size_t)h->w * 4u);
    }
    g_usedMs[i] = hd_now_ms();
    pthread_mutex_unlock(&g_pm);
    if (tx->solid) {
        /* mesmo ajuste de alfa que tex_decode faz na original (gfx_rdp.c): menu opaco (1) e logo do escudo (2) */
        const size_t n = (size_t)h->w * (size_t)h->h;
        size_t q;
        for (q = 0; q < n; q++) {
            uint32_t p = h->px[q];
            if (tx->solid == 1) {
                if ((p >> 24) >= 16u) {
                    p |= 0xFF000000u;
                }
            } else {
                p = (p >> 24) >= 96u ? (p | 0xFF000000u) : (p & 0x00FFFFFFu);
            }
            h->px[q] = p;
        }
    }
    h->clipw = tx->clipw * k;
    h->cliph = tx->cliph * k;
    h->key = tx->key;
    h->hdk = k;
    h->hdTried = 1;
    g_used[i] = ++g_useStamp;
    g_nApplied++;
    report();
    /* a descomprimida fica guardada (ate o limite): se a textura sair da cache de texturas e voltar, e montada
     * na hora, sem ler o cartao de novo (senao a original aparece por algumas imagens: pisca) */
    trim(i);
    return h;
}

/* opcao hd_textures 2: texturas novas so onde a imagem e uma pintura chapada: a logo da abertura (escudo, fogo, letras
 * do PRESS START; marcas 0x54585443/44 em volta do EnMag_Draw inteiro, e 0x5458543C/D e E/F) e os fundos pre-renderizados desenhados como "caixa do ceu" (interior de
 * casas e lojas, becos da cidade do castelo...: vr_*VR_static; marcas 0x54585441/42 em z_vr_box_draw.c). Fora das
 * marcas nada muda no jogo: a origem da TMEM nem e anotada (gx_hd_on 0) e nenhuma textura e procurada no pacote.
 * Os ceus de verdade (vr_fine/cloud/holy) e o resto ficam os originais. A logo vai pelo acelerador 2D com os texels
 * da nova (2x: 320 de largura, 1:1 na tela de 640, sem o acelerador esticar), calculados uma vez (gfx_ge.c). */
/* (hd_textures 2) a logo saiu da tela ha alguns segundos (jogo comecado, abertura pulada): as texturas novas
 * dela, ja descomprimidas (~1,6 MB), saem da memoria. Se a abertura voltar, sao lidas do cartao de novo. */
static uint32_t g_logoMs;
static void hd_release_logo(void) {
    uint32_t i, n = 0, bytes = 0;
    if (__atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) == 0 || hd_now_ms() - g_logoMs < 5000u) {
        return;
    }
    pthread_mutex_lock(&g_pm);
    for (i = 0; i < g_nent; i++) {
        if (__atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE) == 2 && g_px[i] != NULL) {
            __atomic_store_n(&g_st[i], 0, __ATOMIC_RELEASE);
            free(g_px[i]);
            g_px[i] = NULL;
            __atomic_sub_fetch(&g_pxBytes, g_ent[i].rawlen, __ATOMIC_RELAXED);
            bytes += g_ent[i].rawlen;
            n++;
        }
    }
    pthread_mutex_unlock(&g_pm);
    if (n > 0) {
        fprintf(stderr, "[texturas] logo fora da tela: %u texturas novas liberadas (%u KB; na memoria agora %u KB)\n", n,
                bytes >> 10, __atomic_load_n(&g_pxBytes, __ATOMIC_RELAXED) >> 10);
    }
}

void gx_hd_logo_mark(int bit, int on) {
    static unsigned s_mask; /* (marcas abertas: 1 logo inteira, 2 escudo, 4 PRESS START, 8 fundos) */
    if (gx_hd_use == 2 && g_ready) {
        g_logoMs = hd_now_ms();
        if (bit == 8) {
            return; /* (fundos pre-renderizados: ficam os originais desde 03/11; so a logo) */
        }
        s_mask = on ? (s_mask | (unsigned)bit) : (s_mask & ~(unsigned)bit);
        gx_hd_on = s_mask != 0;
    }
}

extern char _object_magSegmentRomStart[], _object_magSegmentRomEnd[];
extern char _vr_MDVR_staticSegmentRomStart[], _vr_MDVR_staticSegmentRomEnd[], _vr_MDVR_pal_staticSegmentRomStart[], _vr_MDVR_pal_staticSegmentRomEnd[];
extern char _vr_MNVR_staticSegmentRomStart[], _vr_MNVR_staticSegmentRomEnd[], _vr_MNVR_pal_staticSegmentRomStart[], _vr_MNVR_pal_staticSegmentRomEnd[];
extern char _vr_RUVR_staticSegmentRomStart[], _vr_RUVR_staticSegmentRomEnd[], _vr_RUVR_pal_staticSegmentRomStart[], _vr_RUVR_pal_staticSegmentRomEnd[];
extern char _vr_LHVR_staticSegmentRomStart[], _vr_LHVR_staticSegmentRomEnd[], _vr_LHVR_pal_staticSegmentRomStart[], _vr_LHVR_pal_staticSegmentRomEnd[];
extern char _vr_KHVR_staticSegmentRomStart[], _vr_KHVR_staticSegmentRomEnd[], _vr_KHVR_pal_staticSegmentRomStart[], _vr_KHVR_pal_staticSegmentRomEnd[];
extern char _vr_K3VR_staticSegmentRomStart[], _vr_K3VR_staticSegmentRomEnd[], _vr_K3VR_pal_staticSegmentRomStart[], _vr_K3VR_pal_staticSegmentRomEnd[];
extern char _vr_K4VR_staticSegmentRomStart[], _vr_K4VR_staticSegmentRomEnd[], _vr_K4VR_pal_staticSegmentRomStart[], _vr_K4VR_pal_staticSegmentRomEnd[];
extern char _vr_K5VR_staticSegmentRomStart[], _vr_K5VR_staticSegmentRomEnd[], _vr_K5VR_pal_staticSegmentRomStart[], _vr_K5VR_pal_staticSegmentRomEnd[];
extern char _vr_SP1a_staticSegmentRomStart[], _vr_SP1a_staticSegmentRomEnd[], _vr_SP1a_pal_staticSegmentRomStart[], _vr_SP1a_pal_staticSegmentRomEnd[];
extern char _vr_MLVR_staticSegmentRomStart[], _vr_MLVR_staticSegmentRomEnd[], _vr_MLVR_pal_staticSegmentRomStart[], _vr_MLVR_pal_staticSegmentRomEnd[];
extern char _vr_KKRVR_staticSegmentRomStart[], _vr_KKRVR_staticSegmentRomEnd[], _vr_KKRVR_pal_staticSegmentRomStart[], _vr_KKRVR_pal_staticSegmentRomEnd[];
extern char _vr_KR3VR_staticSegmentRomStart[], _vr_KR3VR_staticSegmentRomEnd[], _vr_KR3VR_pal_staticSegmentRomStart[], _vr_KR3VR_pal_staticSegmentRomEnd[];
extern char _vr_IPVR_staticSegmentRomStart[], _vr_IPVR_staticSegmentRomEnd[], _vr_IPVR_pal_staticSegmentRomStart[], _vr_IPVR_pal_staticSegmentRomEnd[];
extern char _vr_KSVR_staticSegmentRomStart[], _vr_KSVR_staticSegmentRomEnd[], _vr_KSVR_pal_staticSegmentRomStart[], _vr_KSVR_pal_staticSegmentRomEnd[];
extern char _vr_GLVR_staticSegmentRomStart[], _vr_GLVR_staticSegmentRomEnd[], _vr_GLVR_pal_staticSegmentRomStart[], _vr_GLVR_pal_staticSegmentRomEnd[];
extern char _vr_ZRVR_staticSegmentRomStart[], _vr_ZRVR_staticSegmentRomEnd[], _vr_ZRVR_pal_staticSegmentRomStart[], _vr_ZRVR_pal_staticSegmentRomEnd[];
extern char _vr_DGVR_staticSegmentRomStart[], _vr_DGVR_staticSegmentRomEnd[], _vr_DGVR_pal_staticSegmentRomStart[], _vr_DGVR_pal_staticSegmentRomEnd[];
extern char _vr_ALVR_staticSegmentRomStart[], _vr_ALVR_staticSegmentRomEnd[], _vr_ALVR_pal_staticSegmentRomStart[], _vr_ALVR_pal_staticSegmentRomEnd[];
extern char _vr_NSVR_staticSegmentRomStart[], _vr_NSVR_staticSegmentRomEnd[], _vr_NSVR_pal_staticSegmentRomStart[], _vr_NSVR_pal_staticSegmentRomEnd[];
extern char _vr_LBVR_staticSegmentRomStart[], _vr_LBVR_staticSegmentRomEnd[], _vr_LBVR_pal_staticSegmentRomStart[], _vr_LBVR_pal_staticSegmentRomEnd[];
extern char _vr_TTVR_staticSegmentRomStart[], _vr_TTVR_staticSegmentRomEnd[], _vr_TTVR_pal_staticSegmentRomStart[], _vr_TTVR_pal_staticSegmentRomEnd[];
extern char _vr_FCVR_staticSegmentRomStart[], _vr_FCVR_staticSegmentRomEnd[], _vr_FCVR_pal_staticSegmentRomStart[], _vr_FCVR_pal_staticSegmentRomEnd[];

/* (hd_textures 2) a textura nova pode ser desta textura? */
static int hd_logo_vrom(uint32_t vrom) {
#define R(n) { n##SegmentRomStart, n##SegmentRomEnd }
    static const struct {
        const char *s, *e;
    } k_ok[] = {
    R(_object_mag), R(_nes_font_static),
    /* (os fundos pre-renderizados, vr_*VR_static, sairam em 03/11: so a logo da abertura usa texturas novas) */
    };
#undef R
    unsigned i;
    if (vrom == (uint32_t)(uintptr_t)_object_magSegmentRomStart + 0x19300u) {
        return 0; /* gTitleCopyright1998Tex (a linha "(C) 1998 Nintendo"): fica a original (pedido do Regis, 03/11) */
    }
    for (i = 0; i < sizeof k_ok / sizeof k_ok[0]; i++) {
        if (vrom >= (uint32_t)(uintptr_t)k_ok[i].s && vrom < (uint32_t)(uintptr_t)k_ok[i].e) {
            return 1;
        }
    }
    return 0;
}

/* tx acabou de ser decodificada da tile tl; devolve a textura nova (se ja estiver carregada) ou NULL. A carga
 * pode ter comecado no inicio da textura (inteira) ou no comeco de uma linha dela (faixa de uma textura
 * grande, maior que a TMEM). Se a nova ainda nao chegou, pede para a thread e marca tx->hdPend. */
GxTex *gx_hd_lookup(GxTex *tx, const GxTile *tl, const uint8_t *src, unsigned stride) {
    uint32_t vrom, obytes, off, rowBytes, row, col, colB, bits, i;
    const HdEnt *e;
    int k, st;
    if (g_ready && gx_hd_use == 2 && !gx_hd_on) {
        hd_release_logo(); /* (logo fora da tela ha 5 s: libera as dela) */
    }
    if (!g_ready || !gx_hd_use || src == NULL) {
        return NULL;
    }
    if (gx_hd_use == 2 && !gx_hd_on) {
        return NULL; /* (so a logo: fora das marcas dela) */
    }
    vrom = vrom_of(src);
    if (vrom == 0) {
        g_nNoVrom++;
        return NULL;
    }
    if (gx_hd_use == 2 && !hd_logo_vrom(vrom)) {
        return NULL;
    }
    if (vrom >= (uint32_t)(uintptr_t)_nes_font_staticSegmentRomStart &&
        vrom < (uint32_t)(uintptr_t)_nes_font_staticSegmentRomEnd && !gx_hd_font_ok) {
        g_nFont++;
        return NULL;
    }
    e = find(vrom);
    if (e == NULL) {
        g_nMiss++;
        return NULL;
    }
    i = (uint32_t)(e - g_ent);
    off = vrom - e->vrom;
    bits = 4u << e->siz;
    rowBytes = ((uint32_t)e->ow * bits + 7u) / 8u;
    row = rowBytes ? off / rowBytes : 0;
    colB = rowBytes ? off % rowBytes : 0;
    col = colB * 8u / bits;
    if (stride == 0) {
        stride = (unsigned)tl->line * 8u; /* carga linear: as linhas na TMEM sao as linhas da memoria */
    }
    if (e->fmt != tl->fmt || e->siz != tl->siz || e->ow == 0 || e->hw % e->ow || row + (uint32_t)tx->h > e->oh ||
        (colB * 8u) % bits != 0 || col + (uint32_t)tx->w > e->ow ||
        /* inteira na largura (como sempre) ou um retangulo lido com a mesma largura de linha da original */
        !((col == 0 && (uint32_t)tx->w == e->ow) || stride == rowBytes)) {
        static uint32_t seen[64];
        static int nseen;
        int s2;
        g_nShape++;
        /* diagnostico: as primeiras texturas (diferentes) recusadas, para entender as formas de carga */
        for (s2 = 0; s2 < nseen && seen[s2] != e->vrom; s2++) {
        }
        if (s2 == nseen && nseen < 64) {
            seen[nseen++] = e->vrom;
            fprintf(stderr, "[texturas] recusada %08x: pacote fmt%u/%u %ux%u | tile fmt%u/%u %dx%d (tile %dx%d, mascara %u/%u, "
                            "linha %u) | carga a partir do byte %u (linha %u coluna %u, passo %u)\n",
                    e->vrom, e->fmt, e->siz, e->ow, e->oh, tl->fmt, tl->siz, tx->w, tx->h, tx->clipw, tx->cliph, tl->masks,
                    tl->maskt, tl->line, off, row, col, stride);
        }
        return NULL;
    }
    k = e->hw / e->ow;
    if (k != 1 && k != 2 && k != 4) {
        g_nShape++;
        return NULL;
    }
    if (!g_crcOk[i]) {
        obytes = ent_bytes(e);
        {
            const uint32_t v0 = vrom_of(src - off), v1 = vrom_of(src - off + obytes - 1);
            const int why = v0 != e->vrom ? 1 : v1 == 0 ? 2 : crc32_of(src - off, obytes) != e->crc ? 3 : 0;
            if (why) {
                static uint32_t seenc[64];
                static int nseenc;
                int s3;
                g_nCrc++;
                /* diagnostico: quais texturas ficaram com a original e por que */
                for (s3 = 0; s3 < nseenc && seenc[s3] != e->vrom; s3++) {
                }
                if (s3 == nseenc && nseenc < 64) {
                    seenc[nseenc++] = e->vrom;
                    fprintf(stderr, "[texturas] diferente %08x (%ux%u fmt%u/%u, pedaco linha %u coluna %u): %s (inicio %08x, fim %08x)\n",
                            e->vrom, e->ow, e->oh, e->fmt, e->siz, row, col,
                            why == 1 ? "inicio em outro arquivo" : why == 2 ? "fim sem origem" : "conteudo diferente", v0, v1);
                }
                return NULL;
            }
        }
        g_crcOk[i] = 1;
    }
    st = __atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE);
    if (st == 2) {
        return build(i, tx, row, col);
    }
    if (st == 3) {
        return NULL;
    }
    if (st == 0 || st == 1) {
        enqueue(i); /* (uma que veio da busca antecipada passa para a fila urgente) */
    }
    if (wait_loaded(i)) {
        return build(i, tx, row, col);
    }
    tx->hdPend = (int)i + 1;
    tx->hdRow = (int)row;
    tx->hdCol = (int)col;
    return NULL;
}

/* chamado quando a textura e usada de novo: pendura a nova se ela ja chegou. 1 = pendurou */
int gx_hd_retry(GxTex *tx) {
    uint32_t i = (uint32_t)tx->hdPend - 1;
    int st;
    if (!g_ready || i >= g_nent) {
        tx->hdPend = 0;
        return 0;
    }
    st = __atomic_load_n(&g_st[i], __ATOMIC_ACQUIRE);
    if (st == 3) {
        tx->hdPend = 0;
        return 0;
    }
    if (st == 0) {
        enqueue(i); /* foi liberada antes de chegar a esta faixa: pede de novo */
        return 0;
    }
    if (st != 2) {
        return 0;
    }
    tx->hd = build(i, tx, (uint32_t)tx->hdRow, (uint32_t)tx->hdCol);
    tx->hdPend = 0;
    return tx->hd != NULL;
}

void gx_hd_free(GxTex *h) {
    if (h == NULL) {
        return;
    }
    free(h->px);
    free(h->pp);
    free(h->pf);
    free(h);
}
