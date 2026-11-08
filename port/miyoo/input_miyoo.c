/*
 * input_miyoo.c -- os botoes do Miyoo Mini Plus viram o controle 1 do N64.
 *
 * Le direto do evdev (/dev/input/event0), como no port do Mario Kart 64, toda vez
 * que o jogo pede os controles (osContStartReadData, uma vez por quadro).
 *
 *   Teclas personalizaveis (lote 197; opcoes button_*_tap / button_*_hold, menu Controles): cada botao (menos o MENU)
 *   tem uma acao num toque (solto em menos de 0,5 s, sem outro botao junto) e outra segurado (0,5 s ou mais). Padrao:
 *   A -> A                   B -> pulo (no jogo; nos menus/textos e o B)    Y -> B (espada)
 *   L2 -> C-esquerda         X -> C-baixo          R2 -> C-direita (segurado: andar devagar)
 *   toque no SELECT -> C-cima          L1 -> Z (mirar)          R1 -> R (escudo)
 *   START -> START           SELECT+L1 -> L (minimapa)
 *   SELECT segurado + direcional -> direcional do N64 (o port usa: direita troca as botas,
 *   esquerda troca a tunica; opcao atalho_roupa)
 *   direcional -> analogico (o OoT de varejo nao usa o D-pad do N64)
 *   MENU sozinho (sem nenhuma outra tecla junto): toque, segurar (0,7 s) e dois toques (em 0,3 s) fazem o que o
 *   Tweaks > Shortcuts do OnionOS manda para os jogos (padrao: Game Switcher, voltar ao menu, jogo anterior),
 *   salvando antes o estado para o Game Switcher continuar dele (port/savestate.c). Fora do OnionOS: toque fecha.
 *   MENU+POWER e o print de tela do OnionOS: nao faz nada disso.
 *   SELECT+cima / SELECT+baixo: brilho (gamma) +1 / -1 (0 a 10), gravado no options.txt.
 *   MENU+R2 / MENU+L2: salva / carrega o estado rapido (saves/state_quick.oss).
 *   SELECT+A segurados: pulo da lua (so com o cheat cheat_moon_jump_on_l).
 *   SELECT+Y: grava o proximo quadro (captura_NN.cap e .png) para depurar defeitos de imagem.
 *   SELECT+R1: grava a medicao detalhada dos nucleos (perf_NN.bin; so com a opcao perf_trace).
 */
#include <fcntl.h>
#include <math.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "miyoo.h"
#include "os_hw.h"

void gfx_request_capture(void); /* gfx/gfx_hook.c */
extern int port_opt_cheat_moon_jump_on_l, port_input_select_a; /* port_options.c */
extern int gx_brilho;            /* gfx/gfx_rdp.c */
extern int port_opt_segurar_b, port_text_open, port_text_choice; /* port_options.c */
void port_options_save(void);     /* port_options.c */
extern int port_opt_btn[20]; /* port_options.c: teclas personalizaveis (toque e segurado de cada botao) */
static int port_input_slow;    /* andar devagar agora (acao 12 de algum botao) */
/* andar devagar: o jogo tira 7 de cada eixo (zona morta do PadUtils_UpdateRelXY) e so anda com a forca (raiz de
 * x^2+y^2 dos eixos ja descontados) acima de 20. SLOW_REL e essa forca ja descontada; slow_axis devolve o valor cru
 * do eixo que da a componente pedida depois do desconto (assim as diagonais tambem andam). */
#define SLOW_REL 36.0f
static signed char slow_axis(float c) {
    if (c > -0.5f && c < 0.5f) {
        return 0;
    }
    return (signed char)(c > 0 ? lrintf(c) + 7 : lrintf(c) - 7);
}
extern int port_opt_manual_jump, port_input_jump, port_input_frames, port_jump_stamp, port_jump_mode;

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_START, K_SELECT, K_MENU, K_COUNT };

/* teclas personalizaveis: as acoes (os numeros das opcoes button_*_tap / _hold) */
enum { ACT_NONE, ACT_A, ACT_B, ACT_JUMP, ACT_Z, ACT_R, ACT_L, ACT_START, ACT_CUP, ACT_CDOWN, ACT_CLEFT, ACT_CRIGHT, ACT_SLOW,
       ACT_COUNT };
#define NBTN 10
static const int k_btnKey[NBTN] = { K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_START, K_SELECT }; /* (ordem das opcoes) */
#define BTN_HOLD_MS 500  /* segurado a partir daqui; antes disso (e solto) e um toque (o SELECT do port do Fallout) */
#define BTN_PULSE_MS 120 /* o toque fica apertado para o jogo esse tempo (ele le os controles a 20 Hz) */
typedef struct {
    long long t0, pulseUntil;
    int was, combo, holding, pulseAct;
} BtnState;
static BtnState g_btn[NBTN];
static int btn_act(int v) {
    return v >= 0 && v < ACT_COUNT ? v : ACT_NONE;
}
static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int port_opt_direcional_suave = 40; /* opcoes.txt: ms para a direcao girar 45 graus (0 = desligado) */
static int ev_fd = -1;
static unsigned char held[K_COUNT];
static unsigned char down_any[KEY_MAX / 8 + 1]; /* todas as teclas apertadas agora */
static int menu_down, menu_mixed;
static int start_eaten; /* o START do MENU+START (o "START sozinho" do menu nao conta) */
/* MENU sozinho, medido como o keymon do OnionOS mede para o RetroArch: segurar >= 0,7 s; um segundo toque em ate
 * 0,3 s depois do primeiro e o duplo (o toque simples so espera esse tempo quando o duplo faz alguma coisa) */
#define MENU_LONG_MS 700.0
#define MENU_DOUBLE_MS 300.0
static double menu_press_t;
static int menu_long_done, menu_single_wait, menu_skip_release;
void port_ss_menu(int press);        /* port/savestate.c: 0 toque, 1 segurar, 2 dois toques */
int port_ss_menu_double_used(void);
void port_ss_input_tick(void);
int port_res_hold; /* gfx_hires.c: resolucao dinamica parada (print de tela) */

static int key_index(int code) {
    switch (code) {
        case KEY_UP: return K_UP;
        case KEY_DOWN: return K_DOWN;
        case KEY_LEFT: return K_LEFT;
        case KEY_RIGHT: return K_RIGHT;
        case KEY_SPACE: return K_A;
        case KEY_LEFTCTRL: return K_B;
        case KEY_LEFTSHIFT: return K_X;
        case KEY_LEFTALT: return K_Y;
        case KEY_E: return K_L1;
        case KEY_T: return K_R1;
        case KEY_TAB: return K_L2;
        case KEY_BACKSPACE: return K_R2;
        case KEY_ENTER: return K_START;
        case KEY_RIGHTCTRL: return K_SELECT;
        case KEY_ESC: return K_MENU;
        default: return -1;
    }
}

static int others_down(void) {
    int i;
    for (i = 0; i < (int)sizeof down_any; i++) {
        unsigned char m = down_any[i];
        if (i == KEY_ESC / 8) {
            m &= (unsigned char)~(1u << (KEY_ESC % 8));
        }
        if (m) {
            return 1;
        }
    }
    return 0;
}

static void poll_keys(void) {
    struct input_event ev[32];
    if (ev_fd < 0) {
        return;
    }
    for (;;) {
        ssize_t r = read(ev_fd, ev, sizeof ev);
        int n, i;
        if (r <= 0) {
            break;
        }
        n = (int)(r / (ssize_t)sizeof ev[0]);
        for (i = 0; i < n; i++) {
            int code = ev[i].code, k;
            if (ev[i].type != EV_KEY) {
                continue;
            }
            if (code >= 0 && code < KEY_MAX) {
                if (ev[i].value) {
                    down_any[code / 8] |= (unsigned char)(1u << (code % 8));
                } else {
                    down_any[code / 8] &= (unsigned char)~(1u << (code % 8));
                }
            }
            if (code == KEY_ESC && ev[i].value == 1) { /* MENU apertado */
                const double now = miyoo_now_ms();
                if (menu_single_wait && now - menu_press_t <= MENU_DOUBLE_MS) {
                    menu_single_wait = 0; /* o segundo toque: duplo */
                    menu_skip_release = 1;
                    fprintf(stderr, "botoes: MENU (dois toques)\n");
                    port_ss_menu(2);
                } else {
                    menu_skip_release = 0;
                }
                menu_down = 1;
                menu_mixed = others_down();
                menu_long_done = 0;
                menu_press_t = now;
            } else if (code == KEY_ESC && ev[i].value == 0) { /* MENU solto */
                if (menu_down && !menu_mixed && !menu_long_done && !menu_skip_release && !others_down()) {
                    if (port_ss_menu_double_used()) {
                        menu_single_wait = 1; /* toque simples, a menos que venha um segundo */
                    } else {
                        fprintf(stderr, "botoes: MENU (toque)\n");
                        port_ss_menu(0);
                    }
                }
                menu_down = 0;
                menu_skip_release = 0;
            } else if (code != KEY_ESC && ev[i].value == 1 && menu_down) {
                menu_mixed = 1; /* outra tecla junto com o MENU (POWER: print do OnionOS) */
                if (code == KEY_ENTER) { /* MENU+START: menu de opcoes (menu_miyoo.c) */
                    start_eaten = 1; /* este START ja foi usado: nao fecha (nem responde "nao") de novo */
                    if (miyoo_menu_active()) {
                        miyoo_menu_close();
                    } else {
                        miyoo_menu_open();
                    }
                } else if (code == KEY_BACKSPACE) { /* MENU+R2: salva o estado rapido (port/savestate.c) */
                    extern volatile int port_ss_req;
                    port_ss_req = 1;
                } else if (code == KEY_TAB) { /* MENU+L2: carrega o estado rapido */
                    extern volatile int port_ss_req;
                    port_ss_req = 2;
                }
            }
            k = key_index(code);
            if (k >= 0) {
                held[k] = ev[i].value != 0; /* 1 aperta, 2 repete, 0 solta */
            }
        }
        if (n < 32) {
            break;
        }
    }
}

/* menu de opcoes aberto: os botoes vao para ele (com repeticao no direcional), nada chega ao jogo */
static int menu_fresh = 1; /* o menu acabou de abrir: o que ja estava apertado nao conta */
static void menu_input(void) {
    static unsigned char was[K_COUNT];
    static double next[K_COUNT];
    static const struct { int key, act, rep; } map[] = {
        { K_UP, MENU_K_UP, 1 },        { K_DOWN, MENU_K_DOWN, 1 },     { K_LEFT, MENU_K_LEFT, 1 },
        { K_RIGHT, MENU_K_RIGHT, 1 },  { K_A, MENU_K_A, 0 },           { K_L1, MENU_K_TAB_PREV, 0 },
        { K_R1, MENU_K_TAB_NEXT, 0 },  { K_L2, MENU_K_TAB_PREV, 0 },   { K_R2, MENU_K_TAB_NEXT, 0 },
        { K_B, MENU_K_CLOSE, 0 },
        { K_X, MENU_K_X, 0 },
    };
    const double now = miyoo_now_ms();
    int i;
    if (menu_fresh) {
        memcpy(was, held, sizeof was);
        menu_fresh = 0;
    }
    for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++) {
        const int k = map[i].key;
        if (menu_down) {
            was[k] = held[k]; /* (MENU segurado: MENU+L2/R2 sao do estado rapido) */
            continue;
        }
        if (held[k] && !was[k]) {
            miyoo_menu_key(map[i].act);
            next[k] = now + 350.0;
        } else if (held[k] && map[i].rep && now >= next[k]) {
            miyoo_menu_key(map[i].act);
            next[k] = now + 70.0;
        }
        was[k] = held[k];
    }
    {
        /* START sozinho tambem fecha (MENU+START e tratado na leitura dos eventos) */
        static unsigned char startWas = 1;
        if (!held[K_START]) {
            start_eaten = 0;
        }
        if (held[K_START] && !startWas && !menu_down && !start_eaten) {
            miyoo_menu_close();
        }
        startWas = held[K_START];
    }
}

static void read_pads(OSContPad pads[4]) {
    unsigned short b = 0;
    int dx = 0, dy = 0;
    memset(pads, 0, 4 * sizeof(OSContPad));
    poll_keys();
    if (miyoo_menu_active()) {
        menu_input();
        port_ss_input_tick();
        miyoo_rumble_tick();
        return;
    }
    menu_fresh = 1;
    {
        const double now = miyoo_now_ms();
        if (menu_down && !menu_mixed && !menu_long_done && !menu_skip_release && now - menu_press_t >= MENU_LONG_MS) {
            menu_long_done = 1; /* segurado */
            if (!others_down()) {
                fprintf(stderr, "botoes: MENU (segurado)\n");
                port_ss_menu(1);
            }
        }
        if (menu_single_wait && now - menu_press_t > MENU_DOUBLE_MS) {
            menu_single_wait = 0;
            fprintf(stderr, "botoes: MENU (toque)\n");
            port_ss_menu(0);
        }
    }
    port_ss_input_tick(); /* MENU, bateria no fim e desligar viram pedidos ao jogo (port/savestate.c) */
    miyoo_rumble_tick();  /* (o motor nunca fica ligado mais de 3 s seguidos) */

    /* teclas personalizaveis (opcoes button_*_tap / button_*_hold, port_options.c; menu Controles): cada botao (A, B, X,
     * Y, L1, R1, L2, R2, START, SELECT) tem uma acao no toque e outra segurado. Toque = solto antes de BTN_HOLD_MS e sem
     * outro botao junto: a acao vai ao jogo ao soltar, por BTN_PULSE_MS (o jogo le os controles a 20 Hz). Segurado =
     * BTN_HOLD_MS ou mais: a acao vale enquanto o botao estiver apertado. E o tempo do SELECT do port do Fallout (toque =
     * Esc, segurado = modificador). Com a mesma acao nos dois, o botao vale na hora, como um botao comum (sem espera).
     * Um botao usado numa combinacao (com o MENU, ou o SELECT com outro botao) nao faz nada sozinho.
     * O pulo (acao 3) so pula durante o jogo (o jogo avisa em port_jump_mode, a cada quadro, quando Link pode agir:
     * fora do menu de pausa e sem caixa de texto); nos menus, textos e telas fora do jogo ele e o B do N64 (cancelar,
     * passar texto). Os atalhos com SELECT segurado sao fixos (mais abaixo). */
    {
        const long long now = mono_ms();
        unsigned act = 0; /* (1 << ACT_*): as acoes ativas agora */
        int i, inPlay;
        port_input_frames++;
        port_opt_manual_jump = 0;
        for (i = 0; i < NBTN; i++) {
            const int k = k_btnKey[i];
            const int t = btn_act(port_opt_btn[2 * i]), h = btn_act(port_opt_btn[2 * i + 1]);
            BtnState* s = &g_btn[i];
            /* SELECT segurado: Y (captura), L1 (minimapa), R1 (medicao) e A (pulo da lua, com o cheat) sao atalhos */
            const int selCombo = k != K_SELECT && held[K_SELECT] &&
                                 (k == K_Y || k == K_L1 || k == K_R1 || (k == K_A && port_opt_cheat_moon_jump_on_l));
            if (t == ACT_JUMP || h == ACT_JUMP) {
                port_opt_manual_jump = 1; /* (z_player.c: com um botao de pulo, o pulo automatico e opcional) */
            }
            if (held[k] && !s->was) {
                s->t0 = now;
                s->combo = 0;
                s->holding = 0;
            }
            if (held[k]) {
                if (menu_down || selCombo) {
                    s->combo = 1;
                }
                if (k == K_SELECT) {
                    int j;
                    for (j = 0; j < K_COUNT; j++) {
                        if (j != K_SELECT && j != K_MENU && held[j]) {
                            s->combo = 1; /* SELECT com outro botao: atalho, nao toque */
                        }
                    }
                }
                if (t == h) {
                    if (!selCombo && !(k == K_SELECT && s->combo)) {
                        act |= 1u << t;
                    }
                } else {
                    if (!s->combo && now - s->t0 >= BTN_HOLD_MS) {
                        s->holding = 1;
                    }
                    if (s->holding && !selCombo) {
                        act |= 1u << h;
                    }
                }
            } else if (s->was && t != h && !s->combo && !s->holding && now - s->t0 < BTN_HOLD_MS) {
                s->pulseAct = t; /* toque */
                s->pulseUntil = now + BTN_PULSE_MS;
            }
            s->was = held[k];
            if (now < s->pulseUntil) {
                act |= 1u << s->pulseAct;
            }
        }
        inPlay = port_jump_mode && (port_input_frames - port_jump_stamp) <= 12; /* (leituras a 60 Hz, jogo a 20) */
        {
            /* o pulo apertado como B (texto, menu, cena) continua B ate ser solto: segurar passa varias falas seguidas
             * sem parar no intervalo entre uma caixa e outra (quando o jogo volta a liberar o pulo por um quadro) */
            static int asB;
            if (!(act & (1u << ACT_JUMP))) {
                asB = 0;
            } else if (!inPlay) {
                asB = 1;
            }
            if (asB) {
                inPlay = 0;
            }
        }
        port_input_jump = (act & (1u << ACT_JUMP)) && inPlay;
        if ((act & (1u << ACT_JUMP)) && !inPlay) b |= PORT_BTN_B;
        if (act & (1u << ACT_A)) b |= PORT_BTN_A;
        if (act & (1u << ACT_B)) b |= PORT_BTN_B;
        if (act & (1u << ACT_Z)) b |= PORT_BTN_Z;
        if (act & (1u << ACT_R)) b |= PORT_BTN_R;
        if (act & (1u << ACT_L)) b |= PORT_BTN_L;
        if (act & (1u << ACT_START)) b |= PORT_BTN_START;
        if (act & (1u << ACT_CUP)) b |= PORT_BTN_CUP;
        if (act & (1u << ACT_CDOWN)) b |= PORT_BTN_CDOWN;
        if (act & (1u << ACT_CLEFT)) b |= PORT_BTN_CLEFT;
        if (act & (1u << ACT_CRIGHT)) b |= PORT_BTN_CRIGHT;
        port_input_slow = (act & (1u << ACT_SLOW)) != 0; /* andar devagar (o direcional, logo abaixo) */
    }
    {
        /* segurar B nas falas (opcao hold_b_fast_text): depois de meio segundo segurando, com uma caixa de texto
         * aberta (que nao seja de escolha), o B vira toques seguidos (solto 1 leitura a cada 3). Assim toda parte do
         * jogo que espera um toque novo (fim da mensagem, cenas, atores) passa sozinha, como tocar B rapido. */
        static int bHeldReads;
        if ((b & PORT_BTN_B) && port_opt_segurar_b && port_text_open && !port_text_choice &&
            (port_input_frames - port_jump_stamp) <= 12) {
            bHeldReads++;
            if (bHeldReads > 10 && (bHeldReads % 3) == 0) {
                b &= ~PORT_BTN_B;
            }
        } else if (!(b & PORT_BTN_B)) {
            bHeldReads = 0;
        }
    }
    {
        /* SELECT+Y: grava o proximo quadro (captura_NN.cap/.png na pasta do jogo) para depurar;
         * com SELECT apertado o Y nao chega ao jogo */
        static int cap_was;
        int cap = held[K_SELECT] && held[K_Y];
        if (cap && !cap_was) {
            gfx_request_capture();
        }
        cap_was = cap;
    }

    dx = (held[K_RIGHT] ? 1 : 0) - (held[K_LEFT] ? 1 : 0);
    dy = (held[K_UP] ? 1 : 0) - (held[K_DOWN] ? 1 : 0);
    {
        /* atalhos fixos com SELECT segurado: o direcional vira o D-pad do N64 (e Link fica parado), cima/baixo mudam o
         * brilho, L1 liga/desliga o minimapa (um toque no L, que dura 120 ms para o jogo ver), R1 grava a medicao */
        static long long l_until;
        const long long now = mono_ms();
        int sel = held[K_SELECT];
        if (sel) {
            static int down_was;
            static int up_was;
            static int l1_was;
            if (held[K_UP]) b |= PORT_BTN_DUP;
            if (held[K_UP] && !up_was) { /* SELECT+cima: brilho (gamma) +1, ate 10; fica gravado no options.txt */
                if (gx_brilho < 10) {
                    gx_brilho++;
                    port_options_save();
                }
            }
            up_was = held[K_UP];
            if (held[K_DOWN] && !down_was) { /* SELECT+baixo: brilho (gamma) -1, ate 0 */
                if (gx_brilho > 0) {
                    gx_brilho--;
                    port_options_save();
                }
            }
            down_was = held[K_DOWN];
            if (held[K_L1] && !l1_was) {
                l_until = now + 120; /* SELECT+L1: minimapa (L) */
            }
            l1_was = held[K_L1];
            if (held[K_LEFT]) b |= PORT_BTN_DLEFT;
            if (held[K_RIGHT]) b |= PORT_BTN_DRIGHT;
            {
                /* SELECT+R1: grava a medicao detalhada dos nucleos (opcao perf_trace) */
                static int r1_was;
                if (held[K_R1] && !r1_was) {
                    extern int port_opt_perf_trace;
                    extern void pt_request_dump(void);
                    if (port_opt_perf_trace) {
                        pt_request_dump();
                    }
                }
                r1_was = held[K_R1];
            }
            dx = dy = 0;
        }
        port_input_select_a = sel && held[K_A];
        if (now < l_until) b |= PORT_BTN_L;
    }
    {
        /* ocarina na mao (o som da ocarina ligado, src/audio/game/general.c): o direcional toca as notas dos botoes C
         * (cima/baixo/esquerda/direita = C-cima/baixo/esquerda/direita) e Link nao anda nem entorta a nota; o START
         * fecha a ocarina (como o B). Os outros botoes continuam iguais. Com SELECT apertado, o SELECT+direcional de
         * sempre (brilho...). */
        extern s8 sOcarinaInstrumentId;
        extern u8 sIsOcarinaInputEnabled;
        if (sOcarinaInstrumentId != 0 && !held[K_SELECT]) {
            /* o que a tela mostra e o que se aperta: A = nota A, B = fecha (B do N64), direcional = notas C; os outros
             * botoes (X, Y, toque no SELECT) nao fazem nada enquanto a ocarina esta na mao */
            b &= ~(PORT_BTN_A | PORT_BTN_B | PORT_BTN_CUP | PORT_BTN_CDOWN | PORT_BTN_CLEFT | PORT_BTN_CRIGHT);
            if (held[K_A]) b |= PORT_BTN_A;
            if (held[K_B]) b |= PORT_BTN_B;
            if (held[K_UP]) b |= PORT_BTN_CUP;
            if (held[K_DOWN]) b |= PORT_BTN_CDOWN;
            if (held[K_LEFT]) b |= PORT_BTN_CLEFT;
            if (held[K_RIGHT]) b |= PORT_BTN_CRIGHT;
            /* lote 127: os gatilhos fazem o Z e o R do N64 na ocarina (src/audio/game/general.c): L1 ou L2 = Z (a nota
             * desce meio tom), R1 ou R2 = R (a nota sobe meio tom), segurados junto com a nota, para tocar musicas
             * de verdade. O B continua fechando. */
            b &= ~(PORT_BTN_Z | PORT_BTN_R);
            if (held[K_L1] || held[K_L2]) b |= PORT_BTN_Z;
            if (held[K_R1] || held[K_R2]) b |= PORT_BTN_R;
            dx = dy = 0;
            port_input_jump = 0; /* (o B nao pula) */
            if (sIsOcarinaInputEnabled && held[K_START]) {
                b = (b & ~PORT_BTN_START) | PORT_BTN_B;
            }
        }
    }
    if (menu_down) {
        b = 0; /* nada chega ao jogo enquanto o MENU esta apertado */
        dx = dy = 0;
    }
    {
        /* print de tela do OnionOS (MENU+POWER): enquanto o MENU esta apertado e por ~3 s depois do print, a resolucao
         * dinamica nao muda (gfx_hires.c): o programa do print rouba processador, o desenho atrasa e a resolucao caia
         * antes de a tela ser lida (o print saia na resolucao baixa). Alguns quadros atrasados nesse meio tempo. */
        static int s_menuWas, s_holdUntil;
        if (menu_down && menu_mixed) {
            s_holdUntil = port_input_frames + 180;
        }
        port_res_hold = menu_down || (s_menuWas && menu_mixed) || (port_input_frames - s_holdUntil) < 0;
        s_menuWas = menu_down;
    }

    pads[0].button = b;
    if (port_opt_direcional_suave > 0) {
        /* direcional_suave: so a DIRECAO gira aos poucos (45 graus a cada N ms) ate a do direcional; a forca
         * (76, a mesma nas 8 direcoes) e a parada sao na hora. Partindo do zero ou virando mais de 90 graus
         * (meia-volta), vai direto, como o analogico de verdade passando pelo meio. */
        static float ang;
        static int moving;
        static long long last;
        struct timespec ts;
        long long now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (!dx && !dy) {
            moving = 0;
            pads[0].stick_x = 0;
            pads[0].stick_y = 0;
        } else {
            const float target = atan2f((float)dy, (float)dx);
            float d = target - ang;
            while (d > (float)M_PI) d -= 2.0f * (float)M_PI;
            while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
            if (!moving || fabsf(d) > (float)M_PI * 0.51f) {
                ang = target;
            } else {
                long long dt = now - last;
                float step;
                if (dt > 100) dt = 100;
                if (dt < 0) dt = 0;
                step = (float)M_PI / 4.0f * (float)dt / (float)port_opt_direcional_suave;
                ang += fabsf(d) <= step ? d : (d > 0 ? step : -step);
            }
            moving = 1;
            if (port_input_slow) { /* R2 segurado: andar devagar */
                pads[0].stick_x = slow_axis(SLOW_REL * cosf(ang));
                pads[0].stick_y = slow_axis(SLOW_REL * sinf(ang));
            } else {
                pads[0].stick_x = (s8)lrintf(76.0f * cosf(ang));
                pads[0].stick_y = (s8)lrintf(76.0f * sinf(ang));
            }
        }
        last = now;
    } else if (port_input_slow && !held[K_SELECT]) { /* R2 segurado: andar devagar (analogico pouco inclinado) */
        const float k = (dx && dy) ? SLOW_REL * 0.70710678f : SLOW_REL;
        pads[0].stick_x = slow_axis(k * (float)dx);
        pads[0].stick_y = slow_axis(k * (float)dy);
    } else if (dx && dy) { /* diagonal: mesma forca que uma direcao so */
        pads[0].stick_x = (s8)(dx * 54);
        pads[0].stick_y = (s8)(dy * 54);
    } else {
        pads[0].stick_x = (s8)(dx * 76);
        pads[0].stick_y = (s8)(dy * 76);
    }
}

void miyoo_input_init(void) {
    ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ev_fd < 0) {
        fprintf(stderr, "botoes: sem /dev/input/event0\n");
    }
    port_hooks.read_pads = read_pads;
    port_hooks.pads_connected = 1;
}
