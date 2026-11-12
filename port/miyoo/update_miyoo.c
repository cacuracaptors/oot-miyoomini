/*
 * update_miyoo.c -- "Procurar atualizacoes" no menu de opcoes (aba Geral): pergunta ao GitHub qual e o ultimo release
 * publicado do port e compara com a versao instalada (version.txt na pasta do jogo, gravado pelo tools/oot-package.sh).
 *
 * Etapa 1 (lote 223): so procura e mostra o "What's new" do release novo (o texto da pagina do release). O download e a
 * instalacao vem nas proximas etapas. Lote 224: ao abrir o menu, procura sozinha (lote 237: uma vez cada vez que o
 * aparelho e ligado; antes era uma vez por dia, o que dependia do relogio) e, se houver versao nova, so avisa (a
 * janela "What's new" fica para quando o jogador escolhe a opcao).
 *
 * Etapas 2 e 3 (lote 225): baixar e instalar. O zip do release (o mesmo da pagina do GitHub) e baixado para .update/
 * na pasta do jogo (no cartao SD, nao na memoria), conferido (tamanho e sha256 que o GitHub da) e aberto em .update/x
 * pelo 7z do OnionOS. Depois o jogador escolhe: salvar o jogo antes (o padrao) ou instalar e reiniciar. Instalar e
 * trocar os arquivos pelo nome (rename, no mesmo cartao: o programa que esta rodando continua inteiro na memoria, e o
 * atalho .port que o OnionOS esta rodando tambem) e abrir o jogo de novo do comeco (os estados salvos de outra versao
 * nao valem: savestate.c ja confere). Se a versao nova muda o cartucho (oot_swapmap.bin), o jogo novo refaz o
 * cartucho a partir da ROM: so instala se a ROM estiver na pasta.
 *
 * A rede: o OnionOS, ao abrir um jogo, desliga so os servidores (SSH, FTP, Samba...), nao o Wi-Fi. O pedido vai pelo
 * curl do proprio OnionOS (o mesmo do atualizador dele), com a conexao HTTPS conferida pela lista de certificados que
 * vai com o port (cacert.pem, da Mozilla); o OnionOS conecta sem conferir (-k), e aqui nao, porque o que vem do GitHub
 * vai virar o executavel. Tudo numa thread comum (sem prioridade de tempo real), fora do desenho e dos controles: o
 * jogo nao para enquanto espera a resposta. Memoria: a resposta vai para um arquivo em /tmp e e lida em ate 64 KB.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "miyoo.h"

int cart_is_valid(const char* path, const char* swapmap_path); /* port/cart_build.c */

extern char** environ;

#define UPD_REPO "cacuracaptors/oot-miyoomini"
#define UPD_JSON "/tmp/oot_update.json"
#define UPD_CODE "/tmp/oot_update.code"
#define UPD_ERR "/tmp/oot_update.err"
/* a procura sozinha (ao abrir o menu) e uma vez cada vez que o aparelho e ligado: a marca fica no /tmp, que e na
 * memoria e some ao desligar (fechar e abrir o jogo, o Game Switcher e o reinicio da instalacao nao apagam). Nao
 * depende do relogio: ha aparelhos sem relogio proprio (ou com a hora errada), em que "uma vez por dia" falhava. */
#define UPD_BOOT "/tmp/oot_update_checked"
#define UPD_STAMP_OLD "update_check.txt" /* (a marca de um dia das versoes 1.0.6 e 1.0.7, na pasta do jogo: apagada) */
#define UPD_RETRY (60 * 60)              /* se o GitHub nao respondeu: de novo so depois de uma hora (no mesmo jogo) */
#define UPD_DIR ".update"
#define UPD_ZIP UPD_DIR "/upd.zip"
#define UPD_X UPD_DIR "/x"
#define UPD_READY UPD_DIR "/ready.txt"
#define UPD_PORTS UPD_X "/Roms/PORTS"
#define UPD_GAME UPD_PORTS "/Games/ZeldaOoT"
#define ONION_BIN "/mnt/SDCARD/.tmp_update/bin/"
#define ONION_LIB "LD_LIBRARY_PATH=/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH "

static volatile int g_busy;
static volatile int g_auto; /* a procura em andamento foi a automatica (so avisa se houver versao nova) */

/* o pacote do release achado na ultima procura (o zip ZeldaOoT-miyoomini-vX.zip) */
static char g_tag[64];
static char g_url[512];
static char g_sha[72]; /* (64 hex; vazio = o GitHub nao deu) */
static long g_size;

/* download e instalacao (lido pelo menu a cada quadro) */
static volatile int g_st = MIYOO_UPST_IDLE, g_err, g_cancel, g_needRom, g_hasRom;
static volatile long g_got;
int port_update_installed; /* port_main.c: reiniciar abre o arquivo novo, nao o que estava rodando */

/* versao instalada (version.txt: "1.0.5"); vazio = build de desenvolvimento */
static void local_version(char* out, size_t n) {
    FILE* f = fopen("version.txt", "r");
    out[0] = 0;
    if (f != NULL) {
        if (fgets(out, (int)n, f) == NULL) {
            out[0] = 0;
        }
        fclose(f);
    }
    out[strcspn(out, "\r\n \t")] = 0;
}

void port_update_local_version(char* out, size_t n) { /* (menu: a versao na aba Geral) */
    local_version(out, n);
}

/* "v1.0.5", "1.0.5-teste2" -> 1, 0, 5 (o resto e ignorado) */
static void parse_ver(const char* s, int v[3]) {
    int i;
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (i = 0; i < 3; i++) {
        char* e;
        v[i] = (int)strtol(s, &e, 10);
        if (*e != '.') {
            break;
        }
        s = e + 1;
    }
}

static int ver_cmp(const char* a, const char* b) {
    int x[3], y[3], i;
    parse_ver(a, x);
    parse_ver(b, y);
    for (i = 0; i < 3; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

/* ha uma rota padrao (a conexao de rede esta de pe)? */
static int has_route(void) {
    FILE* f = fopen("/proc/net/route", "r");
    char line[256];
    int ok = 0;
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char ifc[32], dst[32];
        if (sscanf(line, "%31s %31s", ifc, dst) == 2 && !strcmp(dst, "00000000") && strcmp(ifc, "lo") != 0) {
            ok = 1;
        }
    }
    fclose(f);
    return ok;
}

static void put_utf8(char** o, char* end, unsigned cp) {
    char* p = *o;
    if (cp < 0x80) {
        if (p + 1 < end) {
            *p++ = (char)cp;
        }
    } else if (cp < 0x800) {
        if (p + 2 < end) {
            *p++ = (char)(0xC0 | (cp >> 6));
            *p++ = (char)(0x80 | (cp & 0x3F));
        }
    } else if (p + 3 < end) {
        *p++ = (char)(0xE0 | (cp >> 12));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    }
    *o = p;
}

/* o valor (texto) de "key" no JSON, com os escapes desfeitos; 0 = nao achou */
static int json_str(const char* js, const char* key, char* out, size_t n) {
    char pat[64];
    const char* p;
    char *o = out, *end = out + n;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(js, pat);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return 0;
    }
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': put_utf8(&o, end, '\n'); break;
                case 'r': break;
                case 't': put_utf8(&o, end, ' '); break;
                case 'u': {
                    unsigned cp = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                    if (p[1] && p[2] && p[3] && p[4]) {
                        p += 4;
                    }
                    put_utf8(&o, end, cp);
                    break;
                }
                default: put_utf8(&o, end, (unsigned char)*p); break;
            }
            p++;
            continue;
        }
        if (o + 1 < end) {
            *o++ = *p;
        }
        p++;
    }
    *o = 0;
    return 1;
}

/* o texto do release (Markdown do GitHub) em texto simples para a janela: tira titulos (#), negrito, imagens, links e
 * etiquetas HTML; linhas em branco seguidas viram uma */
static void md_clean(char* s) {
    char* r = s;
    char* w = s;
    int blank = 1;
    while (*r) {
        char line[1024];
        char* l = line;
        char* le = line + sizeof line - 1;
        char* p;
        while (*r && *r != '\n' && l < le) {
            *l++ = *r++;
        }
        while (*r && *r != '\n') {
            r++;
        }
        if (*r == '\n') {
            r++;
        }
        *l = 0;
        /* etiquetas HTML e imagens fora; [texto](link) vira texto; ** e ` fora */
        {
            char tmp[1024];
            char* t = tmp;
            p = line;
            while (*p && t < tmp + sizeof tmp - 1) {
                if (*p == '<') {
                    char* q = strchr(p, '>');
                    if (q != NULL) {
                        p = q + 1;
                        continue;
                    }
                }
                if (p[0] == '!' && p[1] == '[') {
                    char* q = strchr(p, ')');
                    if (q != NULL) {
                        p = q + 1;
                        continue;
                    }
                }
                if (*p == '[') {
                    char* q = strstr(p, "](");
                    char* c = q != NULL ? strchr(q, ')') : NULL;
                    if (c != NULL) {
                        size_t k = (size_t)(q - p - 1);
                        if (t + k < tmp + sizeof tmp - 1) {
                            memcpy(t, p + 1, k);
                            t += k;
                        }
                        p = c + 1;
                        continue;
                    }
                }
                if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
                    p += 2;
                    continue;
                }
                if (*p == '`') {
                    p++;
                    continue;
                }
                *t++ = *p++;
            }
            *t = 0;
            p = tmp;
            while (*p == '#') {
                p++;
            }
            while (*p == ' ') {
                p++;
            }
            if (*p == 0) {
                if (!blank) {
                    *w++ = '\n';
                    blank = 1;
                }
                continue;
            }
            blank = 0;
            {
                const size_t k = strlen(p);
                memmove(w, p, k);
                w += k;
                *w++ = '\n';
            }
        }
    }
    while (w > s && w[-1] == '\n') {
        w--;
    }
    *w = 0;
}

/* ---- sha256 (para conferir o zip com o que o GitHub diz) ---- */
typedef struct {
    uint32_t h[8];
    uint8_t b[64];
    uint64_t n;
    int k;
} Sha;

static const uint32_t k_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, r) (((x) >> (r)) | ((x) << (32 - (r))))

static void sha_block(Sha* c) {
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)c->b[i * 4] << 24 | (uint32_t)c->b[i * 4 + 1] << 16 | (uint32_t)c->b[i * 4 + 2] << 8 | c->b[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_sha[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

static void sha_add(Sha* c, const uint8_t* p, size_t n) {
    while (n--) {
        c->b[c->k++] = *p++;
        c->n++;
        if (c->k == 64) {
            sha_block(c);
            c->k = 0;
        }
    }
}

/* o sha256 do arquivo em hexadecimal (65 bytes); 0 = nao deu para ler */
static int sha_file(const char* path, char out[65]) {
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static uint8_t buf[32768];
    Sha c;
    uint64_t bits;
    size_t n;
    int i;
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    memset(&c, 0, sizeof c);
    memcpy(c.h, h0, sizeof h0);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sha_add(&c, buf, n);
    }
    fclose(f);
    bits = c.n * 8;
    {
        uint8_t pad = 0x80, z = 0, len[8];
        sha_add(&c, &pad, 1);
        while (c.k != 56) {
            sha_add(&c, &z, 1);
        }
        for (i = 0; i < 8; i++) {
            len[i] = (uint8_t)(bits >> (56 - 8 * i));
        }
        sha_add(&c, len, 8);
    }
    for (i = 0; i < 8; i++) {
        sprintf(out + i * 8, "%08x", (unsigned)c.h[i]);
    }
    return 1;
}

static pid_t spawn_sh(const char* cmd) {
    char* argv[] = { "sh", "-c", (char*)cmd, NULL };
    posix_spawnattr_t at;
    struct sched_param sp = { 0 };
    pid_t pid;
    int r;
    posix_spawnattr_init(&at);
    posix_spawnattr_setschedpolicy(&at, SCHED_OTHER);
    posix_spawnattr_setschedparam(&at, &sp);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSCHEDULER);
    r = posix_spawn(&pid, "/bin/sh", NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    return r == 0 ? pid : -1;
}

static int run_sh(const char* cmd) {
    char* argv[] = { "sh", "-c", (char*)cmd, NULL };
    posix_spawnattr_t at;
    struct sched_param sp = { 0 };
    pid_t pid;
    int st = 0, r;
    posix_spawnattr_init(&at);
    /* o curl roda sem a prioridade de tempo real de quem chama (o menu roda na thread dos controles) */
    posix_spawnattr_setschedpolicy(&at, SCHED_OTHER);
    posix_spawnattr_setschedparam(&at, &sp);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSCHEDULER);
    r = posix_spawn(&pid, "/bin/sh", NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    if (r != 0) {
        return -1;
    }
    while (waitpid(pid, &st, 0) < 0) {
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* o numero de "key" no JSON a partir de p; -1 = nao achou */
static long json_num(const char* p, const char* key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(p, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return *p >= '0' && *p <= '9' ? strtol(p, NULL, 10) : -1;
}

/* o zip do port entre os arquivos do release ("assets"): o nome ZeldaOoT-miyoomini-*.zip; o tamanho, o sha256 e o
 * endereco vem depois do nome, no mesmo item */
static void find_asset(const char* js, const char* tag) {
    const char* p = strstr(js, "\"assets\"");
    g_url[0] = 0;
    g_sha[0] = 0;
    g_size = 0;
    snprintf(g_tag, sizeof g_tag, "%s", tag);
    while (p != NULL && (p = strstr(p + 1, "\"name\"")) != NULL) {
        char name[128];
        size_t n;
        if (!json_str(p, "name", name, sizeof name)) {
            continue;
        }
        n = strlen(name);
        if (strncmp(name, "ZeldaOoT-miyoomini", 18) != 0 || n < 5 || strcasecmp(name + n - 4, ".zip") != 0) {
            continue;
        }
        {
            char dg[128] = "";
            const char* u = strstr(p, "\"browser_download_url\"");
            const char* d = strstr(p, "\"digest\"");
            g_size = json_num(p, "size");
            if (u != NULL) {
                json_str(u, "browser_download_url", g_url, sizeof g_url);
            }
            if (d != NULL && (u == NULL || d < u) && json_str(d, "digest", dg, sizeof dg) && !strncmp(dg, "sha256:", 7) &&
                strlen(dg + 7) == 64) {
                snprintf(g_sha, sizeof g_sha, "%s", dg + 7);
            }
        }
        fprintf(stderr, "[atualizacao] pacote %s, %ld bytes, sha256 %s\n", name, g_size, g_sha[0] ? g_sha : "(nao veio)");
        return;
    }
    fprintf(stderr, "[atualizacao] o release nao tem o zip do port\n");
}

/* ha uma ROM na pasta do jogo (para refazer o cartucho)? */
static int rom_present(void) {
    DIR* d = opendir(".");
    struct dirent* e;
    int ok = 0;
    if (d == NULL) {
        return 0;
    }
    while (!ok && (e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        ok = n > 4 && (!strcasecmp(e->d_name + n - 4, ".z64") || !strcasecmp(e->d_name + n - 4, ".n64") ||
                       !strcasecmp(e->d_name + n - 4, ".v64") || !strcasecmp(e->d_name + n - 4, ".zip"));
    }
    closedir(d);
    return ok;
}

/* o pacote aberto em .update/x esta pronto: o cartucho atual vale para a versao nova? */
static void set_ready(void) {
    g_needRom = !cart_is_valid("oot.cart", UPD_GAME "/oot_swapmap.bin");
    g_hasRom = rom_present();
    fprintf(stderr, "[atualizacao] %s pronta para instalar%s\n", g_tag,
            g_needRom ? (g_hasRom ? " (refaz o cartucho com a ROM da pasta)" : " (precisa da ROM: o cartucho muda)") : "");
    g_st = MIYOO_UPST_READY;
}

/* o resultado vai ao menu; na procura automatica, so o de versao nova (o resto fica so no log) */
static void result(int code, const char* latest, const char* cur, const char* notes) {
    if (!g_auto || code == MIYOO_UPD_NEWER) {
        miyoo_menu_update_result(code, latest, cur, notes, g_auto);
    }
}

static void* check_thread(void* arg) {
    static char js[65536];
    static char notes[16384];
    char cur[32], tag[64], cmd[1024], code[16] = "";
    const char* curl = access("/mnt/SDCARD/.tmp_update/bin/curl", X_OK) == 0 ? "/mnt/SDCARD/.tmp_update/bin/curl" : "curl";
    FILE* f;
    size_t n;
    int rc;
    (void)arg;
    local_version(cur, sizeof cur);
    if (!has_route()) {
        fprintf(stderr, "[atualizacao] sem rede (sem rota padrao)\n");
        result(MIYOO_UPD_NO_NET, NULL, cur, NULL);
        g_busy = 0;
        return NULL;
    }
    remove(UPD_JSON);
    snprintf(cmd, sizeof cmd,
             "LD_LIBRARY_PATH=/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH %s -sS -L --max-time 20 --cacert cacert.pem "
             "-A oot-miyoomini -H 'Accept: application/vnd.github+json' -o %s -w '%%{http_code}' "
             "https://api.github.com/repos/" UPD_REPO "/releases/latest > %s 2> %s",
             curl, UPD_JSON, UPD_CODE, UPD_ERR);
    rc = run_sh(cmd);
    f = fopen(UPD_CODE, "r");
    if (f != NULL) {
        if (fgets(code, sizeof code, f) == NULL) {
            code[0] = 0;
        }
        fclose(f);
    }
    if (rc != 0 || strcmp(code, "200") != 0) {
        char err[200] = "";
        f = fopen(UPD_ERR, "r");
        if (f != NULL) {
            if (fgets(err, sizeof err, f) == NULL) {
                err[0] = 0;
            }
            fclose(f);
        }
        err[strcspn(err, "\r\n")] = 0;
        fprintf(stderr, "[atualizacao] o pedido falhou: curl %d, http %s, %s\n", rc, code, err);
        result(MIYOO_UPD_ERROR, NULL, cur, NULL);
        g_busy = 0;
        return NULL;
    }
    f = fopen(UPD_JSON, "r");
    n = 0;
    if (f != NULL) {
        n = fread(js, 1, sizeof js - 1, f);
        fclose(f);
    }
    js[n] = 0;
    remove(UPD_JSON);
    remove(UPD_CODE);
    remove(UPD_ERR);
    if (!json_str(js, "tag_name", tag, sizeof tag) || tag[0] == 0) {
        fprintf(stderr, "[atualizacao] resposta sem versao (%u bytes)\n", (unsigned)n);
        result(MIYOO_UPD_ERROR, NULL, cur, NULL);
        g_busy = 0;
        return NULL;
    }
    f = fopen(UPD_BOOT, "w"); /* o GitHub respondeu: a automatica nao procura mais ate desligar o aparelho */
    if (f != NULL) {
        fputs("1\n", f);
        fclose(f);
    }
    remove(UPD_STAMP_OLD);
    find_asset(js, tag);
    notes[0] = 0;
    json_str(js, "body", notes, sizeof notes);
    md_clean(notes);
    fprintf(stderr, "[atualizacao] %sinstalada %s, no GitHub %s\n", g_auto ? "(procura automatica) " : "",
            cur[0] ? cur : "(sem version.txt)", tag);
    if (cur[0] && ver_cmp(cur, tag) >= 0) {
        result(MIYOO_UPD_LATEST, tag, cur, NULL);
    } else {
        if (g_st != MIYOO_UPST_DOWNLOADING && g_st != MIYOO_UPST_PREPARING) {
            /* ja baixada antes (o jogador escolheu salvar primeiro)? entao direto para a escolha */
            char rd[64] = "";
            FILE* r = fopen(UPD_READY, "r");
            if (r != NULL) {
                if (fgets(rd, sizeof rd, r) == NULL) {
                    rd[0] = 0;
                }
                fclose(r);
            }
            rd[strcspn(rd, "\r\n")] = 0;
            if (rd[0] && !strcmp(rd, tag) && access(UPD_GAME "/oot", R_OK) == 0) {
                set_ready();
            } else {
                g_st = MIYOO_UPST_IDLE;
                if (access(UPD_DIR, F_OK) == 0) {
                    run_sh("rm -rf " UPD_DIR); /* (sobra de outra versao, ou de um download que nao terminou) */
                }
            }
        }
        result(MIYOO_UPD_NEWER, tag, cur, notes);
    }
    g_busy = 0;
    return NULL;
}

/* chamado pelo menu: automatic = 0, A em "Procurar atualizacoes"; 1, o menu acabou de abrir (procura uma vez cada vez
 * que o aparelho e ligado, e so se o Wi-Fi estiver ligado; se achar versao nova, so avisa e pede para usar a opcao
 * do menu).
 * 0 = nao comecou (ja estava procurando, ou a automatica nao era hora) */
int port_update_check_start(int automatic) {
    static time_t lastTry;
    pthread_t th;
    pthread_attr_t at;
    struct sched_param sp = { 0 };
    const time_t now = time(NULL);
    if (g_busy) {
        if (!automatic && g_auto) {
            g_auto = 0; /* o jogador pediu enquanto a automatica rodava: o resultado dela vale como o do pedido */
            return 1;
        }
        return 0;
    }
    if (automatic) {
        if (access(UPD_BOOT, F_OK) == 0) {
            return 0; /* ja procurou desde que o aparelho foi ligado */
        }
        if (lastTry != 0 && now >= lastTry && now - lastTry < UPD_RETRY) {
            return 0;
        }
        if (!has_route()) {
            return 0; /* sem Wi-Fi: tenta de novo na proxima vez que o menu abrir */
        }
        lastTry = now;
    }
    g_auto = automatic;
    g_busy = 1;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
    if (pthread_create(&th, &at, check_thread, NULL) != 0) {
        g_busy = 0;
        pthread_attr_destroy(&at);
        return 0;
    }
    pthread_attr_destroy(&at);
    return 1;
}

/* ---------------------------------------------------------------- download e instalacao */

static long mem_available(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = -1;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (sscanf(line, "MemAvailable: %ld", &kb) == 1) {
            break;
        }
    }
    fclose(f);
    return kb;
}

static void* fail(int err) {
    g_err = err;
    g_st = MIYOO_UPST_FAILED;
    run_sh("rm -rf " UPD_DIR);
    g_busy = 0;
    return NULL;
}

static void* dl_thread(void* arg) {
    static char cmd[1024];
    const char* curl = access(ONION_BIN "curl", X_OK) == 0 ? ONION_BIN "curl" : "curl";
    struct statvfs vf;
    struct stat st;
    pid_t pid;
    int status = 0, rc;
    (void)arg;
    run_sh("rm -rf " UPD_DIR);
    if (mkdir(UPD_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[atualizacao] nao criei %s: %s\n", UPD_DIR, strerror(errno));
        return fail(MIYOO_UPERR_SPACE);
    }
    /* espaco no cartao: o zip, os arquivos abertos (cerca de 2,5x o zip) e folga */
    if (statvfs(".", &vf) == 0) {
        const unsigned long long freeB = (unsigned long long)vf.f_bavail * vf.f_frsize;
        const unsigned long long need = (unsigned long long)g_size * 4 + (16ull << 20);
        if (freeB < need) {
            fprintf(stderr, "[atualizacao] pouco espaco no cartao: %llu MB livres, precisa de %llu\n", freeB >> 20, need >> 20);
            return fail(MIYOO_UPERR_SPACE);
        }
    }
    if (!has_route()) {
        return fail(MIYOO_UPERR_NET);
    }
    snprintf(cmd, sizeof cmd,
             "export " ONION_LIB "; exec %s -sS -L -f --cacert cacert.pem -A oot-miyoomini --connect-timeout 20 --speed-limit 512 "
                       "--speed-time 30 -o " UPD_ZIP " '%s' 2> " UPD_DIR "/curl.err",
             curl, g_url);
    fprintf(stderr, "[atualizacao] baixando %s\n", g_url);
    pid = spawn_sh(cmd);
    if (pid < 0) {
        return fail(MIYOO_UPERR_DOWNLOAD);
    }
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (stat(UPD_ZIP, &st) == 0) {
            g_got = (long)st.st_size;
        }
        if (r == pid || (r < 0 && errno != EINTR)) {
            break;
        }
        if (g_cancel) {
            kill(pid, SIGTERM); /* (o sh deu lugar ao curl: exec) */
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            fprintf(stderr, "[atualizacao] download cancelado\n");
            run_sh("rm -rf " UPD_DIR);
            g_st = MIYOO_UPST_IDLE;
            g_busy = 0;
            return NULL;
        }
        usleep(200000);
    }
    rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0 || stat(UPD_ZIP, &st) != 0 || (g_size > 0 && (long)st.st_size != g_size)) {
        char err[200] = "";
        FILE* f = fopen(UPD_DIR "/curl.err", "r");
        if (f != NULL) {
            if (fgets(err, sizeof err, f) == NULL) {
                err[0] = 0;
            }
            fclose(f);
        }
        err[strcspn(err, "\r\n")] = 0;
        fprintf(stderr, "[atualizacao] o download falhou: curl %d, %ld de %ld bytes, %s\n", rc, g_got, g_size, err);
        return fail(MIYOO_UPERR_DOWNLOAD);
    }
    g_st = MIYOO_UPST_PREPARING;
    if (g_sha[0]) {
        char h[72];
        if (!sha_file(UPD_ZIP, h) || strcmp(h, g_sha) != 0) {
            fprintf(stderr, "[atualizacao] o sha256 nao confere: %s (esperado %s)\n", h, g_sha);
            return fail(MIYOO_UPERR_VERIFY);
        }
        fprintf(stderr, "[atualizacao] sha256 confere\n");
    }
    {
        const long kb = mem_available();
        fprintf(stderr, "[atualizacao] memoria livre: %ld KB\n", kb);
        if (kb >= 0 && kb < 12 * 1024) {
            return fail(MIYOO_UPERR_MEMORY);
        }
    }
    if (access(ONION_BIN "7z", X_OK) == 0) {
        snprintf(cmd, sizeof cmd, ONION_LIB ONION_BIN "7z x -y -o" UPD_X " " UPD_ZIP " > " UPD_DIR "/x.log 2>&1");
    } else {
        snprintf(cmd, sizeof cmd, "unzip -o -q -d " UPD_X " " UPD_ZIP " > " UPD_DIR "/x.log 2>&1");
    }
    rc = run_sh(cmd);
    if (rc != 0 || access(UPD_GAME "/oot", R_OK) != 0 || access(UPD_GAME "/oot_swapmap.bin", R_OK) != 0) {
        fprintf(stderr, "[atualizacao] nao consegui abrir o zip (%d)\n", rc);
        return fail(MIYOO_UPERR_EXTRACT);
    }
    remove(UPD_ZIP);
    {
        FILE* f = fopen(UPD_READY, "w");
        if (f != NULL) {
            fprintf(f, "%s\n", g_tag);
            fclose(f);
        }
    }
    set_ready();
    g_busy = 0;
    return NULL;
}

/* menu: A na janela "What's new"; 0 = nao comecou */
int port_update_download_start(void) {
    pthread_t th;
    pthread_attr_t at;
    struct sched_param sp = { 0 };
    if (g_busy || g_url[0] == 0 || g_st == MIYOO_UPST_DOWNLOADING || g_st == MIYOO_UPST_PREPARING) {
        return 0;
    }
    g_busy = 1;
    g_cancel = 0;
    g_got = 0;
    g_err = 0;
    g_st = MIYOO_UPST_DOWNLOADING;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
    if (pthread_create(&th, &at, dl_thread, NULL) != 0) {
        g_busy = 0;
        g_st = MIYOO_UPST_IDLE;
        pthread_attr_destroy(&at);
        return 0;
    }
    pthread_attr_destroy(&at);
    return 1;
}

void port_update_cancel(void) {
    if (g_st == MIYOO_UPST_DOWNLOADING) {
        g_cancel = 1;
    }
}

int port_update_has_package(void) {
    return g_url[0] != 0;
}

/* o estado para o menu (got/total: bytes baixados) */
int port_update_state(long* got, long* total, int* err, int* needRom, int* hasRom) {
    *got = g_got;
    *total = g_size;
    *err = g_err;
    *needRom = g_needRom;
    *hasRom = g_hasRom;
    return g_st;
}

void port_update_forget_error(void) {
    if (g_st == MIYOO_UPST_FAILED) {
        g_st = MIYOO_UPST_IDLE;
    }
}

/* os arquivos de src no lugar dos de dst (pastas: entra nelas); "oot" (o programa) por ultimo, no fim de tudo */
static int move_tree(const char* src, const char* dst, int* nfiles) {
    char a[512], b[512];
    DIR* d = opendir(src);
    struct dirent* e;
    int bad = 0;
    if (d == NULL) {
        return 0; /* (o pacote nao tem essa pasta) */
    }
    mkdir(dst, 0755);
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        if (stat(a, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            bad |= move_tree(a, b, nfiles);
        } else if (strcmp(a, UPD_GAME "/oot") != 0) {
            if (rename(a, b) != 0) {
                fprintf(stderr, "[atualizacao] nao troquei %s: %s\n", b, strerror(errno));
                bad = 1;
            } else {
                (*nfiles)++;
            }
        }
    }
    closedir(d);
    return bad;
}

/* menu: "Instalar e reiniciar". 0 = instalada (o menu pede o reinicio); senao MIYOO_UPERR_* */
int port_update_install(void) {
    int n = 0, bad = 0;
    FILE* f;
    if (g_st != MIYOO_UPST_READY) {
        return MIYOO_UPERR_EXTRACT;
    }
    g_hasRom = rom_present();
    if (g_needRom && !g_hasRom) {
        return MIYOO_UPERR_NEEDROM;
    }
    /* a pasta do jogo e a pasta . (o atalho roda o jogo de dentro dela); Shortcuts e Imgs ficam duas acima */
    bad |= move_tree(UPD_GAME, ".", &n);
    if (access("../../Shortcuts", F_OK) == 0) {
        bad |= move_tree(UPD_PORTS "/Shortcuts", "../../Shortcuts", &n);
        bad |= move_tree(UPD_PORTS "/Imgs", "../../Imgs", &n);
    }
    if (bad) {
        fprintf(stderr, "[atualizacao] instalacao incompleta (%d arquivos trocados); o programa ficou o antigo\n", n);
        return MIYOO_UPERR_INSTALL;
    }
    if (rename(UPD_GAME "/oot", "oot") != 0) {
        fprintf(stderr, "[atualizacao] nao troquei oot: %s\n", strerror(errno));
        return MIYOO_UPERR_INSTALL;
    }
    f = fopen("version.txt", "w");
    if (f != NULL) {
        fprintf(f, "%s\n", g_tag[0] == 'v' || g_tag[0] == 'V' ? g_tag + 1 : g_tag);
        fclose(f);
    }
    sync();
    run_sh("rm -rf " UPD_DIR);
    sync();
    port_update_installed = 1;
    fprintf(stderr, "[atualizacao] %s instalada (%d arquivos + oot); reiniciando\n", g_tag, n);
    g_st = MIYOO_UPST_IDLE;
    return 0;
}
