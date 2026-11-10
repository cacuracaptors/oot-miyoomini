/*
 * miyoo.h -- a camada do Miyoo Mini Plus (OnionOS): tela, botoes, som e bateria.
 * Mesmo esquema do port do Mario Kart 64 (mk64-dc/port, arquivos _miyoo.c), adaptado aos
 * ganchos de hardware deste port (platform/os_hw.h).
 */
#ifndef PORT_MIYOO_H
#define PORT_MIYOO_H

#include <stdint.h>

double miyoo_now_ms(void);

/* ---- video (video_miyoo.c) ---- */
/* Abre a tela (/dev/fb0) e o 2D do chip (MI_GFX). Pode ser chamada cedo, para as
 * telas de mensagem. */
void miyoo_video_init(void);
/* Liga a tela ao jogo: cada quadro novo do VI vai para a tela, e o vsync do painel
 * passa a ser o relogio do VI (se funcionar). Chamar antes de bootproc(). */
void miyoo_video_start(void);
/* Tela de mensagem (texto centralizado, linhas separadas por '\n'). Desenha direto
 * na tela; para usar antes do jogo comecar (ou depois de parar). */
void miyoo_video_message(const char* text, uint32_t rgb);
/* Deixa a tela preta (ao sair, para o OnionOS redesenhar o menu). */
void miyoo_video_shutdown(void);
extern int miyoo_opt_perf;   /* linhas de tempo no log a cada 2 s */
extern int miyoo_opt_nogfx;  /* nao usar o MI_GFX (copia pela CPU) */

/* ---- botoes (input_miyoo.c) ---- */
void miyoo_input_init(void);

/* ---- som (audio_miyoo.c) ---- */
/* Recebe o PCM estereo que o jogo manda ao AI (rate = frequencia do AI). */
void miyoo_audio_init(int mute);
void miyoo_audio_push(const int16_t* pcm, uint32_t frames, uint32_t rate);
void miyoo_audio_shutdown(void);

/* ---- bateria (battery_miyoo.c) ---- */
void miyoo_battery_init(void);
void miyoo_battery_release(void);
void miyoo_battery_draw(uint16_t* rgb565, int w, int h);
int miyoo_battery_low(void);

/* ---- vibracao (rumble_miyoo.c) ---- */
void miyoo_rumble_init(void);
void miyoo_rumble_tick(void);
void miyoo_rumble_off(void);

/* ---- menu de opcoes no jogo (menu_miyoo.c) ---- */
enum { MENU_K_UP, MENU_K_DOWN, MENU_K_LEFT, MENU_K_RIGHT, MENU_K_A, MENU_K_TAB_PREV, MENU_K_TAB_NEXT, MENU_K_CLOSE, MENU_K_X };
void miyoo_menu_status(const char* text);
void miyoo_menu_slots_changed(void);
/* atualizacoes (update_miyoo.c): o resultado de "Procurar atualizacoes" volta ao menu */
enum { MIYOO_UPD_LATEST, MIYOO_UPD_NEWER, MIYOO_UPD_NO_NET, MIYOO_UPD_ERROR };
void miyoo_menu_update_result(int code, const char* latest, const char* cur, const char* notes, int automatic);
int port_update_check_start(int automatic);
enum { MIYOO_UPST_IDLE, MIYOO_UPST_DOWNLOADING, MIYOO_UPST_PREPARING, MIYOO_UPST_READY, MIYOO_UPST_FAILED };
enum { MIYOO_UPERR_NONE, MIYOO_UPERR_NET, MIYOO_UPERR_SPACE, MIYOO_UPERR_DOWNLOAD, MIYOO_UPERR_VERIFY, MIYOO_UPERR_MEMORY,
       MIYOO_UPERR_EXTRACT, MIYOO_UPERR_NEEDROM, MIYOO_UPERR_INSTALL };
int port_update_download_start(void);
void port_update_cancel(void);
int port_update_has_package(void);
int port_update_state(long* got, long* total, int* err, int* needRom, int* hasRom);
void port_update_forget_error(void);
int port_update_install(void);
int miyoo_menu_active(void);
int miyoo_menu_dirty(void);
void miyoo_menu_open(void);
void miyoo_menu_close(void);      /* B/START: pergunta se reinicia, quando preciso */
void miyoo_menu_close_now(void);  /* fecha sem perguntar */
int miyoo_menu_text_width(const char* s);
int miyoo_menu_draw_text(uint32_t* page, int x, int y, const char* s, uint32_t c, int scale); /* UTF-8 e japones */
void miyoo_menu_draw_toast(uint32_t* page, const char* s); /* aviso curto no alto da tela (estado salvo...) */
const char* miyoo_menu_jp(const char* en);
const char* miyoo_menu_eu(const char* en, int lg); /* menu_miyoo.c: alemao (3) ou frances (4); NULL = nao ha */
void miyoo_restart(void);
void port_ss_restart_request(void);
void miyoo_menu_key(int k);
void miyoo_menu_render(uint32_t* page);
extern volatile int miyoo_battery_exit;

/* ---- saida (port_main.c): fecha tudo e termina o processo ---- */
void miyoo_quit(int code);

#endif
