/*
 * update_miyoo.c -- "Procurar atualizacoes" no menu de opcoes (aba Geral): pergunta ao GitHub qual e o ultimo release
 * publicado do port e compara com a versao instalada (version.txt na pasta do jogo, gravado pelo tools/oot-package.sh).
 *
 * Etapa 1 (lote 223): so procura e mostra o "What's new" do release novo (o texto da pagina do release). O download e a
 * instalacao vem nas proximas etapas.
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
#include <sys/wait.h>
#include <unistd.h>

#include "miyoo.h"

extern char** environ;

#define UPD_REPO "cacuracaptors/oot-miyoomini"
#define UPD_JSON "/tmp/oot_update.json"
#define UPD_CODE "/tmp/oot_update.code"
#define UPD_ERR "/tmp/oot_update.err"

static volatile int g_busy;

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
        miyoo_menu_update_result(MIYOO_UPD_NO_NET, NULL, cur, NULL);
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
        miyoo_menu_update_result(MIYOO_UPD_ERROR, NULL, cur, NULL);
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
        miyoo_menu_update_result(MIYOO_UPD_ERROR, NULL, cur, NULL);
        g_busy = 0;
        return NULL;
    }
    notes[0] = 0;
    json_str(js, "body", notes, sizeof notes);
    md_clean(notes);
    fprintf(stderr, "[atualizacao] instalada %s, no GitHub %s\n", cur[0] ? cur : "(sem version.txt)", tag);
    if (cur[0] && ver_cmp(cur, tag) >= 0) {
        miyoo_menu_update_result(MIYOO_UPD_LATEST, tag, cur, NULL);
    } else {
        miyoo_menu_update_result(MIYOO_UPD_NEWER, tag, cur, notes);
    }
    g_busy = 0;
    return NULL;
}

/* chamado pelo menu (A em "Procurar atualizacoes"); 0 = ja estava procurando */
int port_update_check_start(void) {
    pthread_t th;
    pthread_attr_t at;
    struct sched_param sp = { 0 };
    if (g_busy) {
        return 0;
    }
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
