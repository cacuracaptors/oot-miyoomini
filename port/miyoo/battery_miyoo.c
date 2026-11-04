/*
 * battery_miyoo.c -- bateria fraca no OnionOS, como o sistema faz para os emuladores
 * (mesmo codigo do port do Mario Kart 64).
 *
 * Aviso: o batmon do OnionOS desenha um icone vermelho direto na tela abaixo do nivel
 * "avisar em"; como o jogo troca de pagina a cada quadro, o icone piscava. Um app que
 * mostra a bateria sozinho avisa o batmon com /tmp/hasBatteryDisplay; o jogo cria essa
 * marca enquanto roda e desenha o mesmo icone em cada quadro.
 *
 * Desligamento: abaixo do nivel "sair em", o keymon fecha o RetroArch e deixa
 * /tmp/.lowBat; o runtime.sh mostra a tela de bateria fraca e desliga. Um port nao e
 * fechado pelo keymon, entao o jogo faz o mesmo sozinho: deixa /tmp/.lowBat e sai
 * (o save do OoT ja fica gravado no arquivo a cada vez que o jogo salva).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "miyoo.h"

#define ONION_CONFIG "/mnt/SDCARD/.tmp_update/config/"
#define FLAG_OWN_DISPLAY "/tmp/hasBatteryDisplay"

static volatile int battery_low;
volatile int miyoo_battery_exit;
static int own_flag;

static int read_int(const char* path, int fallback) {
    FILE* f = fopen(path, "r");
    int v = fallback;
    if (!f) {
        return fallback;
    }
    if (fscanf(f, "%d", &v) != 1) {
        v = fallback;
    }
    fclose(f);
    return v;
}

static int exists(const char* path) {
    return access(path, F_OK) == 0;
}

static void* battery_proc(void* arg) {
    int logged = 0;
    (void)arg;
    for (;;) {
        int perc = read_int("/tmp/percBat", -1);
        int warn_at = exists(ONION_CONFIG ".noBatteryWarning") ? 0 : read_int(ONION_CONFIG "battery/warnAt", 15);
        int exit_at = exists(ONION_CONFIG ".noLowBatteryAutoSave") ? 0 : read_int(ONION_CONFIG "battery/exitAt", 4);
        int valid = perc >= 0 && perc <= 100; /* (500: carregando) */
        battery_low = valid && perc < warn_at;
        if (valid && exit_at > 0 && perc <= exit_at && !miyoo_battery_exit) {
            FILE* f = fopen("/tmp/.lowBat", "w");
            if (f) {
                fclose(f);
            }
            fprintf(stderr, "bateria: %d%% (sair em %d%%): fechando o jogo para o OnionOS desligar\n", perc, exit_at);
            miyoo_battery_exit = 1;
        }
        if (valid && battery_low && !logged) {
            fprintf(stderr, "bateria: %d%% (aviso em %d%%)\n", perc, warn_at);
            logged = 1;
        }
        sleep(2);
    }
    return NULL;
}

void miyoo_battery_init(void) {
    FILE* f;
    pthread_t th;
    if (!exists("/tmp/percBat") && !exists(ONION_CONFIG)) {
        return; /* (nao e o OnionOS) */
    }
    f = fopen(FLAG_OWN_DISPLAY, "w");
    if (f) {
        fclose(f);
        own_flag = 1;
    }
    if (pthread_create(&th, NULL, battery_proc, NULL) == 0) {
        pthread_detach(th);
    }
}

void miyoo_battery_release(void) {
    if (own_flag) {
        unlink(FLAG_OWN_DISPLAY);
    }
}

/* o icone do batmon (contorno vermelho, quase vazio), a 15 pixels do topo e da direita
 * da tela de 640x480, na escala do quadro */
void miyoo_battery_draw(uint16_t* buf, int w, int h) {
    const uint16_t red = 0xF800;
    const float s = (float)w / 640.0f;
    int x0, y0, bw, bh, hw, t, x, y;
    if (!battery_low || !buf) {
        return;
    }
#define PX(v) ((int)((v) * s + 0.5f))
    x0 = w - PX(15 + 30 + 4);
    y0 = PX(15);
    bw = PX(30);
    bh = PX(15);
    hw = PX(4);
    t = PX(1) > 0 ? PX(1) : 1;
    if (x0 < 0 || y0 + bh > h) {
        return;
    }
    for (y = 0; y < bh; y++) {
        uint16_t* row = buf + (y0 + y) * w + x0;
        int edge_y = y < t || y >= bh - t;
        for (x = 0; x < bw; x++) {
            if (edge_y || x < t || x >= bw - t) {
                row[x] = red;
            }
        }
        if (y >= PX(3) && y < bh - PX(3)) {
            for (x = PX(3); x < PX(3) + (PX(2) > 0 ? PX(2) : 1); x++) {
                row[x] = red;
            }
        }
        if (y >= PX(4) && y < bh - PX(4)) {
            for (x = bw; x < bw + hw; x++) {
                row[x] = red;
            }
        }
    }
#undef PX
}

/* a bateria esta abaixo do aviso (video_miyoo.c desenha o icone do batmon por cima de tudo) */
int miyoo_battery_low(void) {
    return battery_low;
}
