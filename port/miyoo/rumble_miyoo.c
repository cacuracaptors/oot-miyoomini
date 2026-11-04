/*
 * rumble_miyoo.c -- o motor de vibracao do Miyoo Mini (Plus) faz o papel do Rumble Pak do N64.
 *
 * O motor fica no GPIO 48, ligado com o valor 0 (como o OnionOS faz em src/common/system/rumble.h). Ele so liga e
 * desliga, como o Rumble Pak: o proprio jogo faz a intensidade ligando e desligando o motor a cada quadro
 * (z_rumble.c / padmgr.c). Com a opcao rumble = 0 o jogo ve o controle sem Rumble Pak (como no N64 sem ele);
 * a Pedra da Agonia so vibra com ele (a opcao visual_agony mostra um icone no lugar).
 *
 * Seguranca: o motor nunca fica ligado mais de 3 s seguidos (um jogo travado com ele ligado) e e desligado ao
 * fechar o jogo.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "miyoo.h"
#include "os_hw.h"

int port_opt_rumble = 1; /* options.txt: 1 = o aparelho vibra como o Rumble Pak; 0 = sem vibracao */

static int g_fd = -1;
static int g_state = -1; /* 1 ligado, 0 desligado, -1 nao sabido */
static double g_onSince;

static void write_str(const char* path, const char* v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, v, strlen(v)) < 0) {
            /* (ja exportado, por exemplo) */
        }
        close(fd);
    }
}

static void motor(int on) {
    if (g_fd < 0 || on == g_state) {
        return;
    }
    if (pwrite(g_fd, on ? "0" : "1", 1, 0) == 1) {
        g_state = on;
        if (on) {
            g_onSince = miyoo_now_ms();
        }
    }
}

static void hook_rumble(int on) {
    motor(on && port_opt_rumble);
}

static int hook_rumble_on(void) {
    return g_fd >= 0 && port_opt_rumble;
}

void miyoo_rumble_init(void) {
    write_str("/sys/class/gpio/export", "48");
    write_str("/sys/class/gpio/gpio48/direction", "out");
    g_fd = open("/sys/class/gpio/gpio48/value", O_WRONLY | O_CLOEXEC);
    if (g_fd < 0) {
        fprintf(stderr, "vibracao: sem o motor (GPIO 48)\n");
        return;
    }
    motor(0);
    port_hooks.rumble = hook_rumble;
    port_hooks.rumble_on = hook_rumble_on;
    fprintf(stderr, "vibracao: motor no GPIO 48 (%s)\n", port_opt_rumble ? "ligada" : "desligada na opcao rumble");
}

/* ~60 vezes por segundo (input_miyoo.c): o limite de 3 s ligado */
void miyoo_rumble_tick(void) {
    if (g_state == 1 && (!port_opt_rumble || miyoo_now_ms() - g_onSince > 3000.0)) {
        motor(0);
    }
}

/* ao fechar (e no tratador de falhas: so pwrite) */
void miyoo_rumble_off(void) {
    if (g_fd >= 0) {
        if (pwrite(g_fd, "1", 1, 0) != 1) {
            /* (nada a fazer) */
        }
        g_state = 0;
    }
}
