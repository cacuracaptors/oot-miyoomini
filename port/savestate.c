/*
 * savestate.c -- estados do jogo: estado rapido (MENU+R2 / MENU+L2), Game Switcher do OnionOS, salvar ao
 * desligar e continuar de onde parou ao ligar. Feito como no port do Mario Kart 64 (savestate_miyoo.c).
 *
 * O ESTADO e uma copia de tudo o que o jogo (thread grafica, que roda a logica) guarda:
 *   - a RAM do N64 (4 MB em 0x80000000: heap do jogo, atores, cena, salas, objetos, framebuffers);
 *   - os dados e o BSS dos arquivos do jogo (secoes game_data / game_bss, separadas na compilacao pelo
 *     oot-build.sh; o ligador define __start_/__stop_ delas).
 * Ficam de fora (continuam como estao): as outras threads do jogo (som, escalonador, controles, DMA), os
 * buffers do desenho e tudo do port. A copia so e feita num ponto seguro: graph.c (Port_SsFrame), entre dois
 * quadros, com o desenho parado e nenhuma leitura da ROM em andamento; ao carregar, o som e acertado
 * (Port_SsAudioRestore em sequence.c). Salvar so durante o jogo (cena carregada, fora de troca de cena).
 *
 * ARQUIVOS (pasta dos saves, savedir.c: Saves/CurrentProfile/saves/ZeldaOoT no OnionOS, entao cada perfil -- Modo
 * Visitante, Kids Mode... -- tem os seus; ./saves fora dele):
 *   state_quick.oss   estado rapido (MENU+R2 salva, MENU+L2 carrega); gravado por uma thread separada (o
 *                     jogo nao espera o cartao) e guardado tambem na memoria (carregar na sessao e imediato)
 *   state_auto.oss    estado de quando o jogo fechou (MENU, desligar, bateria): o Game Switcher continua dele
 *   resume_boot       o aparelho desligou com o jogo aberto: ao ligar, continua de state_auto.oss
 *   state_loading     uma retomada em andamento; se o jogo travar nela, a proxima abertura comeca do inicio
 * Paginas de 4 KB so com zeros nao sao gravadas (um mapa de bits diz quais estao no arquivo). Um estado so vale
 * para a mesma compilacao (data/hora + enderecos das secoes): um de outra versao e recusado.
 *
 * ONIONOS (como o keymon faz com o RetroArch):
 *   MENU sozinho: toque, segurar (0,7 s) e dois toques (em 0,3 s) fazem o que o Tweaks > Shortcuts manda para
 *     os jogos (config/keymap.json; padrao: Game Switcher, voltar ao menu, jogo anterior). Antes, o estado
 *     state_auto.oss e a imagem que o Game Switcher mostra (romScreens/<hash>.png) sao gravados.
 *   Aberto pelo Game Switcher (/tmp/prev_state = "switcher"): continua de state_auto.oss; aberto pela lista
 *     de jogos: comeca do inicio.
 *   Desligar (o OnionOS cria /tmp/.offOrder e manda SIGTERM; o sinal nem sempre chega, entao o arquivo e
 *     olhado 4 vezes por segundo) e bateria no fim: salva o estado, deixa resume_boot e devolve o
 *     cmd_to_run.sh (que o keymon apaga): o OnionOS abre o jogo de novo ao ligar e ele continua de onde estava.
 *     Um script em .tmp_update/checkoff faz o OnionOS esperar o jogo salvar (20 s no maximo).
 *   Fora do jogo propriamente dito (abertura, tela de arquivos) nao ha estado: o jogo fecha sem ele e o
 *     state_auto.oss antigo e apagado (o Game Switcher nao volta para um ponto velho).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "savedir.h"

extern char __start_game_data[];
extern char __stop_game_data[];
extern char __start_game_bss[];
extern char __stop_game_bss[];
extern uint32_t osMemSize;
extern int port_opt_idioma;

/* port/miyoo (fracos: o teste no PC nao tem) */
void miyoo_video_toast(const char* text) __attribute__((weak));
int miyoo_video_front(const uint32_t** page) __attribute__((weak)); /* pagina na tela, 640x480 girada 180 */
void miyoo_quit(int code) __attribute__((weak));
int miyoo_video_thumb(uint16_t* out) __attribute__((weak));   /* 256x192: a imagem do jogo quando o menu abriu */
void miyoo_menu_status(const char* text) __attribute__((weak)); /* aviso dentro do menu de opcoes */
void miyoo_menu_slots_changed(void) __attribute__((weak));
void miyoo_menu_close_now(void) __attribute__((weak));
const char* miyoo_menu_jp(const char* en) __attribute__((weak)); /* o texto em japones (NULL = nao ha) */
const char* miyoo_menu_eu(const char* en, int lg) __attribute__((weak)); /* em alemao (3) ou frances (4) */
void miyoo_restart(void) __attribute__((weak));                 /* port_main.c: abre o jogo de novo (nao volta) */
extern volatile int miyoo_battery_exit __attribute__((weak));

/* a pasta dos saves e os arquivos nela (savedir.c: a do perfil do OnionOS, ou ./saves), acertados no port_ss_init antes de
 * qualquer outro uso */
static char g_saveDir[PORT_PATH_MAX] = "saves";
static char g_quickState[PORT_PATH_MAX] = "saves/state_quick.oss";
static char g_autoState[PORT_PATH_MAX] = "saves/state_auto.oss";
static char g_bootResume[PORT_PATH_MAX] = "saves/resume_boot";
static char g_loadingFlag[PORT_PATH_MAX] = "saves/state_loading";
#define ONION_DIR "/mnt/SDCARD/.tmp_update"
#define CMD_TO_RUN ONION_DIR "/cmd_to_run.sh"
#define RUN_SWITCHER ONION_DIR "/.runGameSwitcher"
#define CHECKOFF_DIR ONION_DIR "/checkoff"
#define CHECKOFF_SCRIPT CHECKOFF_DIR "/zeldaoot_save.sh"
#define ROMSCREEN_DIR "/mnt/SDCARD/Saves/CurrentProfile/romScreens"
#define RECENT_LIST "/mnt/SDCARD/Roms/recentlist.json"
#define RECENT_HIDDEN "/mnt/SDCARD/Roms/recentlist-hidden.json"
#define KEYMAP ONION_DIR "/config/keymap.json"
#define NSLOTS 10
#define THUMB_W 256 /* (miniaturas antigas, de 160x120, sao ampliadas ao ler) */
#define THUMB_H 192

#define SS_MAGIC 0x3353534Fu /* "OSS3" */
#define RAM_BASE 0x80000000u
#define PAGE 4096u
#define LOADED_OK_FRAMES 300 /* ~10 s depois de uma retomada: ela nao travou o jogo */

/* pedidos para o jogo (graph.c: Port_SsFrame) */
enum { OP_NONE, OP_SAVE_QUICK, OP_LOAD_QUICK, OP_EXIT, OP_LOAD_AUTO, OP_SAVE_SLOT, OP_LOAD_SLOT };
/* jeitos de fechar */
enum { EXIT_QUIT, EXIT_SWITCHER, EXIT_POWEROFF, EXIT_BATTERY, EXIT_QUICK_SWITCH, EXIT_RESTART, EXIT_RESET };
/* acoes do MENU no OnionOS (keymon) */
enum { MENU_OFF, MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH, MENU_RA_MENU };
enum { MENU_SINGLE, MENU_LONG, MENU_DOUBLE };

typedef struct {
    uint32_t magic;
    uint32_t build;  /* identifica a compilacao */
    uint32_t ram;    /* bytes de RAM */
    uint32_t dataAddr, dataLen, bssAddr, bssLen;
    uint16_t seqMain, seqSub; /* musica tocando quando o estado foi salvo */
    uint32_t payload; /* bytes da copia (RAM + dados + BSS, completados ate a pagina) */
    uint32_t pages;   /* paginas no arquivo (as outras sao zeros) */
    uint32_t pbgW, pbgH, pbgLv, pbgLen; /* fundo do menu de pausa em resolucao maior (gfx_hires.c), depois da copia */
} SsHdr;

/* gfx_hires.c (fracos: o teste no PC tambem os tem, mas assim o modulo fica independente) */
int gx_hr_pbg_get(const void** px, int* w, int* h, int* lv) __attribute__((weak));
void gx_hr_pbg_set(const void* px, int w, int h, int lv) __attribute__((weak));

volatile int port_ss_req; /* 1 = salvar, 2 = carregar o estado rapido (botoes) */

static volatile sig_atomic_t g_offSignal;
static volatile int g_loopRunning;
static volatile int g_exitKind = -1;  /* fechar pedido (botoes, desligar, bateria) */
static double g_exitAt;               /* quando foi pedido (o jogo nao atendendo: fecha mesmo assim) */
static volatile int g_menuPress = -1; /* MENU_SINGLE/LONG/DOUBLE: atendido no proximo quadro */
static int g_menuAction[3] = { MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH };
static int g_onion;
static char g_cmd[4096]; /* cmd_to_run.sh como estava na abertura */
static int g_cmdLen = -1;
static int g_offAtStart;
static int g_checkoff;
static double g_nextOffPoll;
static char g_romscreen[192];
static int g_bootLoad;        /* continuar de state_auto.oss assim que o jogo estiver de pe */
static unsigned g_frames, g_loadedAt, g_wait;
static int g_waitOp;
static volatile int g_exiting; /* um caminho de saida ja comecou (botoes x jogo) */
static volatile int g_slotOp, g_slot; /* aba Estados do menu: OP_SAVE_SLOT / OP_LOAD_SLOT no estado g_slot (0..9) */

static uint8_t* g_buf; /* ultimo estado rapido: cabecalho + copia (completa, com os zeros) */
static size_t g_bufLen;
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
static int g_writePending, g_writerOn;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int exists(const char* path) {
    return access(path, F_OK) == 0;
}

/* um texto fixo no idioma escolhido */
static const char* lang_text(const char* en, const char* pt) {
    if (port_opt_idioma == 2 && miyoo_menu_jp != NULL) {
        const char* jp = miyoo_menu_jp(en);
        return jp != NULL ? jp : en;
    }
    if (port_opt_idioma >= 3 && port_opt_idioma <= 5 && miyoo_menu_eu != NULL) {
        const char* eu = miyoo_menu_eu(en, port_opt_idioma);
        return eu != NULL ? eu : en;
    }
    return port_opt_idioma == 1 ? pt : en;
}

static void toast(const char* en, const char* pt) {
    const char* s = lang_text(en, pt);
    fprintf(stderr, "[estado] %s\n", s);
    if (miyoo_video_toast != NULL) {
        miyoo_video_toast(s);
    }
    if (miyoo_menu_status != NULL) {
        miyoo_menu_status(s);
    }
}

static int begin_exit(void) {
    return !__atomic_exchange_n(&g_exiting, 1, __ATOMIC_SEQ_CST);
}

static void restart_now(void) {
    if (miyoo_restart != NULL) {
        miyoo_restart(); /* (nao volta) */
    }
    _exit(0);
}

static void quit_now(void) {
    if (miyoo_quit != NULL) {
        miyoo_quit(0);
    }
    _exit(0);
}

/* ---------------------------------------------------------------- arquivos */

static void fsync_path(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* o arquivo inteiro de uma vez: ao lado do antigo, depois renomeado por cima */
static int write_file(const char* path, const void* data, size_t len) {
    char tmp[300];
    const uint8_t* p = data;
    int fd;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return 0;
    }
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) {
            close(fd);
            remove(tmp);
            return 0;
        }
        p += w;
        len -= (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    fsync_path(path);
    return 1;
}

static void touch(const char* path) {
    write_file(path, "", 0);
}

/* ---------------------------------------------------------------- a copia */

static uint32_t build_id(void) {
    const char* s = __DATE__ " " __TIME__;
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    h = (h ^ (uint32_t)(uintptr_t)__start_game_data) * 16777619u;
    h = (h ^ (uint32_t)(__stop_game_data - __start_game_data)) * 16777619u;
    h = (h ^ (uint32_t)(uintptr_t)__start_game_bss) * 16777619u;
    h = (h ^ (uint32_t)(__stop_game_bss - __start_game_bss)) * 16777619u;
    return h;
}

static void fill_hdr(SsHdr* h, uint16_t seqMain, uint16_t seqSub) {
    memset(h, 0, sizeof *h);
    h->magic = SS_MAGIC;
    h->build = build_id();
    h->ram = osMemSize;
    h->dataAddr = (uint32_t)(uintptr_t)__start_game_data;
    h->dataLen = (uint32_t)(__stop_game_data - __start_game_data);
    h->bssAddr = (uint32_t)(uintptr_t)__start_game_bss;
    h->bssLen = (uint32_t)(__stop_game_bss - __start_game_bss);
    h->seqMain = seqMain;
    h->seqSub = seqSub;
    h->payload = (h->ram + h->dataLen + h->bssLen + PAGE - 1) & ~(PAGE - 1);
}

static int hdr_ok(const SsHdr* h) {
    SsHdr cur;
    fill_hdr(&cur, 0, 0);
    return h->magic == SS_MAGIC && h->build == cur.build && h->ram == cur.ram && h->dataAddr == cur.dataAddr &&
           h->dataLen == cur.dataLen && h->bssAddr == cur.bssAddr && h->bssLen == cur.bssLen &&
           h->payload == cur.payload;
}

int port_ss_available(void) {
    return (uintptr_t)__stop_game_bss > (uintptr_t)__start_game_bss &&
           (uintptr_t)__stop_game_data >= (uintptr_t)__start_game_data;
}

/* a memoria do jogo -> buf (cabecalho + copia) */
static uint8_t* snapshot(uint16_t seqMain, uint16_t seqSub, uint8_t* buf, size_t* len) {
    SsHdr h;
    uint8_t* p;
    const void* pbg = NULL;
    int pw = 0, ph = 0, plv = 0;
    fill_hdr(&h, seqMain, seqSub);
    if (gx_hr_pbg_get != NULL && gx_hr_pbg_get(&pbg, &pw, &ph, &plv)) {
        h.pbgW = (uint32_t)pw;
        h.pbgH = (uint32_t)ph;
        h.pbgLv = (uint32_t)plv;
        h.pbgLen = (uint32_t)(pw * ph * 2);
    }
    if (buf != NULL && *len != sizeof h + h.payload + h.pbgLen) {
        free(buf);
        buf = NULL;
    }
    *len = sizeof h + h.payload + h.pbgLen;
    if (buf == NULL) {
        buf = malloc(*len);
        if (buf == NULL) {
            return NULL;
        }
    }
    memcpy(buf, &h, sizeof h);
    p = buf + sizeof h;
    memcpy(p, (const void*)(uintptr_t)RAM_BASE, h.ram);
    p += h.ram;
    memcpy(p, __start_game_data, h.dataLen);
    p += h.dataLen;
    memcpy(p, __start_game_bss, h.bssLen);
    p += h.bssLen;
    memset(p, 0, (size_t)(buf + sizeof h + h.payload - p));
    if (h.pbgLen) {
        memcpy(buf + sizeof h + h.payload, pbg, h.pbgLen);
    }
    return buf;
}

/* buf -> a memoria do jogo */
static void restore(const uint8_t* buf) {
    SsHdr h;
    const uint8_t* p = buf + sizeof h;
    memcpy(&h, buf, sizeof h);
    memcpy((void*)(uintptr_t)RAM_BASE, p, h.ram);
    p += h.ram;
    memcpy(__start_game_data, p, h.dataLen);
    p += h.dataLen;
    memcpy(__start_game_bss, p, h.bssLen);
    if (gx_hr_pbg_set != NULL) { /* o fundo da pausa do estado (ou nenhum: o de agora seria de outro momento) */
        gx_hr_pbg_set(h.pbgLen ? buf + sizeof h + h.payload : NULL, (int)h.pbgW, (int)h.pbgH, (int)h.pbgLv);
    }
}

static int page_zero(const uint32_t* p) {
    unsigned i;
    for (i = 0; i < PAGE / 4; i += 8) {
        if (p[i] | p[i + 1] | p[i + 2] | p[i + 3] | p[i + 4] | p[i + 5] | p[i + 6] | p[i + 7]) {
            return 0;
        }
    }
    return 1;
}

/* buf (cabecalho + copia completa) -> arquivo: cabecalho, mapa de bits, paginas que nao sao so zeros */
static int write_state(const char* path, const uint8_t* buf) {
    SsHdr h;
    const uint8_t* pay = buf + sizeof h;
    size_t np, nbm, i;
    uint8_t* bm;
    char tmp[300];
    FILE* f;
    int ok = 0;
    double t0 = now_ms();
    memcpy(&h, buf, sizeof h);
    np = h.payload / PAGE;
    nbm = (np + 7) / 8;
    bm = calloc(nbm, 1);
    if (bm == NULL) {
        return 0;
    }
    h.pages = 0;
    for (i = 0; i < np; i++) {
        if (!page_zero((const uint32_t*)(pay + i * PAGE))) {
            bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            h.pages++;
        }
    }
    mkdir(g_saveDir, 0777);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f != NULL && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(bm, 1, nbm, f) == nbm) {
        ok = 1;
        for (i = 0; i < np && ok;) {
            size_t j = i;
            if (!(bm[i >> 3] & (1u << (i & 7)))) {
                i++;
                continue;
            }
            while (j < np && (bm[j >> 3] & (1u << (j & 7)))) {
                j++;
            }
            if (fwrite(pay + i * PAGE, 1, (j - i) * PAGE, f) != (j - i) * PAGE) {
                ok = 0;
            }
            i = j;
        }
        if (ok && h.pbgLen && fwrite(pay + h.payload, 1, h.pbgLen, f) != h.pbgLen) {
            ok = 0;
        }
        if (ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
            ok = 0;
        }
    }
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    if (ok && rename(tmp, path) == 0) {
        fsync_path(path);
        fsync_path(g_saveDir);
    } else {
        ok = 0;
        remove(tmp);
    }
    free(bm);
    if (ok) {
        fprintf(stderr, "[estado] gravado %s: %u de %u paginas (%u KB), %.0f ms\n", path, h.pages, (unsigned)np,
                h.pages * 4u, now_ms() - t0);
    } else {
        fprintf(stderr, "[estado] FALHOU ao gravar %s (%s); o anterior ficou como estava\n", path, strerror(errno));
    }
    return ok;
}

/* arquivo -> buf novo (cabecalho + copia completa). 1 = ok; 0 = sem arquivo; -1 = de outra versao/estragado */
static int read_state(const char* path, uint8_t** out, size_t* outLen) {
    FILE* f = fopen(path, "rb");
    SsHdr h;
    size_t np, nbm, i;
    uint8_t *bm = NULL, *buf = NULL;
    int r = -1;
    if (f == NULL) {
        return 0;
    }
    if (fread(&h, sizeof h, 1, f) != 1 || !hdr_ok(&h)) {
        fclose(f);
        return -1;
    }
    if (h.pbgLen != h.pbgW * h.pbgH * 2u || h.pbgLen > 640u * 480u * 2u) {
        fclose(f);
        return -1;
    }
    np = h.payload / PAGE;
    nbm = (np + 7) / 8;
    bm = malloc(nbm);
    buf = malloc(sizeof h + h.payload + h.pbgLen);
    if (bm != NULL && buf != NULL && fread(bm, 1, nbm, f) == nbm) {
        uint8_t* pay = buf + sizeof h;
        unsigned got = 0;
        r = 1;
        memcpy(buf, &h, sizeof h);
        for (i = 0; i < np && r == 1; i++) {
            if (bm[i >> 3] & (1u << (i & 7))) {
                if (fread(pay + i * PAGE, 1, PAGE, f) != PAGE) {
                    r = -1;
                }
                got++;
            } else {
                memset(pay + i * PAGE, 0, PAGE);
            }
        }
        if (got != h.pages) {
            r = -1;
        }
        if (r == 1 && h.pbgLen && fread(pay + h.payload, 1, h.pbgLen, f) != h.pbgLen) {
            r = -1;
        }
    }
    fclose(f);
    free(bm);
    if (r != 1) {
        free(buf);
        return -1;
    }
    *out = buf;
    *outLen = sizeof h + h.payload + h.pbgLen;
    return 1;
}

static int state_usable(const char* path) {
    FILE* f = fopen(path, "rb");
    SsHdr h;
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = fread(&h, sizeof h, 1, f) == 1 && hdr_ok(&h);
    fclose(f);
    return ok;
}

static void* writer_main(void* unused) {
    (void)unused;
    pthread_mutex_lock(&g_m);
    for (;;) {
        while (!g_writePending) {
            pthread_cond_wait(&g_c, &g_m);
        }
        g_writePending = 0;
        write_state(g_quickState, g_buf); /* (com o trinco: um novo estado espera este chegar ao cartao) */
    }
    return NULL;
}

/* ---------------------------------------------------------------- imagem do Game Switcher (PNG) */

static uint32_t crc_tab[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t* p, size_t n) {
    uint32_t i;
    int k;
    if (!crc_tab[1]) {
        for (i = 0; i < 256; i++) {
            uint32_t c = i;
            for (k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_tab[i] = c;
        }
    }
    crc = ~crc;
    while (n--) {
        crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static uint8_t* put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

/* PNG RGB sem compressao (blocos "stored" do deflate): ~0,9 MB, gravado uma vez ao fechar */
static int write_png_page(const char* path, const uint32_t* page, int w, int h) {
    const size_t stride = (size_t)w * 3 + 1, n = stride * (size_t)h;
    const size_t nblk = (n + 65534) / 65535;
    const size_t zlen = 2 + n + nblk * 5 + 4;
    uint8_t *raw = malloc(n), *file = malloc(zlen + 64);
    uint8_t *p, *c;
    uint32_t a = 1, b = 0;
    size_t i, off;
    int x, y, ok = 0;
    if (raw == NULL || file == NULL) {
        free(raw);
        free(file);
        return 0;
    }
    for (y = 0; y < h; y++) { /* a pagina esta de cabeca para baixo (painel girado 180) */
        uint8_t* o = raw + (size_t)y * stride;
        const uint32_t* s = page + (size_t)(h - 1 - y) * w;
        *o++ = 0;
        for (x = 0; x < w; x++) {
            uint32_t px = s[w - 1 - x];
            *o++ = (uint8_t)(px >> 16);
            *o++ = (uint8_t)(px >> 8);
            *o++ = (uint8_t)px;
        }
    }
    for (i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    p = file;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    p += 8;
    c = p;
    p = put32(p, 13);
    memcpy(p, "IHDR", 4);
    p += 4;
    p = put32(p, (uint32_t)w);
    p = put32(p, (uint32_t)h);
    *p++ = 8;
    *p++ = 2;
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    p = put32(p, crc32_of(0, c + 4, 17));
    c = p;
    p = put32(p, (uint32_t)zlen);
    memcpy(p, "IDAT", 4);
    p += 4;
    *p++ = 0x78;
    *p++ = 0x01;
    for (off = 0; off < n;) {
        size_t len = n - off > 65535 ? 65535 : n - off;
        *p++ = off + len >= n ? 1 : 0;
        *p++ = (uint8_t)len;
        *p++ = (uint8_t)(len >> 8);
        *p++ = (uint8_t)~len;
        *p++ = (uint8_t)(~len >> 8);
        memcpy(p, raw + off, len);
        p += len;
        off += len;
    }
    p = put32(p, (b << 16) | a);
    p = put32(p, crc32_of(0, c + 4, zlen + 4));
    c = p;
    p = put32(p, 0);
    memcpy(p, "IEND", 4);
    p += 4;
    p = put32(p, crc32_of(0, c + 4, 4));
    ok = write_file(path, file, (size_t)(p - file));
    free(raw);
    free(file);
    return ok;
}

static void write_romscreen(void) {
    const uint32_t* page;
    double t0 = now_ms();
    int ok;
    if (!g_romscreen[0] || miyoo_video_front == NULL || !miyoo_video_front(&page)) {
        return;
    }
    mkdir(ROMSCREEN_DIR, 0777);
    ok = write_png_page(g_romscreen, page, 640, 480);
    fprintf(stderr, "[estado] imagem do Game Switcher %s (%.0f ms)\n", ok ? "gravada" : "FALHOU", now_ms() - t0);
}

/* FNV1A_Pippip_Yurii do OnionOS (src/common/utils/hash.h) */
static uint32_t onion_hash(const char* str, size_t len) {
    const uint32_t prime = 591798841u;
    uint64_t h = 14695981039346656037ULL;
    uint32_t h32;
    if (len > 8) {
        size_t cycles = ((len - 1) >> 4) + 1;
        const size_t ndhead = len - (cycles << 3);
        for (; cycles--; str += 8) {
            uint64_t a, b;
            memcpy(&a, str, 8);
            memcpy(&b, str + ndhead, 8);
            h = (h ^ a) * prime;
            h = (h ^ b) * prime;
        }
    } else {
        uint64_t a = 0;
        memcpy(&a, str, len);
        h = (h ^ a) * prime;
    }
    h32 = (uint32_t)(h ^ (h >> 32));
    return h32 ^ (h32 >> 16);
}

/* a linha do jogo na lista de recentes do OnionOS: o caminho de que o Game Switcher tira o hash */
static void find_romscreen(void) {
    static char line[8192];
    char cwd[256], path[512];
    const char* dir;
    FILE* f;
    if (!getcwd(cwd, sizeof cwd)) {
        return;
    }
    dir = strrchr(cwd, '/');
    dir = dir ? dir + 1 : cwd; /* a pasta do jogo: ZeldaOoT */
    f = fopen(exists(RECENT_HIDDEN) ? RECENT_HIDDEN : RECENT_LIST, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        const char* t = strstr(line, "\"type\":");
        const char* r = strstr(line, "\"rompath\":\"");
        const char *e, *colon, *rom;
        int type, ours;
        if (!t || !r) {
            continue;
        }
        type = atoi(t + 7);
        if (type != 5 && type != 17) {
            continue;
        }
        r += 11;
        e = strchr(r, '"');
        if (!e || e - r >= (int)sizeof path) {
            continue;
        }
        memcpy(path, r, (size_t)(e - r));
        path[e - r] = 0;
        colon = strchr(path, ':'); /* "launch:rompath" (a busca) */
        rom = colon ? colon + 1 : path;
        ours = strstr(rom, dir) != NULL;
        if (!ours) { /* o atalho .port diz a pasta (GameDir="ZeldaOoT") */
            FILE* s = fopen(rom, "r");
            if (s) {
                char buf[4096];
                size_t n = fread(buf, 1, sizeof buf - 1, s);
                buf[n] = 0;
                fclose(s);
                ours = strstr(buf, dir) != NULL;
            }
        }
        if (ours) {
            snprintf(g_romscreen, sizeof g_romscreen, ROMSCREEN_DIR "/%u.png", (unsigned)onion_hash(rom, strlen(rom)));
            fprintf(stderr, "[estado] na lista de recentes como %s -> %s\n", rom, g_romscreen);
            break;
        }
    }
    fclose(f);
}

static int json_int(const char* json, const char* key, int fallback) {
    char k[64];
    const char* p;
    snprintf(k, sizeof k, "\"%s\"", key);
    p = strstr(json, k);
    if (!p || !(p = strchr(p + strlen(k), ':'))) {
        return fallback;
    }
    return atoi(p + 1);
}

/* o que o MENU faz num jogo (keymon: _settings_load_keymap e as marcas antigas) */
static void read_menu_settings(void) {
    FILE* f = fopen(KEYMAP, "r");
    if (f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        g_menuAction[MENU_SINGLE] = json_int(buf, "ingame_single_press", g_menuAction[MENU_SINGLE]);
        g_menuAction[MENU_LONG] = json_int(buf, "ingame_long_press", g_menuAction[MENU_LONG]);
        g_menuAction[MENU_DOUBLE] = json_int(buf, "ingame_double_press", g_menuAction[MENU_DOUBLE]);
    }
    if (exists(ONION_DIR "/config/.menuInverted")) {
        g_menuAction[MENU_SINGLE] = MENU_EXIT;
        g_menuAction[MENU_LONG] = MENU_SWITCHER;
    }
    if (exists(ONION_DIR "/config/.noGameSwitcher")) {
        g_menuAction[MENU_SINGLE] = MENU_EXIT;
        g_menuAction[MENU_LONG] = MENU_OFF;
    }
    fprintf(stderr, "[estado] MENU (Tweaks): toque %d, segurar %d, dois toques %d (1 Game Switcher, 2 menu, 3 jogo anterior)\n",
            g_menuAction[MENU_SINGLE], g_menuAction[MENU_LONG], g_menuAction[MENU_DOUBLE]);
}

/* o jogo anterior, como o quick switch do keymon (resumeGame(1)): o 2o jogo da lista de recentes vai para o
 * topo, o comando dele vai para o cmd_to_run.sh e /tmp/quick_switch faz o runtime.sh abri-lo. 0 = nao ha. */
static int quick_switch_prepare(void) {
    static char lines[64][2048];
    static char out[64 * 2048];
    const char* list = exists(RECENT_HIDDEN) ? RECENT_HIDDEN : RECENT_LIST;
    FILE* f = fopen(list, "r");
    int n = 0, pick = -1, valid = 0, complete, i;
    char cmd[2200] = "";
    if (f == NULL) {
        return 0;
    }
    while (n < 64 && fgets(lines[n], sizeof lines[n], f)) {
        const char* line = lines[n++];
        const char *t, *r, *e;
        char rom[1024], launch[1024] = "";
        char* colon;
        if (pick >= 0) {
            continue;
        }
        t = strstr(line, "\"type\":");
        r = strstr(line, "\"rompath\":\"");
        if (!t || !r) {
            continue;
        }
        i = atoi(t + 7);
        if (i != 5 && i != 17) {
            continue;
        }
        r += 11;
        e = strchr(r, '"');
        if (!e || e - r >= (int)sizeof rom) {
            continue;
        }
        memcpy(rom, r, (size_t)(e - r));
        rom[e - r] = 0;
        colon = strchr(rom, ':');
        if (colon) {
            *colon = 0;
            snprintf(launch, sizeof launch, "%s", rom);
            memmove(rom, colon + 1, strlen(colon + 1) + 1);
        } else {
            const char* l = strstr(line, "\"launch\":\"");
            const char* le = l ? strchr(l + 10, '"') : NULL;
            if (le && le - (l + 10) < (int)sizeof launch) {
                memcpy(launch, l + 10, (size_t)(le - (l + 10)));
                launch[le - (l + 10)] = 0;
            }
        }
        if (!exists(rom) || !launch[0] || !exists(launch)) {
            continue;
        }
        if (valid++ == 1) {
            pick = n - 1;
            snprintf(cmd, sizeof cmd, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, rom);
        }
    }
    complete = feof(f);
    fclose(f);
    if (pick < 0) {
        return 0;
    }
    if (pick > 0 && complete) {
        size_t len = 0;
        for (i = -1; i < n; i++) {
            const int k = i < 0 ? pick : i;
            size_t l;
            if (i == pick) {
                continue;
            }
            l = strlen(lines[k]);
            memcpy(out + len, lines[k], l);
            len += l;
        }
        write_file(list, out, len);
    }
    if (!write_file(CMD_TO_RUN, cmd, strlen(cmd))) {
        return 0;
    }
    touch("/tmp/quick_switch");
    touch("/tmp/force_auto_load_state");
    fprintf(stderr, "[estado] jogo anterior: %s\n", cmd);
    return 1;
}

static void put_back_cmd(void) {
    if (g_cmdLen > 0 && !exists(CMD_TO_RUN)) {
        int ok = write_file(CMD_TO_RUN, g_cmd, (size_t)g_cmdLen);
        fprintf(stderr, "[estado] cmd_to_run.sh devolvido (%s)\n", ok ? "ok" : "FALHOU");
    }
}

static void make_checkoff_script(const char* argv0) {
    const char* exe = strrchr(argv0, '/');
    char s[512];
    int n;
    exe = exe ? exe + 1 : argv0;
    mkdir(CHECKOFF_DIR, 0777);
    n = snprintf(s, sizeof s,
                 "#!/bin/sh\n"
                 "# Zelda OoT: o OnionOS espera aqui (20 s no maximo) enquanto o jogo salva ao desligar\n"
                 "i=0\n"
                 "while grep -q '%s' /proc/%d/cmdline 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done\n",
                 exe, (int)getpid());
    g_checkoff = write_file(CHECKOFF_SCRIPT, s, (size_t)n);
}

/* (pode ser chamado de um tratador de sinal: so unlink) */
void port_ss_release(void) {
    if (g_checkoff) {
        unlink(CHECKOFF_SCRIPT);
    }
}

/* ha um pedido de fechar (o menu de opcoes congelado sai do caminho: graph.c) */
int port_ss_exit_pending(void) {
    return g_exitKind >= 0 || g_offSignal;
}

/* salvamento automatico (z_play.c, Port_Autosave) */
void port_ss_autosave_toast(void) {
    toast("Game autosaved", "Jogo salvo automaticamente");
}

/* ---------------------------------------------------------------- abertura */

void port_ss_init(const char* argv0) {
    char prev[32] = "";
    FILE* f;
    int boot, switcher;
    char tmp[PORT_PATH_MAX + 8];
    g_onion = exists(ONION_DIR);
    snprintf(g_saveDir, sizeof g_saveDir, "%s", port_save_dir());
    port_save_path(g_quickState, sizeof g_quickState, "state_quick.oss");
    port_save_path(g_autoState, sizeof g_autoState, "state_auto.oss");
    port_save_path(g_bootResume, sizeof g_bootResume, "resume_boot");
    port_save_path(g_loadingFlag, sizeof g_loadingFlag, "state_loading");
    mkdir(g_saveDir, 0777);
    snprintf(tmp, sizeof tmp, "%s.tmp", g_autoState);
    remove(tmp);
    snprintf(tmp, sizeof tmp, "%s.tmp", g_quickState);
    remove(tmp);
    if (!port_ss_available()) {
        fprintf(stderr, "[estado] secoes do jogo ausentes: estados desligados\n");
        return;
    }
    fprintf(stderr, "[estado] RAM %u KB, dados %u KB, BSS %u KB\n", osMemSize / 1024,
            (unsigned)((__stop_game_data - __start_game_data) / 1024), (unsigned)((__stop_game_bss - __start_game_bss) / 1024));
    if (g_onion) {
        f = fopen(CMD_TO_RUN, "rb");
        if (f) {
            size_t n = fread(g_cmd, 1, sizeof g_cmd - 1, f);
            if (n > 0 && n < sizeof g_cmd - 1) {
                g_cmdLen = (int)n;
            }
            fclose(f);
        }
        g_offAtStart = exists("/tmp/.offOrder");
        make_checkoff_script(argv0);
        find_romscreen();
        read_menu_settings();
    }
    f = fopen("/tmp/prev_state", "r");
    if (f) {
        if (fgets(prev, sizeof prev, f)) {
            prev[strcspn(prev, "\r\n")] = 0;
        }
        fclose(f);
    }
    if (exists(g_loadingFlag)) {
        fprintf(stderr, "[estado] a ultima retomada nao chegou a rodar (travou?): comecando do inicio\n");
        remove(g_loadingFlag);
        remove(g_bootResume);
        return;
    }
    boot = exists(g_bootResume);
    switcher = !strcmp(prev, "switcher") || !strcmp(prev, "game");
    if (boot || switcher) {
        if (state_usable(g_autoState)) {
            g_bootLoad = 1;
        } else if (exists(g_autoState)) {
            fprintf(stderr, "[estado] %s e de outra versao do jogo: apagado\n", g_autoState);
            remove(g_autoState);
        }
    }
    if (!g_bootLoad) {
        remove(g_bootResume);
    }
    fprintf(stderr, "[estado] aberto por '%s'%s: %s\n", prev[0] ? prev : "?", boot ? " (desligou no jogo)" : "",
            g_bootLoad ? "continuando de onde parou" : "comecando do inicio");
}

/* ---------------------------------------------------------------- pedidos (botoes, sinais) */

/* tratador do SIGTERM: 1 = o jogo salva e fecha no proximo quadro; 0 = o jogo ainda nao roda (fechar ja) */
int port_ss_take_sigterm(void) {
    if (!g_loopRunning) {
        return 0;
    }
    g_offSignal = 1;
    return 1;
}

/* botoes: MENU sozinho (MENU_SINGLE/LONG/DOUBLE) */
void port_ss_menu(int press) {
    if (press >= 0 && press < 3) {
        g_menuPress = press;
    }
}

/* o toque simples precisa esperar um possivel segundo toque? */
int port_ss_menu_double_used(void) {
    return g_onion && g_menuAction[MENU_DOUBLE] != MENU_OFF;
}

static void request_exit(int kind) {
    if (g_exitKind < 0) {
        g_exitKind = kind;
        g_exitAt = now_ms();
    }
}

/* fecha sem estado (abertura, tela de arquivos, ou o jogo nao chegou a um ponto seguro) */
static void exit_without_state(int kind) {
    const int off = kind == EXIT_POWEROFF || kind == EXIT_BATTERY;
    if (!begin_exit()) {
        return; /* (o jogo ja esta salvando e fechando) */
    }
    fprintf(stderr, "[estado] fechando sem estado\n");
    if (kind == EXIT_RESET) { /* reiniciar do comeco: sem estado para continuar */
        remove(AUTO_STATE);
        remove(BOOT_RESUME);
        restart_now();
    }
    if (kind == EXIT_RESTART) { /* (fora do jogo: so abre de novo, do inicio) */
        if (!g_bootLoad) {
            remove(g_autoState);
            remove(g_bootResume);
        }
        restart_now();
    }
    if (g_bootLoad) { /* a retomada nem aconteceu: o estado no cartao continua sendo o de continuar */
        if (off) {
            touch(g_bootResume);
            put_back_cmd();
        }
        quit_now();
    }
    remove(g_autoState); /* o Game Switcher nao volta para um ponto antigo */
    remove(g_bootResume);
    if (off) {
        put_back_cmd();
    }
    if (kind == EXIT_SWITCHER) {
        touch(RUN_SWITCHER);
    }
    if (kind == EXIT_QUICK_SWITCH && !quick_switch_prepare()) {
        fprintf(stderr, "[estado] nenhum jogo anterior na lista: de volta ao menu\n");
    }
    quit_now();
}

/* chamado pelos botoes (thread de entrada) ~60 vezes por segundo: transforma o MENU em pedido e, se o jogo nao
 * atender um pedido de fechar em 5 s (travado, ou ainda abrindo), fecha mesmo assim */
void port_ss_input_tick(void) {
    int press = g_menuPress;
    if (press >= 0) {
        g_menuPress = -1;
        if (!g_onion) {
            if (press == MENU_SINGLE) {
                request_exit(EXIT_QUIT); /* (fora do OnionOS: o MENU fecha o jogo, como antes) */
            }
        } else {
            switch (g_menuAction[press]) {
                case MENU_SWITCHER: request_exit(EXIT_SWITCHER); break;
                case MENU_EXIT: request_exit(EXIT_QUIT); break;
                case MENU_QUICK_SWITCH: request_exit(EXIT_QUICK_SWITCH); break;
                default: break;
            }
        }
    }
    if (&miyoo_battery_exit != NULL && miyoo_battery_exit) {
        request_exit(EXIT_BATTERY);
    }
    if (g_offSignal) {
        request_exit(EXIT_POWEROFF);
    }
    if (g_exitKind >= 0 && now_ms() - g_exitAt > 5000.0) {
        fprintf(stderr, "[estado] o jogo nao chegou a um ponto seguro em 5 s\n");
        exit_without_state(g_exitKind);
    }
}

/* ---------------------------------------------------------------- o jogo (graph.c, entre dois quadros) */

/* o que fazer agora (OP_*). isPlay: no jogo propriamente dito; isSetup: o jogo ainda nem abriu a 1a tela */
int port_ss_want(int isPlay, int isSetup) {
    int off;
    g_loopRunning = 1;
    g_frames++;
    if (g_loadedAt && g_frames - g_loadedAt >= LOADED_OK_FRAMES) {
        remove(g_loadingFlag); /* a retomada rodou: nao travou o jogo */
        g_loadedAt = 0;
    }
    /* desligar: o sinal ou o arquivo do OnionOS (um antigo, de antes do jogo, nao conta) */
    off = g_offSignal;
    if (!off && g_onion && !g_offAtStart) {
        double now = now_ms();
        if (now >= g_nextOffPoll) {
            g_nextOffPoll = now + 250.0;
            off = exists("/tmp/.offOrder");
        }
    }
    if (off) {
        request_exit(EXIT_POWEROFF);
    }
    if (g_exitKind >= 0) {
        if (!isPlay) {
            exit_without_state(g_exitKind);
        }
        return OP_EXIT;
    }
    if (isSetup) {
        return OP_NONE;
    }
    if (g_bootLoad && g_frames >= 3) {
        return OP_LOAD_AUTO;
    }
    if (port_ss_req == 1) {
        if (!isPlay) {
            port_ss_req = 0;
            toast("Quick save only during gameplay", "Estado rápido só durante o jogo");
            return OP_NONE;
        }
        return OP_SAVE_QUICK;
    }
    if (port_ss_req == 2) {
        return OP_LOAD_QUICK;
    }
    return OP_NONE;
}

/* o jogo nao pode atender agora (troca de cena, leitura da ROM): tenta no proximo quadro */
void port_ss_defer(int op) {
    if (op != g_waitOp) {
        g_waitOp = op;
        g_wait = 0;
    }
    if (++g_wait < 150) { /* ~5 s */
        return;
    }
    g_wait = 0;
    if (op == OP_SAVE_SLOT || op == OP_LOAD_SLOT) {
        g_slotOp = 0;
        toast("Busy, try again", "Ocupado, tente de novo");
    } else if (op == OP_SAVE_QUICK || op == OP_LOAD_QUICK) {
        port_ss_req = 0;
        toast("Busy, try again", "Ocupado, tente de novo");
    } else if (op == OP_LOAD_AUTO) {
        g_bootLoad = 0;
        remove(g_bootResume);
        fprintf(stderr, "[estado] a retomada nao achou um ponto seguro: comecando do inicio\n");
    }
    /* (OP_EXIT: port_ss_input_tick fecha sem estado depois de 5 s) */
}

int port_ss_save(uint16_t seqMain, uint16_t seqSub) {
    double t0 = now_ms();
    port_ss_req = 0;
    g_waitOp = OP_NONE;
    pthread_mutex_lock(&g_m);
    g_buf = snapshot(seqMain, seqSub, g_buf, &g_bufLen);
    if (g_buf == NULL) {
        g_bufLen = 0;
        pthread_mutex_unlock(&g_m);
        toast("Not enough memory for quick save", "Sem memória para o estado rápido");
        return -1;
    }
    g_writePending = 1;
    if (!g_writerOn) {
        pthread_t th;
        if (pthread_create(&th, NULL, writer_main, NULL) == 0) {
            pthread_detach(th);
            g_writerOn = 1;
        }
    }
    pthread_cond_signal(&g_c);
    pthread_mutex_unlock(&g_m);
    fprintf(stderr, "[estado] copia %.1f ms\n", now_ms() - t0);
    toast("State saved", "Estado salvo");
    return 0;
}

static void slot_path(char* out, size_t n, int slot, const char* ext) {
    snprintf(out, n, "%s/state_%02d.%s", g_saveDir, slot + 1, ext);
}

/* which: 0 o estado rapido (memoria ou arquivo), 1 state_auto.oss (retomada ao abrir), 2 o estado g_slot do menu.
 * 0 = carregado. */
int port_ss_load(int which, uint16_t* seqMain, uint16_t* seqSub) {
    SsHdr h;
    double t0 = now_ms();
    g_waitOp = OP_NONE;
    if (which == 2) {
        char path[PORT_PATH_MAX];
        uint8_t* b = NULL;
        size_t len;
        int r;
        const int slot = g_slot;
        g_slotOp = 0;
        slot_path(path, sizeof path, slot, "oss");
        r = read_state(path, &b, &len);
        if (r != 1) {
            if (r == 0) {
                toast("Empty slot", "Estado vazio");
            } else {
                toast("Saved state is from another version", "Estado salvo é de outra versão");
            }
            return -1;
        }
        restore(b);
        memcpy(&h, b, sizeof h);
        free(b);
        fprintf(stderr, "[estado] %s carregado (%.0f ms)\n", path, now_ms() - t0);
        if (miyoo_menu_close_now != NULL) {
            miyoo_menu_close_now(); /* o jogo continua do estado carregado */
        }
        {
            char msg[48];
            snprintf(msg, sizeof msg, lang_text("State %d loaded", "Estado %d carregado"), slot + 1);
            toast(msg, msg);
        }
    } else if (which == 1) {
        uint8_t* b = NULL;
        size_t len;
        int r;
        g_bootLoad = 0;
        touch(g_loadingFlag);
        remove(g_bootResume);
        r = read_state(g_autoState, &b, &len);
        if (r != 1) {
            remove(g_loadingFlag);
            fprintf(stderr, "[estado] %s nao carregado (%s)\n", g_autoState, r == 0 ? "sem arquivo" : "outra versao ou estragado");
            return -1;
        }
        restore(b);
        memcpy(&h, b, sizeof h);
        free(b);
        g_loadedAt = g_frames;
        fprintf(stderr, "[estado] continuando de %s (%.0f ms)\n", g_autoState, now_ms() - t0);
    } else {
        port_ss_req = 0;
        pthread_mutex_lock(&g_m);
        if (g_buf == NULL) {
            uint8_t* b = NULL;
            size_t len;
            int r = read_state(g_quickState, &b, &len);
            if (r != 1) {
                pthread_mutex_unlock(&g_m);
                if (r == 0) {
                    toast("No saved state", "Nenhum estado salvo");
                } else {
                    toast("Saved state is from another version", "Estado salvo é de outra versão");
                }
                return -1;
            }
            g_buf = b;
            g_bufLen = len;
        }
        restore(g_buf);
        memcpy(&h, g_buf, sizeof h);
        pthread_mutex_unlock(&g_m);
        fprintf(stderr, "[estado] estado rapido carregado (%.1f ms)\n", now_ms() - t0);
        toast("State loaded", "Estado carregado");
    }
    *seqMain = h.seqMain;
    *seqSub = h.seqSub;
    return 0;
}

/* ---------------------------------------------------------------- os 10 estados do menu (aba Estados) */

/* menu_miyoo.c: pedir para salvar (OP_SAVE_SLOT = 5) ou carregar (6) o estado slot (0..9) */
void port_ss_slot_request(int save, int slot) {
    if (slot >= 0 && slot < NSLOTS) {
        g_slot = slot;
        g_slotOp = save ? OP_SAVE_SLOT : OP_LOAD_SLOT;
    }
}

int port_ss_menu_op(void) {
    return g_slotOp;
}

void port_ss_slot_not_play(void) {
    g_slotOp = 0;
    toast("Saving only during gameplay", "Salvar só durante o jogo");
}

/* 0 = vazio, 1 = ok (*when = hora em que foi salvo), -1 = de outra versao */
int port_ss_slot_info(int slot, long* when) {
    char path[PORT_PATH_MAX];
    struct stat st;
    slot_path(path, sizeof path, slot, "oss");
    if (stat(path, &st) != 0) {
        return 0;
    }
    *when = (long)st.st_mtime;
    return state_usable(path) ? 1 : -1;
}

/* a miniatura (256x192 RGB565) do estado slot; 1 = lida. As das versoes antigas (160x120) sao ampliadas. */
int port_ss_slot_thumb(int slot, uint16_t* out) {
    char path[PORT_PATH_MAX];
    FILE* f;
    long size;
    int ok = 0;
    slot_path(path, sizeof path, slot, "thm");
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size == THUMB_W * THUMB_H * 2) {
        ok = fread(out, 2, THUMB_W * THUMB_H, f) == THUMB_W * THUMB_H;
    } else if (size == 160 * 120 * 2) {
        uint16_t* old = malloc(160 * 120 * 2);
        if (old != NULL && fread(old, 2, 160 * 120, f) == 160 * 120) {
            int x, y;
            for (y = 0; y < THUMB_H; y++) {
                for (x = 0; x < THUMB_W; x++) {
                    out[y * THUMB_W + x] = old[(y * 120 / THUMB_H) * 160 + x * 160 / THUMB_W];
                }
            }
            ok = 1;
        }
        free(old);
    }
    fclose(f);
    return ok;
}

int port_ss_save_slot(uint16_t seqMain, uint16_t seqSub) {
    char path[PORT_PATH_MAX], msg[48];
    uint8_t* b;
    size_t len;
    int ok = 0;
    const int slot = g_slot;
    g_slotOp = 0;
    g_waitOp = OP_NONE;
    slot_path(path, sizeof path, slot, "oss");
    pthread_mutex_lock(&g_m);
    b = snapshot(seqMain, seqSub, NULL, &len);
    if (b != NULL) {
        ok = write_state(path, b);
        free(b);
    }
    pthread_mutex_unlock(&g_m);
    if (ok && miyoo_video_thumb != NULL) {
        uint16_t* thumb = malloc(THUMB_W * THUMB_H * 2); /* (nao estatico: ver menu_miyoo.c, g_thumbs) */
        if (thumb != NULL && miyoo_video_thumb(thumb)) {
            slot_path(path, sizeof path, slot, "thm");
            write_file(path, thumb, THUMB_W * THUMB_H * 2);
        }
        free(thumb);
    }
    if (ok) {
        snprintf(msg, sizeof msg, lang_text("Saved to state %d", "Salvo no estado %d"), slot + 1);
        toast(msg, msg);
    } else {
        toast("Save failed", "Falhou ao salvar");
    }
    if (miyoo_menu_slots_changed != NULL) {
        miyoo_menu_slots_changed();
    }
    return ok ? 0 : -1;
}

/* fechar com o estado (o jogo no ponto seguro): grava state_auto.oss e a imagem do Game Switcher. Nao volta. */
void port_ss_exit(uint16_t seqMain, uint16_t seqSub) {
    static const char* names[] = { "fechar", "Game Switcher", "desligar", "bateria no fim", "jogo anterior", "reiniciar",
                                   "reiniciar do comeco" };
    const int kind = g_exitKind < 0 ? EXIT_QUIT : g_exitKind;
    const int off = kind == EXIT_POWEROFF || kind == EXIT_BATTERY;
    uint8_t* b;
    size_t len;
    int ok = 0;
    if (g_bootLoad || kind == EXIT_RESET) {
        exit_without_state(kind); /* (a retomada nem aconteceu; ou reiniciar do comeco, sem estado) */
    }
    if (!begin_exit()) {
        for (;;) {
            pause(); /* (os botoes ja estao fechando o jogo sem estado) */
        }
    }
    fprintf(stderr, "[estado] saindo (%s)\n", names[kind]);
    if (g_onion && kind != EXIT_RESTART) {
        write_romscreen();
    }
    pthread_mutex_lock(&g_m); /* (espera um estado rapido que esteja sendo gravado) */
    b = snapshot(seqMain, seqSub, NULL, &len);
    if (b != NULL) {
        ok = write_state(g_autoState, b);
        free(b);
    }
    pthread_mutex_unlock(&g_m);
    if (ok) {
        remove(g_loadingFlag);
    } else {
        remove(g_autoState);
    }
    if (kind == EXIT_RESTART) { /* abre de novo e continua deste estado */
        if (ok) {
            touch(g_bootResume);
        }
        restart_now();
    }
    if (off) {
        if (ok) {
            touch(g_bootResume);
        }
        put_back_cmd();
    }
    if (kind == EXIT_SWITCHER) {
        touch(RUN_SWITCHER);
    }
    if (kind == EXIT_QUICK_SWITCH && !quick_switch_prepare()) {
        fprintf(stderr, "[estado] nenhum jogo anterior na lista: de volta ao menu\n");
    }
    quit_now();
}

/* menu de opcoes: uma opcao que so vale ao abrir o jogo mudou e o jogador quer reiniciar ja */
void port_ss_restart_request(void) {
    request_exit(EXIT_RESTART);
}

/* menu de opcoes, "Reiniciar o jogo": abre de novo do comeco (abertura, tela de arquivos), como o reset do N64;
 * antes salva o jogo sozinho, como o "Sair" (se autosave) */
void port_ss_reset_request(void) {
    request_exit(EXIT_RESET);
}

/* menu de opcoes, "Sair do jogo": fecha como o MENU fora do OnionOS (o jogo salva sozinho, se autosave, e sai) */
void port_ss_quit_request(void) {
    request_exit(EXIT_QUIT);
}
