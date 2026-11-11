/*
 * screen_miyoo.c -- contraste, matiz e saturacao da tela (opcoes screen_contrast, screen_hue, screen_saturation),
 * pelo ajuste de cor do proprio chip da tela (o mesmo do Tweaks do OnionOS: Display): sem custo nenhum no desenho.
 *
 * O OnionOS guarda os valores dele no /appconfigs/system.json (0 a 20) e os passa ao chip pelo comando "csc" de
 * /proc/mi_modules/mi_disp/mi_disp0 (.tmp_update/script/disp_csc_reset.sh): contraste + 40, matiz x 5, saturacao x 5;
 * a luminancia fica com o OnionOS (atalho Menu+volume) e aqui so e repetida como esta no chip. As opcoes usam a mesma
 * escala do Tweaks; -1 = a do sistema (nada e mudado).
 *
 * Uma thread comum (sem tempo real) confere as opcoes 4 vezes por segundo e escreve so quando mudam; com alguma opcao
 * ligada, a cada 2 s confere tambem o chip (o OnionOS pode ter escrito os valores dele, ao mexer na luminancia) e
 * escreve de novo. Ao fechar o jogo (sair, Game Switcher, reiniciar, erro), os valores do sistema voltam.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DISP "/proc/mi_modules/mi_disp/mi_disp0"

extern int port_opt_screen_contrast, port_opt_screen_hue, port_opt_screen_saturation; /* port_options.c */

static int g_on;          /* algum valor nosso esta no chip */
static int g_want[3] = { -1, -1, -1 };
static volatile int g_quit;
static pthread_t g_th;
static int g_started;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

/* os valores do OnionOS (0..20), do system.json; os que faltarem: os padroes dele (10) */
static void sys_values(int* cont, int* hue, int* sat) {
    static char js[8192];
    FILE* f = fopen("/appconfigs/system.json", "r");
    size_t n = 0;
    const char* keys[3] = { "\"contrast\"", "\"hue\"", "\"saturation\"" };
    int* out[3] = { cont, hue, sat };
    int i;
    *cont = *hue = *sat = 10;
    if (f == NULL) {
        return;
    }
    n = fread(js, 1, sizeof js - 1, f);
    fclose(f);
    js[n] = 0;
    for (i = 0; i < 3; i++) {
        const char* p = strstr(js, keys[i]);
        if (p != NULL) {
            p = strchr(p, ':');
            if (p != NULL) {
                const int v = atoi(p + 1);
                if (v >= 0 && v <= 20) {
                    *out[i] = v;
                }
            }
        }
    }
}

/* o que o chip usa agora: luminancia, contraste, matiz, saturacao, nitidez (0 = nao deu para ler) */
static int chip_values(int v[5]) {
    char line[256];
    int found = 0;
    FILE* f = fopen(DISP, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, "CscMatrix") != NULL && strstr(line, "Saturation") != NULL) {
            if (fgets(line, sizeof line, f) != NULL &&
                sscanf(line, "%*s %*s %*d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
                found = 1;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

static int chip_write(int luma, int cont, int hue, int sat, int sharp) {
    FILE* f = fopen(DISP, "w");
    int ok;
    if (f == NULL) {
        return 0;
    }
    /* csc devid matriz(3 = RGB para BT709, a do OnionOS) contraste matiz luminancia saturacao nitidez ganho */
    ok = fprintf(f, "csc 0 3 %d %d %d %d %d 0\n", cont, hue, luma, sat, sharp) > 0;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* os valores que devem estar no chip (escala dele) a partir das opcoes e do sistema */
static void target(const int want[3], int* cont, int* hue, int* sat) {
    int sc, sh, ss;
    sys_values(&sc, &sh, &ss);
    *cont = (want[0] >= 0 ? want[0] : sc) + 40;
    *hue = (want[1] >= 0 ? want[1] : sh) * 5;
    *sat = (want[2] >= 0 ? want[2] : ss) * 5;
}

/* escreve no chip o que as opcoes pedem (ou devolve o do sistema); com check, so se o chip estiver diferente */
static void apply(const int want[3], int check) {
    int v[5], cont, hue, sat;
    if (!chip_values(v)) {
        return; /* (sem o mi_disp0: nada a fazer) */
    }
    target(want, &cont, &hue, &sat);
    if (check && v[1] == cont && v[2] == hue && v[3] == sat) {
        return;
    }
    if (chip_write(v[0], cont, hue, sat, v[4])) {
        fprintf(stderr, "tela: contraste %d, matiz %d, saturacao %d (chip; luminancia %d fica)\n", cont, hue, sat, v[0]);
    }
}

static void* screen_thread(void* arg) {
    int tick = 0;
    (void)arg;
    while (!g_quit) {
        const int w[3] = { port_opt_screen_contrast, port_opt_screen_hue, port_opt_screen_saturation };
        const int any = w[0] >= 0 || w[1] >= 0 || w[2] >= 0;
        pthread_mutex_lock(&g_mx);
        if (!g_quit) {
            if (memcmp(w, g_want, sizeof w) != 0) {
                if (any || g_on) {
                    apply(w, 0); /* (todas em -1 depois de alguma ligada: volta o do sistema) */
                }
                memcpy(g_want, w, sizeof w);
                g_on = any;
                tick = 0;
            } else if (any && ++tick >= 8) {
                apply(w, 1); /* o OnionOS pode ter escrito os valores dele por cima */
                tick = 0;
            }
        }
        pthread_mutex_unlock(&g_mx);
        usleep(250000);
    }
    return NULL;
}

void miyoo_screen_init(void) {
    pthread_attr_t at;
    struct sched_param sp = { 0 };
    if (g_started) {
        return;
    }
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
    g_started = pthread_create(&g_th, &at, screen_thread, NULL) == 0;
    pthread_attr_destroy(&at);
}

/* menu: o valor do OnionOS (0..20) de uma das opcoes, para comecar dele ao sair de "sistema" */
int miyoo_screen_sys(const char* name) {
    int c, h, s;
    sys_values(&c, &h, &s);
    return !strcmp(name, "screen_contrast") ? c : !strcmp(name, "screen_hue") ? h : s;
}

/* ao fechar: os valores do sistema voltam (so se mudamos algo) */
void miyoo_screen_restore(void) {
    static const int none[3] = { -1, -1, -1 };
    int i;
    g_quit = 1;
    for (i = 0; i < 50 && pthread_mutex_trylock(&g_mx) != 0; i++) {
        usleep(10000); /* (a thread esta escrevendo agora: espera; trylock porque o fechamento por erro tambem chama) */
    }
    if (i == 50) {
        return;
    }
    if (g_on) {
        apply(none, 0);
        g_on = 0;
    }
    pthread_mutex_unlock(&g_mx);
}
