/*
 * menu_miyoo.c -- menu de opcoes dentro do jogo (MENU+START): as opcoes do options.txt que valem na hora.
 *
 * Abre por cima do jogo, que fica parado enquanto o menu esta aberto (graph.c: Port_MenuFreeze; a musica continua).
 *   L1 / R1            troca de aba (Estados, Geral, Controles, Melhorias, Correcoes, Trapacas, Graficos)
 *   cima / baixo       escolhe a opcao (segurar repete)
 *   esquerda / direita muda o valor (liga/desliga, ou anda na barra)
 *   A                  liga/desliga (ou avanca o valor)
 *   B, START ou MENU+START fecha e grava o options.txt
 * Ficam de fora as que so valem ao abrir o jogo (idioma, clock, texturas novas, filtro das texturas) e as de
 * desempenho e diagnostico (para quem mede o port; continuam no options.txt).
 *
 * Aba Estados: 10 estados salvos (saves/state_01..10.oss, com miniatura .thm), separados do estado rapido
 * (MENU+R2/L2). A carrega (e fecha o menu), X salva (X de novo para gravar por cima de um que ja existe). A
 * operacao e feita pelo jogo, no ponto seguro (graph.c: Port_MenuFreeze / Port_SsRun; port/savestate.c).
 *
 * O desenho: a thread da tela guarda a ultima imagem do jogo, escurecida, e desenha o menu por cima a cada mudanca
 * (video_miyoo.c chama miyoo_menu_render). Botoes e menu rodam na thread dos controles (input_miyoo.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>

#include "miyoo.h"
#include "font8x16.h"
#include "menu_textos.h" /* portugues e japones (gerado por tools/oot-menu-textos.py) */

int* port_option_ptr(const char* name);   /* port_options.c */
int port_option_default(const char* name, int* out);
void port_options_default_diagnostics(void);
const char* port_option_help(const char* name);
void port_options_save(void);
void port_ss_slot_request(int save, int slot); /* port/savestate.c */
void port_ss_restart_request(void);
void port_ss_quit_request(void);
void port_ss_reset_request(void);
int port_ss_slot_info(int slot, long* when);
int port_ss_slot_thumb(int slot, uint16_t* out);
extern int port_opt_idioma;

#define W 640
#define H 480

enum { TAB_SAVE, TAB_LOAD, TAB_GENERAL, TAB_CONTROLS, TAB_ENH, TAB_FIX, TAB_CHEATS, TAB_GFX, TAB_COUNT };
#define IS_STATES(t) ((t) == TAB_SAVE || (t) == TAB_LOAD)

typedef struct {
    int tab;
    const char* name; /* nome no options.txt */
    const char* en;
    const char* pt;
    int min, max, step;
    int restart; /* so vale ao reabrir o jogo: ao fechar o menu, pergunta se reinicia agora */
} Item;

#define T(tab, name, en, pt) { tab, name, en, pt, 0, 1, 1, 0 }
#define R(tab, name, en, pt, mn, mx, st) { tab, name, en, pt, mn, mx, st, 0 }
#define RR(tab, name, en, pt, mn, mx, st) { tab, name, en, pt, mn, mx, st, 1 }

static const Item k_items[] = {
    RR(TAB_GENERAL, "language", "Language (restarts the game)", "Idioma (reinicia o jogo)", 0, 6, 1),
    RR(TAB_GENERAL, "cpu_clock", "CPU clock (MHz, restarts)", "Clock da CPU (MHz, reinicia)", 1200, 1800, 100),
    R(TAB_GENERAL, "autosave", "Autosave (minutes)", "Salvar sozinho (minutos)", 0, 30, 1),
    R(TAB_GENERAL, "menu_theme", "Menu colors", "Cores do menu", 0, 1, 1),
    R(TAB_GENERAL, "text_speed", "Text speed", "Velocidade do texto", 1, 5, 1),
    R(TAB_GENERAL, "music_volume", "Music volume (%)", "Volume da música (%)", 0, 150, 10),
    R(TAB_GENERAL, "sound_volume", "Sound effects volume (%)", "Volume dos efeitos (%)", 0, 150, 10),
    T(TAB_GENERAL, "hold_b_fast_text", "Hold B to fast-forward text", "Segurar B acelera o texto"),
    T(TAB_GENERAL, "skip_cutscenes", "START skips cutscenes", "START pula cenas"),
    { TAB_GENERAL, "@update", "Check for updates", "Procurar atualizações", -1, -1, 0, 0 },
    { TAB_GENERAL, "@restore", "Restore defaults (all tabs)", "Restaurar padrão (todas as abas)", -1, -1, 0, 0 },
    { TAB_GENERAL, "@reset", "Restart the game", "Reiniciar o jogo", -1, -1, 0, 0 },
    { TAB_GENERAL, "@quit", "Quit the game", "Sair do jogo", -1, -1, 0, 0 },

    T(TAB_CONTROLS, "rumble", "Vibration (Rumble Pak)", "Vibração (Rumble Pak)"),
    R(TAB_CONTROLS, "dpad_smooth_turn", "Smooth D-pad turn (ms)", "Direcional suave (ms)", 0, 200, 10),
    T(TAB_CONTROLS, "natural_aiming", "Natural aiming (up looks up)", "Mira natural (cima olha p/ cima)"),
    T(TAB_CONTROLS, "smooth_aiming", "Smooth aiming", "Mira suave"),
    T(TAB_CONTROLS, "auto_jump", "Automatic jump", "Pulo automático"),
    R(TAB_CONTROLS, "jump_strength", "Jump strength (x10)", "Força do pulo (x10)", 40, 120, 2),
    T(TAB_CONTROLS, "air_steering", "Mid-air steering", "Direção no ar"),
    T(TAB_CONTROLS, "ledge_grab", "Grab ledges when walking off", "Agarrar a borda ao sair dela"),
    R(TAB_CONTROLS, "sprint_speed", "Sprint speed (%)", "Corrida (% da velocidade)", 100, 250, 10),
    T(TAB_CONTROLS, "quick_gear_swap", "SELECT+Left/Right: tunic/boots", "SELECT+esq./dir.: túnica/botas"),
    /* teclas personalizaveis (lote 197): no fim da aba, debaixo de um titulo (linha "#": so o titulo, nao se escolhe);
     * toque e segurado de cada botao (input_miyoo.c); o nome vem de key_label */
    { TAB_CONTROLS, "#keys", "Buttons: tap and hold", "Teclas: toque e segurar", -1, -1, 0, 0 },
    R(TAB_CONTROLS, "button_hold_ms", "Hold time (ms)", "Tempo para segurar (ms)", 150, 600, 50),
    R(TAB_CONTROLS, "button_a_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_a_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_b_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_b_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_x_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_x_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_y_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_y_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_l1_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_l1_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_r1_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_r1_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_l2_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_l2_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_r2_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_r2_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_start_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_start_hold", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_select_tap", "", "", 0, 12, 1),
    R(TAB_CONTROLS, "button_select_hold", "", "", 0, 12, 1),
    { TAB_CONTROLS, "@keys", "Default buttons", "Teclas padrão", -1, -1, 0, 0 },

    T(TAB_ENH, "fast_chests", "Fast chests", "Baús rápidos"),
    T(TAB_ENH, "tot_medallion_colors", "Colored ToT medallions", "Medalhões coloridos (Templo)"),
    T(TAB_ENH, "better_bombchu_shopping", "Better bombchu shopping", "Comprar bombchus melhor"),
    T(TAB_ENH, "quit_fishing_at_door", "Quit fishing at the door", "Parar de pescar na porta"),
    T(TAB_ENH, "fast_ocarina_playback", "Fast ocarina playback", "Ocarina rápida"),
    T(TAB_ENH, "enemy_health_bar", "Enemy health bar", "Barra de vida do inimigo"),
    T(TAB_ENH, "low_health_beep", "Low health beep", "Alarme de vida baixa"),
    T(TAB_ENH, "disable_crit_wiggle", "No camera wiggle at low health", "Sem tremor com vida baixa"),
    T(TAB_ENH, "remember_save_location", "Remember save location", "Lembrar onde salvou"),
    T(TAB_ENH, "gs_token_count", "Gold Skulltula count", "Contagem de Skulltulas"),
    T(TAB_ENH, "heart_piece_count", "Piece of Heart count", "Contagem de pedaços de coração"),
    T(TAB_ENH, "heart_container_count", "Heart Container count", "Contagem de containers"),
    T(TAB_ENH, "better_owl", "Better owl", "Coruja melhor"),
    T(TAB_ENH, "instant_putaway", "Instant putaway", "Guardar item na hora"),
    R(TAB_ENH, "sleeping_waterfall", "Zora's Domain waterfall", "Cachoeira dos Zora", 0, 2, 1),
    T(TAB_ENH, "faster_heavy_block_lift", "Faster heavy lift", "Erguer pedras pesadas rápido"),
    T(TAB_ENH, "skip_breath_animation", "Skip breath animation", "Pular a respirada na água"),
    R(TAB_ENH, "climb_speed", "Climb speed (+N)", "Escalar mais rápido (+N)", 0, 10, 1),
    R(TAB_ENH, "block_push_speed", "Block push speed (+N)", "Empurrar blocos (+N)", 0, 10, 1),
    R(TAB_ENH, "crawl_speed", "Crawl speed (x)", "Engatinhar (x)", 1, 4, 1),
    R(TAB_ENH, "king_zora_speed", "King Zora speed (x)", "Rei Zora (x)", 1, 5, 1),
    T(TAB_ENH, "instant_scarecrow", "Instant scarecrow", "Espantalho na hora"),
    T(TAB_ENH, "no_skulltula_freeze", "No Skulltula freeze", "Skulltula não congela"),
    T(TAB_ENH, "skip_save_confirmation", "Skip save confirmation", "Pular 'jogo salvo'"),
    R(TAB_ENH, "forge_time_days", "Biggoron forge time (days)", "Forja do Biggoron (dias)", 0, 3, 1),
    T(TAB_ENH, "door_locks_both_sides", "Door chains on both sides", "Correntes dos dois lados"),
    T(TAB_ENH, "remember_minimap", "Remember minimap", "Lembrar o minimapa"),
    T(TAB_ENH, "dynamic_wallet_icon", "Dynamic wallet icon", "Ícone da carteira com a cor"),
    T(TAB_ENH, "visual_agony", "Visual Stone of Agony", "Pedra da Agonia visual"),
    T(TAB_ENH, "file_select_time_flow", "Time flows on file select", "Céu andando nos arquivos"),
    T(TAB_ENH, "no_dropped_ocarina_inputs", "No dropped ocarina notes", "Ocarina sem notas perdidas"),
    T(TAB_ENH, "persistent_masks", "Persistent masks", "Máscaras persistentes"),
    T(TAB_ENH, "nuts_explode_bombs", "Deku Nuts explode bombs", "Nozes explodem bombas"),
    T(TAB_ENH, "no_first_person_bombchus", "No first-person bombchus", "Bombchu sem 1ª pessoa"),
    T(TAB_ENH, "skip_arrow_animation", "Skip magic arrow animation", "Pular animação das flechas"),
    T(TAB_ENH, "fast_boomerang", "Instant boomerang recall", "Bumerangue volta na hora"),
    T(TAB_ENH, "better_roll", "Better roll", "Rolada melhor"),
    T(TAB_ENH, "faster_pause_menu", "Faster pause menu", "Menu de pausa rápido"),
    T(TAB_ENH, "faster_bottle_empty", "Faster bottle emptying", "Esvaziar garrafa rápido"),
    T(TAB_ENH, "faster_rupee_counter", "Faster rupee counter", "Contador de rupias rápido"),
    T(TAB_ENH, "faster_shadow_ship", "Faster Shadow Temple boat", "Barco da Sombra rápido"),
    T(TAB_ENH, "rebottle_blue_fire", "Rebottle blue fire", "Pegar fogo azul de volta"),
    T(TAB_ENH, "no_heart_animation", "Still life hearts", "Corações sem animação"),
    T(TAB_ENH, "disable_title_card", "No area name on entering", "Sem o nome da área ao entrar"),
    T(TAB_ENH, "dampe_all_night", "Dampe all night", "Dampé a noite toda"),
    T(TAB_ENH, "daytime_gold_skulltulas", "Night Skulltulas by day", "Skulltulas noturnas de dia"),
    T(TAB_ENH, "shops_open_all_hours", "Shops open all hours", "Lojas sempre abertas"),
    T(TAB_ENH, "fast_drops", "Skip pickup messages", "Pegar itens sem mensagem"),
    T(TAB_ENH, "skip_navi_forced_talk", "Skip Navi forced talk", "Navi sem fala forçada"),
    T(TAB_ENH, "skip_npc_forced_talk", "Skip NPC forced talk", "Personagens sem fala forçada"),
    T(TAB_ENH, "skip_warp_cutscenes", "Skip warp song scenes", "Pular cena do teleporte"),
    T(TAB_ENH, "skip_jabu_fish", "Skip feeding Jabu-Jabu", "Jabu-Jabu sem peixe"),
    T(TAB_ENH, "skip_child_stealth", "Skip child stealth", "Pular os guardas do castelo"),
    T(TAB_ENH, "skip_tower_escape", "Skip tower escape", "Pular a fuga da torre"),
    T(TAB_ENH, "market_sneak", "Leave the Market at night", "Sair do mercado à noite"),
    T(TAB_ENH, "arrow_cycle", "Arrow cycle (R1 while aiming)", "Trocar flecha (R1 mirando)"),
    T(TAB_ENH, "bow_reticle", "Bow and slingshot reticle", "Mira no arco e estilingue"),
    T(TAB_ENH, "hookshot_reticle_color", "Green Hookshot reticle", "Mira verde no gancho"),
    T(TAB_ENH, "time_travel", "Song of Time time travel", "Viajar no tempo com a ocarina"),
    T(TAB_ENH, "pause_warp", "Warp songs from the pause menu", "Teleporte pelo menu de pausa"),
    T(TAB_ENH, "mask_select", "Mask select", "Escolher a máscara"),
    T(TAB_ENH, "mm_bunny_hood", "Majora's Mask Bunny Hood", "Máscara de coelho do Majora"),
    T(TAB_ENH, "ask_to_equip", "Ask to equip new items", "Perguntar para equipar"),
    T(TAB_ENH, "minimal_ui", "Minimal UI", "Interface mínima"),
    T(TAB_ENH, "better_farore", "Better Farore's Wind", "Vento de Farore melhor"),
    T(TAB_ENH, "fast_farores", "Faster Farore's Wind", "Vento de Farore rápido"),
    T(TAB_ENH, "red_ganon_blood", "Red Ganon blood", "Sangue vermelho do Ganon"),
    T(TAB_ENH, "pulsate_boss_icon", "Pulsating boss icon", "Ícone do chefe pulsando"),
    R(TAB_ENH, "frogs_note_timer", "Frogs: time per note (x)", "Sapos: tempo por nota (x)", 1, 5, 1),
    T(TAB_ENH, "lost_woods_customize", "Lost Woods: custom game", "Bosque Perdido: personalizar"),
    R(TAB_ENH, "lost_woods_starting_notes", "Lost Woods: starting notes", "Bosque: notas iniciais", 1, 8, 1),
    R(TAB_ENH, "lost_woods_round1_notes", "Lost Woods: round 1 notes", "Bosque: notas rodada 1", 1, 8, 1),
    R(TAB_ENH, "lost_woods_round2_notes", "Lost Woods: round 2 notes", "Bosque: notas rodada 2", 1, 8, 1),
    R(TAB_ENH, "lost_woods_round3_notes", "Lost Woods: round 3 notes", "Bosque: notas rodada 3", 1, 8, 1),
    T(TAB_ENH, "fishing_guarantee_bite", "Fishing: fish always bite", "Pesca: peixe sempre morde"),
    T(TAB_ENH, "fishing_fish_never_escape", "Fishing: fish never escape", "Pesca: peixe não escapa"),
    R(TAB_ENH, "fishing_min_weight_child", "Fishing: child prize (lbs)", "Pesca: prêmio criança (lbs)", 0, 20, 1),
    R(TAB_ENH, "fishing_min_weight_adult", "Fishing: adult prize (lbs)", "Pesca: prêmio adulto (lbs)", 0, 20, 1),

    T(TAB_FIX, "fix_gravedigging_tour", "Gravedigging tour", "Escavação do Dampé"),
    T(TAB_FIX, "fix_floor_switches", "Raised floor switches", "Botões de chão altos"),
    T(TAB_FIX, "fix_zora_hint", "Zora hint dialogue", "Dica do Zora"),
    T(TAB_FIX, "fix_vine_fall", "Falling from vine edges", "Queda na beira de trepadeira"),
    T(TAB_FIX, "fix_enemy_spawns_water", "Enemies near water", "Inimigos perto da água"),
    T(TAB_FIX, "fix_deku_nut_upgrade", "Deku Nut upgrade", "Melhoria das nozes"),
    T(TAB_FIX, "fix_broken_giants_knife", "Broken Giant's Knife", "Faca do gigante quebrada"),
    T(TAB_FIX, "fix_camera_drift", "Camera drift", "Câmera escorregando"),
    T(TAB_FIX, "fix_camera_swing", "Camera swing", "Câmera girando na parede"),
    T(TAB_FIX, "fix_camera_surface_hop", "Camera hop when surfacing", "Câmera pulando ao emergir"),
    T(TAB_FIX, "fix_ledge_swing_rate", "Ledge camera swing", "Câmera na beirada"),
    T(TAB_FIX, "fix_menu_lr", "Pause menu L/R color", "Cor do L/R no menu"),
    T(TAB_FIX, "fix_dungeon_entrance_icons", "Dungeon entrance icons", "Ícones de entrada de masmorra"),
    T(TAB_FIX, "fix_two_handed_idle", "Two-handed idle pose", "Pose com espada de 2 mãos"),
    T(TAB_FIX, "fix_gerudo_warrior_clothes", "Gerudo warrior clothes", "Roupa da guerreira Gerudo"),
    T(TAB_FIX, "fix_eyes_open_sleeping", "Link's eyes while sleeping", "Olhos do Link dormindo"),
    T(TAB_FIX, "fix_silver_rupee_jingle", "Silver rupee jingle", "Som das rúpias de prata"),
    T(TAB_FIX, "fix_darunia_dance", "Darunia's dance speed", "Dança do Darunia"),
    T(TAB_FIX, "fix_vanishing_paths", "Vanishing paths", "Estradas que somem"),

    T(TAB_CHEATS, "cheat_infinite_money", "Infinite money", "Dinheiro infinito"),
    T(TAB_CHEATS, "cheat_infinite_health", "Infinite health", "Vida infinita"),
    T(TAB_CHEATS, "cheat_infinite_ammo", "Infinite ammo", "Munição infinita"),
    T(TAB_CHEATS, "cheat_infinite_magic", "Infinite magic", "Magia infinita"),
    T(TAB_CHEATS, "cheat_infinite_nayrus_love", "Infinite Nayru's Love", "Amor de Nayru infinito"),
    T(TAB_CHEATS, "cheat_infinite_epona_boost", "Infinite Epona boost", "Cenouras da Epona infinitas"),
    T(TAB_CHEATS, "cheat_freeze_time", "Freeze time of day", "Parar o relógio do dia"),
    T(TAB_CHEATS, "cheat_no_item_restrictions", "No item restrictions", "Sem restrição de itens"),
    T(TAB_CHEATS, "cheat_disable_sandstorm", "No sandstorm", "Sem tempestade de areia"),
    T(TAB_CHEATS, "cheat_moon_jump_on_l", "Moon jump (SELECT+A)", "Pulo da lua (SELECT+A)"),
    R(TAB_CHEATS, "cheat_bomb_timer_percent", "Bomb fuse (%)", "Pavio das bombas (%)", 50, 300, 25),
    R(TAB_CHEATS, "cheat_hookshot_reach_percent", "Hookshot reach (%)", "Alcance do gancho (%)", 100, 300, 25),
    T(TAB_CHEATS, "cheat_no_redead_freeze", "No ReDead freeze", "ReDead não paralisa"),
    T(TAB_CHEATS, "cheat_fireproof_deku_shield", "Fireproof Deku Shield", "Escudo Deku não queima"),
    R(TAB_CHEATS, "cheat_deku_stick", "Deku Stick", "Graveto Deku", 0, 2, 1),
    T(TAB_CHEATS, "cheat_hookshot_everything", "Hookshot everything", "Gancho em tudo"),
    T(TAB_CHEATS, "cheat_super_tunic", "Super tunic", "Super túnica"),
    T(TAB_CHEATS, "cheat_shield_two_handed", "Shield with two-handed weapons", "Escudo com arma de 2 mãos"),
    T(TAB_CHEATS, "cheat_drops_dont_despawn", "Drops never disappear", "Itens no chão não somem"),
    T(TAB_CHEATS, "cheat_no_fish_despawn", "Released fish stay", "Peixes soltos ficam"),
    T(TAB_CHEATS, "cheat_no_bugs_despawn", "Released bugs stay", "Insetos soltos ficam"),
    T(TAB_CHEATS, "cheat_gs_targetable", "Target Gold Skulltulas", "Mirar Skulltulas douradas"),
    T(TAB_CHEATS, "cheat_no_keese_guay_target", "Keese/Guay don't attack", "Keese/Guay não atacam"),
    T(TAB_CHEATS, "cheat_dampe_always_win", "Dampe always wins", "Dampé sempre premia"),

    T(TAB_GFX, "frame_interpolation", "30 fps (in-between frames)", "30 fps (quadros extras)"),
    T(TAB_GFX, "frame_rate_steps", "Frame rate steps (30..20)", "Degraus de fps (30..20)"),
    R(TAB_GFX, "brightness", "Brightness (gamma)", "Brilho (gamma)", 0, 10, 1),
    R(TAB_GFX, "retro_filter", "Retro filter", "Filtro retrô", 0, 2, 1),
    R(TAB_GFX, "retro_filter_strength", "Retro filter strength (%)", "Força do filtro retrô (%)", 25, 75, 5),
    T(TAB_GFX, "retro_filter_tv", "Retro filter: TV screen", "Filtro retrô: tela de TV"),
    R(TAB_GFX, "max_resolution", "Maximum resolution (width)", "Resolução máxima (largura)", 320, 640, 32),
    R(TAB_GFX, "resolution_target_ms", "Dynamic resolution target (ms)", "Alvo da resolução dinâmica (ms)", 20, 40, 1),
    R(TAB_GFX, "dither", "Dithering", "Pontilhado de cor", 0, 2, 1),
    RR(TAB_GFX, "texture_filter", "Texture filter (restarts)", "Filtro das texturas (reinicia)", 0, 2, 1),
    RR(TAB_GFX, "exact_filter_close", "Finer filter up close (restarts)", "Filtro fino de perto (reinicia)", 0, 16, 1),
    RR(TAB_GFX, "close_filter_mode", "Up-close filter (restarts)", "Filtro de perto (reinicia)", 0, 1, 1),
    T(TAB_GFX, "sharp_text_boxes", "Sharp text boxes", "Caixas de texto nítidas"),
    T(TAB_GFX, "opaque_text_boxes", "Opaque text boxes", "Caixas de texto opacas"),
    T(TAB_GFX, "opaque_pause_menu", "Opaque pause menu", "Menu de pausa opaco"),
    T(TAB_GFX, "light_hit_marks", "Lighter hit flash", "Brilho de acerto leve"),
    T(TAB_GFX, "lens_flare", "Sun lens flare", "Reflexo do sol na lente"),
    R(TAB_GFX, "light_halo_tolerance", "Light halo tolerance", "Folga dos halos de luz", 0, 2000, 100),
};
#define NITEMS ((int)(sizeof k_items / sizeof k_items[0]))

static const char* const k_tabEn[TAB_COUNT] = { "Save", "Load", "General", "Controls", "Enhance", "Fixes", "Cheats", "Graphics" };
static const char* const k_tabPt[TAB_COUNT] = { "Salvar", "Carregar", "Geral", "Controles", "Melhorias", "Correções", "Trapaças", "Gráficos" };

static volatile int g_dirty; /* o menu precisa ser redesenhado */

/* ---- idioma do menu: o da opcao language (0 ingles, 1 portugues, 2 japones, 3 alemao, 4 frances, 5 italiano,
 * 6 espanhol) ---- */
static int lang(void) {
    return port_opt_idioma >= 1 && port_opt_idioma <= 6 ? port_opt_idioma : 0;
}

/* alemao (lg 3), frances (lg 4), italiano (lg 5) ou espanhol (lg 6) de um texto fixo (pela chave em ingles); NULL = nao ha */
const char* miyoo_menu_eu(const char* en, int lg) {
    int i;
    if (lg < 3 || lg > 6) {
        return NULL;
    }
    for (i = 0; i < (int)(sizeof k_ui_eu / sizeof k_ui_eu[0]); i++) {
        if (!strcmp(k_ui_eu[i][0], en)) {
            return k_ui_eu[i][lg - 2];
        }
    }
    return NULL;
}

/* japones de um texto fixo (pela chave em ingles); NULL = nao ha */
const char* miyoo_menu_jp(const char* en) {
    int i;
    for (i = 0; i < (int)(sizeof k_ui_jp / sizeof k_ui_jp[0]); i++) {
        if (!strcmp(k_ui_jp[i][0], en)) {
            return k_ui_jp[i][1];
        }
    }
    return NULL;
}

/* texto fixo no idioma do menu */
static const char* tr(const char* en, const char* pt) {
    if (lang() == 2) {
        const char* jp = miyoo_menu_jp(en);
        return jp != NULL ? jp : en;
    }
    if (lang() >= 3) {
        const char* eu = miyoo_menu_eu(en, lang());
        return eu != NULL ? eu : en;
    }
    return lang() == 1 ? pt : en;
}

static const MenuTexto* texto(const char* name) {
    int i;
    for (i = 0; i < (int)(sizeof k_textos / sizeof k_textos[0]); i++) {
        if (!strcmp(k_textos[i].name, name)) {
            return &k_textos[i];
        }
    }
    return NULL;
}

/* teclas personalizaveis: "A: toque" / "A: segurar" (o nome do botao e igual em todos os idiomas) */
static const char* key_label(const Item* it) {
    static char buf[24][64];
    static int next;
    static const char* const k_keys[][2] = { { "a", "A" },   { "b", "B" },   { "x", "X" },         { "y", "Y" },
                                             { "l1", "L1" }, { "r1", "R1" }, { "l2", "L2" },       { "r2", "R2" },
                                             { "start", "START" },           { "select", "SELECT" } };
    const char* p = it->name + 7; /* (depois de "button_") */
    const char* u = strrchr(p, '_');
    char* b = buf[next++ % 24];
    const char* kn = "?";
    int i;
    for (i = 0; u != NULL && i < (int)(sizeof k_keys / sizeof k_keys[0]); i++) {
        if (strlen(k_keys[i][0]) == (size_t)(u - p) && !strncmp(p, k_keys[i][0], (size_t)(u - p))) {
            kn = k_keys[i][1];
        }
    }
    snprintf(b, 64, "%s: %s", kn, u != NULL && !strcmp(u, "_hold") ? tr("hold", "segurar") : tr("tap", "toque"));
    return b;
}

static const char* item_label(const Item* it) {
    const MenuTexto* t;
    if (!strncmp(it->name, "button_", 7)) {
        return key_label(it);
    }
    if (it->name[0] == '#') {
        return tr(it->en, it->pt); /* (titulo de uma parte da aba) */
    }
    if (lang() == 2) {
        if (it->name[0] == '@') {
            return tr(it->en, it->pt);
        }
        t = texto(it->name);
        return t != NULL ? t->jp : it->en;
    }
    if (lang() >= 3) {
        const char* l;
        if (it->name[0] == '@') {
            return tr(it->en, it->pt);
        }
        t = texto(it->name);
        l = t == NULL ? NULL : (lang() == 3 ? t->de : lang() == 4 ? t->fr : lang() == 5 ? t->it : t->es);
        return l != NULL && l[0] != 0 ? l : it->en;
    }
    return lang() == 1 ? it->pt : it->en;
}

static const char* item_help(const Item* it) {
    const MenuTexto* t = texto(it->name);
    int i;
    if (!strncmp(it->name, "button_", 7)) {
        const char* u = strrchr(it->name, '_');
        if (u != NULL && !strcmp(u, "_hold")) {
            return tr("What this button does when held for the hold time or more; it lasts while the button stays pressed "
                      "(walk slowly, shield...). When tap and hold do the same thing, the button works at once, like a "
                      "normal button.",
                      "O que este botão faz segurado pelo tempo para segurar ou mais; vale enquanto ele continuar apertado "
                      "(andar devagar, escudo...). Quando o toque e o segurar fazem a mesma coisa, o botão vale na hora, "
                      "como um botão comum.");
        }
        return tr("What a quick tap on this button does: pressed and released before the hold time, with no other "
                  "button. When tap and hold do the same thing, the button works at once, like a normal button. Shortcuts "
                  "with SELECT held (brightness, minimap, tunic and boots) don't change.",
                  "O que um toque rápido neste botão faz: apertado e solto antes do tempo para segurar, sem outro botão "
                  "junto. Quando o toque e o segurar fazem a mesma coisa, o botão vale na hora, como um botão comum. Os "
                  "atalhos com SELECT segurado (brilho, minimapa, túnica e botas) não mudam.");
    }
    if (lang() >= 3 && t != NULL) {
        const char* h = lang() == 3 ? t->de_help : lang() == 4 ? t->fr_help : lang() == 5 ? t->it_help : t->es_help;
        if (h[0] != 0) {
            return h;
        }
    } else if (lang() != 0 && t != NULL) {
        return lang() == 2 ? t->jp_help : t->pt_help;
    }
    for (i = 0; i < (int)(sizeof k_en_help / sizeof k_en_help[0]); i++) {
        if (!strcmp(k_en_help[i][0], it->name)) {
            return k_en_help[i][1]; /* (o menu mostra nomes onde o options.txt fala em numeros) */
        }
    }
    return port_option_help(it->name);
}

/* ---- letras japonesas: a fonte kanji do jogo (16x16, 4 bits), pedidas ao jogo (graph.c, z_kanfont.c) ---- */
#define NGLYPHS ((int)(sizeof k_jp_glyphs / sizeof k_jp_glyphs[0]))
static unsigned char g_glyph[sizeof k_jp_glyphs / sizeof k_jp_glyphs[0]][128];
static volatile int g_glyphsOk;

int miyoo_menu_glyphs_wanted(void) {
    return lang() == 2 && !g_glyphsOk;
}

void miyoo_menu_load_glyphs(void (*load)(unsigned short sjis, void* out)) {
    int i;
    for (i = 0; i < NGLYPHS; i++) {
        load(k_jp_glyphs[i], g_glyph[i]);
    }
    g_glyphsOk = 1;
    g_dirty = 1;
    fprintf(stderr, "menu: %d letras japonesas carregadas da fonte do jogo\n", NGLYPHS);
}

static const unsigned char* glyph_of(unsigned short code) {
    int lo = 0, hi = NGLYPHS - 1;
    if (!g_glyphsOk) {
        return NULL;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (k_jp_glyphs[mid] == code) {
            return g_glyph[mid];
        }
        if (k_jp_glyphs[mid] < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static int sjis_lead(unsigned char c) {
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF);
}

/* aba Estados */
#define NSLOTS 10
static int g_slotState[NSLOTS]; /* 0 vazio, 1 ok, -1 de outra versao */
static long g_slotWhen[NSLOTS];
#define THUMB_W 256 /* (port/savestate.c: miniaturas 256x192) */
#define THUMB_H 192
/* (malloc, nao estatico: o executavel tem de acabar antes de 0x81000000, onde o jogo ainda enxerga os dados dele;
 * com 1 MB a mais de dados estaticos o z-buffer do jogo passou dessa linha no lote 159) */
static uint16_t (*g_thumbs)[THUMB_W * THUMB_H];
static int g_thumbOk[NSLOTS];
static volatile int g_slotsDirty = 1;
static int g_saveArmed = -1; /* estado com o 1o X (gravar por cima pede o 2o) */
static char g_status[192];
static double g_statusUntil;

static void load_slots(void) {
    int i;
    g_slotsDirty = 0;
    for (i = 0; i < NSLOTS; i++) {
        g_slotState[i] = port_ss_slot_info(i, &g_slotWhen[i]);
        if (g_thumbs == NULL) {
            g_thumbs = malloc(sizeof *g_thumbs * NSLOTS);
        }
        g_thumbOk[i] = g_thumbs != NULL && g_slotState[i] != 0 && port_ss_slot_thumb(i, g_thumbs[i]);
    }
}

/* port/savestate.c: um estado foi gravado */
void miyoo_menu_slots_changed(void) {
    g_slotsDirty = 1;
    g_dirty = 1;
}

/* port/savestate.c: avisos (salvo, carregado, ocupado...) aparecem dentro do menu */
void miyoo_menu_status(const char* text) {
    snprintf(g_status, sizeof g_status, "%s", text);
    g_statusUntil = miyoo_now_ms() + 3000.0;
    g_dirty = 1;
}

static void status_ms(const char* text, double ms) {
    snprintf(g_status, sizeof g_status, "%s", text);
    g_statusUntil = miyoo_now_ms() + ms;
    g_dirty = 1;
}

/* janela "What's new" (update_miyoo.c achou uma versao nova): o texto do release, rolando com cima/baixo */
#define UPD_MAXL 220
static volatile int g_upd;
static char g_updTitle[128];
static char g_updText[16384];
static char g_updLines[UPD_MAXL][256];
static int g_updN, g_updTop, g_updWrapped;
static volatile int g_open;
static const char* tr(const char* en, const char* pt);

void miyoo_menu_update_result(int code, const char* latest, const char* cur, const char* notes, int automatic) {
    char buf[192];
    if (code == MIYOO_UPD_LATEST) {
        snprintf(buf, sizeof buf, "%s v%s", tr("You have the latest version:", "Você já tem a versão mais nova:"),
                 cur[0] == 'v' || cur[0] == 'V' ? cur + 1 : cur);
        status_ms(buf, 8000.0);
    } else if (code == MIYOO_UPD_NEWER && automatic) {
        /* a procura automatica (ao abrir o menu, uma vez por dia): so avisa, e so com o menu aberto */
        if (g_open) {
            snprintf(buf, sizeof buf, "%s %s - %s", tr("New version available:", "Versão nova disponível:"), latest,
                     tr("use Check for updates", "use Procurar atualizações"));
            status_ms(buf, 12000.0);
        }
    } else if (code == MIYOO_UPD_NEWER) {
        snprintf(g_updTitle, sizeof g_updTitle, "%s %s", tr("New version available:", "Versão nova disponível:"), latest);
        if (cur[0]) {
            snprintf(g_updTitle + strlen(g_updTitle), sizeof g_updTitle - strlen(g_updTitle), " (%s v%s)",
                     tr("you have", "você tem"), cur[0] == 'v' || cur[0] == 'V' ? cur + 1 : cur);
        }
        snprintf(g_updText, sizeof g_updText, "%s", notes != NULL ? notes : "");
        g_updTop = 0;
        g_updWrapped = 0;
        g_status[0] = 0;
        if (g_open) {
            g_upd = 1;
        } else {
            status_ms(g_updTitle, 8000.0);
        }
    } else if (code == MIYOO_UPD_NO_NET) {
        status_ms(tr("No internet: turn on Wi-Fi in OnionOS", "Sem internet: ligue o Wi-Fi no OnionOS"), 8000.0);
    } else {
        status_ms(tr("Could not check for updates", "Não foi possível procurar atualizações"), 8000.0);
    }
    g_dirty = 1;
}


static int g_tab, g_sel[TAB_COUNT], g_top[TAB_COUNT];
static int g_changed;
static int g_restoreArmed; /* "Restaurar padrao" / "Sair" / "Reiniciar": o 1o A pede confirmacao, o 2o faz (1 restaurar,
                            * 2 sair, 3 reiniciar) */
static int action_id(const char* name) {
    return !strcmp(name, "@quit") ? 2 : !strcmp(name, "@reset") ? 3 : !strcmp(name, "@keys") ? 4 : !strcmp(name, "@update") ? 5 : 1;
}
static int g_confirm; /* pergunta na tela: uma opcao que so vale reiniciando mudou; reiniciar agora ou cancelar */
static int g_openVal[256];  /* valores ao abrir o menu (para saber se uma opcao de reiniciar mudou) */
static double g_restoreAt;

static int is_action(const Item* it) {
    return it->name[0] == '@';
}

/* titulo de uma parte da aba (nome "#..."): so o texto, a escolha pula por cima */
static int is_header(const Item* it) {
    return it != NULL && it->name[0] == '#';
}

static int item_ok(const Item* it) {
    return is_action(it) || is_header(it) || port_option_ptr(it->name) != NULL;
}

/* opcao que nao vale agora (aparece cinza e nao muda): a resolucao com o filtro retro ligado (fica em 320x240) e a
 * forca do filtro com ele desligado */
static int item_locked(const Item* it) {
    extern int gx_retro; /* gfx_hires.c */
    if (is_action(it)) {
        return 0;
    }
    return (!strcmp(it->name, "max_resolution") && gx_retro) || ((!strcmp(it->name, "retro_filter_strength") || !strcmp(it->name, "retro_filter_tv")) && !gx_retro);
}

/* todas as opcoes do menu (e as de diagnostico) voltam aos valores de fabrica */
static void restore_defaults(void) {
    int i, n = 0;
    for (i = 0; i < NITEMS; i++) {
        int* v = is_action(&k_items[i]) ? NULL : port_option_ptr(k_items[i].name);
        int d;
        if (v != NULL && port_option_default(k_items[i].name, &d) && *v != d) {
            *v = d;
            n++;
        }
    }
    port_options_default_diagnostics();
    g_changed = 1;
    fprintf(stderr, "menu: padrao restaurado (%d opcoes mudaram)\n", n);
}

/* "Teclas padrao": so os botoes (toque e segurado) voltam ao esquema do port */
static void restore_keys(void) {
    int i, n = 0;
    for (i = 0; i < NITEMS; i++) {
        int* v = strncmp(k_items[i].name, "button_", 7) ? NULL : port_option_ptr(k_items[i].name);
        int d;
        if (v != NULL && port_option_default(k_items[i].name, &d) && *v != d) {
            *v = d;
            n++;
        }
    }
    g_changed = 1;
    fprintf(stderr, "menu: teclas padrao (%d opcoes mudaram)\n", n);
}

int miyoo_menu_active(void) {
    return g_open;
}

static int g_anim;      /* (desenho) algo se mexe na tela: redesenha sempre */
static double g_selAt; /* (desenho) quando a escolha mudou */

/* hora de redesenhar sem mudanca nenhuma (o cartao da descricao comecar a crescer); 0 = nenhuma */
static double g_wakeAt;

int miyoo_menu_dirty(void) {
    if (g_wakeAt > 0.0 && miyoo_now_ms() >= g_wakeAt) {
        g_wakeAt = 0.0;
        return 1;
    }
    return g_dirty || g_anim;
}

static int tab_count(int tab, int* first) {
    int i, n = 0;
    *first = -1;
    if (IS_STATES(tab)) {
        return NSLOTS;
    }
    for (i = 0; i < NITEMS; i++) {
        if (k_items[i].tab == tab && item_ok(&k_items[i])) {
            if (*first < 0) {
                *first = i;
            }
            n++;
        }
    }
    return n;
}

/* o n-esimo item da aba (pulando opcoes que esta compilacao nao tem) */
static const Item* tab_item(int tab, int n) {
    int i;
    for (i = 0; i < NITEMS; i++) {
        if (k_items[i].tab == tab && item_ok(&k_items[i]) && n-- == 0) {
            return &k_items[i];
        }
    }
    return NULL;
}

static int restart_changed(void) {
    int i;
    for (i = 0; i < NITEMS && i < 256; i++) {
        const int* v = k_items[i].restart ? port_option_ptr(k_items[i].name) : NULL;
        if (v != NULL && *v != g_openVal[i]) {
            return 1;
        }
    }
    return 0;
}

/* "cancelar" na pergunta: as opcoes que so valem reiniciando voltam ao que eram quando o menu abriu (trocar o
 * idioma sem reiniciar deixava a resolucao baixa de vez: nenhuma delas fica mudada sem reiniciar) */
static void restart_revert(void) {
    int i;
    for (i = 0; i < NITEMS && i < 256; i++) {
        int* v = k_items[i].restart ? port_option_ptr(k_items[i].name) : NULL;
        if (v != NULL && *v != g_openVal[i]) {
            fprintf(stderr, "menu: %s volta a %d (reinicio cancelado)\n", k_items[i].name, g_openVal[i]);
            *v = g_openVal[i];
        }
    }
}

void miyoo_menu_open(void) {
    if (!g_open) {
        int i;
        for (i = 0; i < NITEMS && i < 256; i++) {
            const int* v = k_items[i].name[0] != '@' ? port_option_ptr(k_items[i].name) : NULL;
            g_openVal[i] = v != NULL ? *v : 0;
        }
        g_confirm = 0;
        g_upd = 0;
        miyoo_rumble_off();
        g_slotsDirty = 1;
        g_saveArmed = -1;
        g_status[0] = 0;
        g_changed = 0;
        g_selAt = miyoo_now_ms();
        g_dirty = 1;
        g_open = 1;
        fprintf(stderr, "menu: aberto\n");
        port_update_check_start(1); /* (uma vez por dia, so com Wi-Fi; nao segura o menu: roda numa thread) */
    }
}

/* fecha (restarting = vai reiniciar agora: as opcoes que so valem reiniciando ficam; senao elas voltam ao que eram) */
static void close_menu(int restarting) {
    if (g_open) {
        if (!restarting) {
            restart_revert();
        }
        g_confirm = 0;
        g_open = 0;
        if (g_changed) {
            port_options_save();
            fprintf(stderr, "menu: fechado, options.txt gravado\n");
        } else {
            fprintf(stderr, "menu: fechado\n");
        }
    }
}

/* fecha na hora (estado carregado, desligar...): opcoes que so valem reiniciando mudadas e nao confirmadas voltam */
void miyoo_menu_close_now(void) {
    close_menu(0);
}

/* B, START, MENU+START: fecha; se uma opcao que so vale reiniciando mudou, antes pergunta: reinicia ou cancela */
void miyoo_menu_close(void) {
    if (g_open && !g_confirm && restart_changed()) {
        g_confirm = 1;
        g_dirty = 1;
        return;
    }
    miyoo_menu_close_now();
}

/* filtro retro: as linhas (e a mascara) escurecem a imagem, entao o brilho sobe sozinho ao ligar (3 com linhas, 4 com
 * mascara; o jogador ainda pode mudar) e volta ao valor de antes do filtro ao desligar (guardado no options.txt) */
static void retro_brightness(int from, int to) {
    extern int gx_brilho, port_opt_retro_brightness_base; /* gfx_rdp.c, port_options.c */
    static const int k_up[3] = { 0, 3, 4 };
    from = from < 0 ? 0 : from > 2 ? 2 : from;
    to = to < 0 ? 0 : to > 2 ? 2 : to;
    if (from == 0 && to != 0) {
        port_opt_retro_brightness_base = gx_brilho;
        gx_brilho += k_up[to];
    } else if (to == 0) {
        if (port_opt_retro_brightness_base >= 0) {
            gx_brilho = port_opt_retro_brightness_base;
        }
        port_opt_retro_brightness_base = -1;
    } else {
        gx_brilho += k_up[to] - k_up[from];
    }
    gx_brilho = gx_brilho < 0 ? 0 : gx_brilho > 10 ? 10 : gx_brilho;
    fprintf(stderr, "menu: filtro retro %d -> %d: brilho %d\n", from, to, gx_brilho);
}

static void change(const Item* it, int dir) {
    int* v;
    int x;
    if (is_header(it)) {
        return;
    }
    if (is_action(it)) {
        const int which = action_id(it->name);
        if (which == 5) {
            /* procurar atualizacoes: um A so (nao muda nada no jogo) */
            if (port_update_check_start(0)) {
                status_ms(tr("Checking for updates...", "Procurando atualizações..."), 30000.0);
            }
            return;
        }
        if (g_restoreArmed == which && miyoo_now_ms() - g_restoreAt < 4000.0) {
            g_restoreArmed = 0;
            if (which == 1) {
                restore_defaults();
            } else if (which == 4) {
                restore_keys();
            } else if (which == 3) {
                /* reiniciar do comeco (abertura), como o reset do N64; salva antes, como o "Sair" */
                close_menu(0);
                miyoo_menu_status(tr("Restarting...", "Reiniciando..."));
                port_ss_reset_request();
            } else {
                /* sair do jogo (como o MENU fora do OnionOS): o jogo salva sozinho (autosave) e fecha */
                close_menu(0);
                miyoo_menu_status(tr("Quitting...", "Saindo..."));
                port_ss_quit_request();
            }
        } else {
            g_restoreArmed = which;
            g_restoreAt = miyoo_now_ms();
        }
        return;
    }
    v = port_option_ptr(it->name);
    if (v == NULL || item_locked(it)) {
        return;
    }
    if (it->min == 0 && it->max == 1) {
        x = !*v;
    } else {
        int tries = 0;
        x = *v;
        do {
            const int from = x;
            x = from + dir * it->step;
            if (x < it->min) {
                x = dir < 0 && from > it->min ? it->min : (dir < 0 ? it->max : it->min); /* da volta nas pontas */
            } else if (x > it->max) {
                x = dir > 0 && from < it->max ? it->max : (dir > 0 ? it->min : it->max);
            }
            /* alemao e frances so existem depois de o jogo tirar da ROM europeia (port/lang_pal.c); italiano e espanhol
             * so com o idiomas/it.oot e o idiomas/es.oot */
        } while (!strcmp(it->name, "language") && ((x == 3 && access("idiomas/de.oot", R_OK) != 0) ||
                                                    (x == 4 && access("idiomas/fr.oot", R_OK) != 0) ||
                                                    (x == 5 && access("idiomas/it.oot", R_OK) != 0) ||
                                                    (x == 6 && access("idiomas/es.oot", R_OK) != 0)) &&
                 ++tries < 7);
    }
    if (x != *v) {
        const int from = *v;
        *v = x;
        g_changed = 1;
        fprintf(stderr, "menu: %s = %d\n", it->name, x);
        if (!strcmp(it->name, "retro_filter")) {
            retro_brightness(from, x);
        }
    }
}

/* botoes (input_miyoo.c), ja com a repeticao de quem segura: MENU_K_* */
void miyoo_menu_key(int k) {
    int first, n = tab_count(g_tab, &first);
    g_selAt = miyoo_now_ms(); /* (nome comprido e descricao comecam a rolar de novo) */
    int* sel = &g_sel[g_tab];
    const Item* it;
    if (g_upd) {
        /* janela "What's new": cima/baixo rolam o texto, B (ou A) fecha */
        const int vis = 11;
        if (k == MENU_K_UP && g_updTop > 0) {
            g_updTop--;
        } else if (k == MENU_K_DOWN && g_updTop + vis < g_updN) {
            g_updTop++;
        } else if (k == MENU_K_CLOSE || k == MENU_K_A) {
            g_upd = 0;
        }
        g_dirty = 1;
        return;
    }
    if (g_confirm) {
        if (k == MENU_K_A) { /* sim: o jogo salva o estado, reinicia e continua dali */
            close_menu(1);
            miyoo_menu_status(tr("Restarting...", "Reiniciando..."));
            port_ss_restart_request();
        } else if (k == MENU_K_CLOSE) { /* cancelar: essas opcoes voltam ao que eram */
            miyoo_menu_close_now();
        }
        return;
    }
    if (k != MENU_K_A) {
        g_restoreArmed = 0; /* (a confirmacao e so com o A seguido) */
    }
    if (k != MENU_K_X && !(g_tab == TAB_SAVE && k == MENU_K_A)) {
        g_saveArmed = -1;
    }
    if (IS_STATES(g_tab) && (k == MENU_K_A || k == MENU_K_X)) {
        const int slot = *sel;
        if (g_tab == TAB_LOAD && k == MENU_K_X) {
            return;
        }
        if (g_tab == TAB_LOAD) {
            if (g_slotState[slot] == 1) {
                port_ss_slot_request(0, slot);
                miyoo_menu_status(tr("Loading...", "Carregando..."));
            } else {
                miyoo_menu_status(g_slotState[slot] == 0 ? tr("Empty slot", "Estado vazio")
                                                          : tr("State from another version", "Estado de outra versão"));
            }
        } else if (g_slotState[slot] != 0 && g_saveArmed != slot) {
            g_saveArmed = slot;
            miyoo_menu_status(tr("Press A again to overwrite", "A de novo para gravar por cima"));
        } else {
            g_saveArmed = -1;
            port_ss_slot_request(1, slot);
            miyoo_menu_status(tr("Saving...", "Salvando..."));
        }
        g_dirty = 1;
        return;
    }
    switch (k) {
        case MENU_K_UP:
            if (n > 0) {
                int g = 0;
                do {
                    *sel = (*sel + n - 1) % n; /* (pula os titulos) */
                } while (is_header(tab_item(g_tab, *sel)) && ++g < n);
            }
            break;
        case MENU_K_DOWN:
            if (n > 0) {
                int g = 0;
                do {
                    *sel = (*sel + 1) % n;
                } while (is_header(tab_item(g_tab, *sel)) && ++g < n);
            }
            break;
        case MENU_K_TAB_PREV:
            g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT;
            break;
        case MENU_K_TAB_NEXT:
            g_tab = (g_tab + 1) % TAB_COUNT;
            break;
        case MENU_K_LEFT:
        case MENU_K_RIGHT:
        case MENU_K_A:
            it = tab_item(g_tab, *sel);
            if (it != NULL) {
                change(it, k == MENU_K_LEFT ? -1 : 1);
            }
            break;
        case MENU_K_CLOSE:
            miyoo_menu_close();
            break;
        default:
            break;
    }
    g_dirty = 1;
}

/* ---------------------------------------------------------------- desenho (thread da tela) */
/*
 * Visual (03/11/2026): cartoes com cantos redondos e sombra, abas em pilula, chaves liga/desliga, barras finas com
 * bolinha, dicas do rodape como teclas; dois esquemas de cores (menu_theme); letras suavizadas maiores (font_menu.h,
 * 26 px; japones: a fonte do jogo ampliada 1,5x), abas com contorno (rolam, com setas, quando nao cabem), lista com
 * barra de rolagem, nomes compridos que rolam quando escolhidos, descricao em ate 4 linhas (rola sozinha quando e
 * maior). Os paineis sao semitransparentes sobre a ultima imagem do jogo (escurecida pela tela, video_miyoo.c).
 */

static uint32_t* g_page;
static int g_clipX0 = 0, g_clipX1 = W; /* letras so entre essas colunas */
static int g_clipY0 = 0, g_clipY1 = H; /* e entre essas linhas (o cartao da descricao crescendo) */

/* cores: dois esquemas (opcao menu_theme): 0 = verde escuro e dourado, 1 = claro (verde-limao, branco e azul-marinho) */
typedef struct {
    uint32_t bg; int bgA;            /* tela inteira por baixo do menu (sobre o jogo escurecido) */
    uint32_t card; int cardA;        /* cartao da lista */
    uint32_t text, dim;              /* nomes e valores na lista */
    uint32_t help; int helpA;        /* cartao da descricao */
    uint32_t helpText;
    uint32_t accent;                 /* destaques (setas, barra de rolagem, botao ligado) */
    uint32_t sel, selText, selDim;   /* linha escolhida */
    uint32_t tabOn, tabOnText, tabOff; int tabOffA; uint32_t tabOffText;
    uint32_t swOn, swOff, knob;      /* chave liga/desliga */
    uint32_t track, fill;            /* barras */
    uint32_t status, warn;           /* avisos no rodape / "A de novo" */
    int shadowA;                     /* sombra dos cartoes */
    uint32_t foot, chipBg, chipText; /* dicas do rodape */
    uint32_t toastBg, toastText;
} MenuTheme;

static const MenuTheme k_themes[2] = {
    { /* verde escuro e dourado */
      0x06140Cu, 150, 0x10301Cu, 238, 0xF2F0DCu, 0x9CB496u, 0x0A2214u, 242, 0xD6E4CCu, 0xE6C25Au,
      0x2C6A36u, 0xFFFFFFu, 0xD6E4CCu, 0xE6C25Au, 0x10200Fu, 0xFFFFFFu, 28, 0xD6E4CCu,
      0xE6C25Au, 0x3A4A3Eu, 0xFFFFFFu, 0x284A32u, 0xE6C25Au, 0xFFE680u, 0xFFB060u, 70,
      0x9CB496u, 0x284A32u, 0xF2F0DCu, 0x10301Cu, 0xF2F0DCu },
    { /* claro: verde-limao, branco e azul-marinho */
      0x86C23Cu, 240, 0xFFFFFFu, 252, 0x1D3B4Cu, 0x6B7F8Au, 0xFFFFFFu, 252, 0x1D3B4Cu, 0x7DBE3Au,
      0x2C6A36u, 0xFFFFFFu, 0xD6E4CCu, 0xFFFFFFu, 0x1D3B4Cu, 0xFFFFFFu, 70, 0xFFFFFFu,
      0x7DBE3Au, 0xC9D3CFu, 0xFFFFFFu, 0xDDE6E2u, 0x7DBE3Au, 0xFFFFFFu, 0xF08A3Cu, 55,
      0x1D3B4Cu, 0xFFFFFFu, 0x1D3B4Cu, 0xFFFFFFu, 0x1D3B4Cu },
};
extern int port_opt_menu_theme; /* opcao menu_theme (port_options.c) */
static const MenuTheme* g_th = &k_themes[0];
#define TH(f) (g_th->f)

/* tamanho da pagina em que se desenha: o menu e sempre 640x480; o aviso por cima do jogo vai na pagina da tela, que no
 * Mini v4 / Mini Flip e de 752x560 (video_miyoo.c) */
static int g_pgW = W, g_pgH = H;
extern int miyoo_panel_w, miyoo_panel_h;

static inline uint32_t* px_ptr(int x, int y) {
    return &g_page[(size_t)(g_pgH - 1 - y) * g_pgW + (g_pgW - 1 - x)]; /* painel girado 180 graus */
}

static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)g_pgW && (unsigned)y < (unsigned)g_pgH) {
        *px_ptr(x, y) = c;
    }
}

/* mistura c com o que ja esta no pixel (a = 0..255) */
static inline void px_a(int x, int y, uint32_t c, int a) {
    if ((unsigned)x < (unsigned)g_pgW && (unsigned)y < (unsigned)g_pgH && a > 0) {
        uint32_t* d = px_ptr(x, y);
        if (a >= 255) {
            *d = c;
        } else {
            const uint32_t o = *d;
            const int r = (int)((o >> 16) & 255) + ((((int)((c >> 16) & 255) - (int)((o >> 16) & 255)) * a) >> 8);
            const int g = (int)((o >> 8) & 255) + ((((int)((c >> 8) & 255) - (int)((o >> 8) & 255)) * a) >> 8);
            const int b = (int)(o & 255) + ((((int)(c & 255) - (int)(o & 255)) * a) >> 8);
            *d = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}


static void rect_a(int x0, int y0, int w, int h, uint32_t c, int a) {
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            px_a(x, y, c, a);
        }
    }
}




/* ---- formas suavizadas (cantos redondos, circulos, sombras) ---- */

/* retangulo de cantos redondos (raio r), suavizado nas bordas, com alfa a */
static void rrect(int x0, int y0, int w, int h, int r, uint32_t c, int a) {
    int x, y;
    if (r * 2 > h) {
        r = h / 2;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    for (y = y0; y < y0 + h; y++) {
        for (x = x0; x < x0 + w; x++) {
            int cx = -1, cy = -1;
            if (x < x0 + r) {
                cx = x0 + r;
            } else if (x >= x0 + w - r) {
                cx = x0 + w - r - 1;
            }
            if (y < y0 + r) {
                cy = y0 + r;
            } else if (y >= y0 + h - r) {
                cy = y0 + h - r - 1;
            }
            if (cx >= 0 && cy >= 0) {
                /* canto: cobertura pela distancia ao centro do arco */
                const float dx = (float)(x - cx) + (x < cx ? 0.5f : -0.5f), dy = (float)(y - cy) + (y < cy ? 0.5f : -0.5f);
                const float d = sqrtf(dx * dx + dy * dy), cov = (float)r + 0.5f - d;
                if (cov <= 0.0f) {
                    continue;
                }
                px_a(x, y, c, cov >= 1.0f ? a : (int)((float)a * cov));
            } else {
                px_a(x, y, c, a);
            }
        }
    }
}

static void circle(float cx, float cy, float r, uint32_t c, int a) {
    int x, y;
    for (y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
        for (x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            const float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov > 0.0f) {
                px_a(x, y, c, cov >= 1.0f ? a : (int)((float)a * cov));
            }
        }
    }
}

/* sombra suave embaixo de um cartao */
static void shadow(int x0, int y0, int w, int h, int r) {
    int k;
    if (TH(shadowA) <= 0) {
        return;
    }
    for (k = 6; k >= 1; k--) {
        rrect(x0 - k + 1, y0 - k + 4, w + 2 * k - 2, h + 2 * k - 2, r + k, 0x000000u, TH(shadowA) / (k + 1));
    }
}

/* seta fina (chevron) suavizada: dir 0 esquerda, 1 direita, 2 cima, 3 baixo */
static void chevron(int cx, int cy, int s, int dir, uint32_t c) {
    int i;
    for (i = -s; i <= s; i++) {
        const float t = (float)(s - (i < 0 ? -i : i)) * 0.6f;
        float x, y;
        switch (dir) {
            case 0: x = cx - t; y = cy + i; break;
            case 1: x = cx + t; y = cy + i; break;
            case 2: x = cx + i; y = cy - t; break;
            default: x = cx + i; y = cy + t; break;
        }
        circle(x, y, 1.3f, c, 255);
    }
}

/* ---- letras pequenas 8x16 (avisos da tela e dicas do rodape) ---- */

/* uma letra latina (ASCII ou acentuada, UTF-8 de 2 bytes: U+00A0..U+00FF): o desenho 8x16, ou NULL. *len = bytes */
static const unsigned char* latin_glyph(const unsigned char* p, int* len) {
    unsigned int c = p[0];
    *len = 1;
    if ((c == 0xC2 || c == 0xC3) && (p[1] & 0xC0) == 0x80) {
        *len = 2;
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return c >= 0xA0 ? font_latin1[c - 0xA0] : font8x16['?' - 32];
    }
    if (c < 32 || c > 126) {
        c = '?';
    }
    return font8x16[c - 32];
}

static int g_scale = 1; /* (os avisos da tela usam letras 2x) */

static void px_s(int x, int y, uint32_t c) {
    int sy, sx;
    for (sy = 0; sy < g_scale; sy++) {
        for (sx = 0; sx < g_scale; sx++) {
            px(x + sx, y + sy, c);
        }
    }
}

/* texto 8x16 (UTF-8) e japones Shift-JIS 16x16 (fonte do jogo); devolve o x do fim. (Os textos em japones sao
 * Shift-JIS e os outros UTF-8: os primeiros bytes nao se confundem, 0xC2/0xC3 nao comecam letra Shift-JIS.) */
static int text(int x, int y, const char* s, uint32_t c) {
    const unsigned char* p = (const unsigned char*)s;
    const int k = g_scale;
    while (*p) {
        int gy, gx;
        if (sjis_lead(*p) && p[1]) {
            const unsigned char* g = glyph_of((unsigned short)(p[0] << 8 | p[1]));
            if (g != NULL) {
                for (gy = 0; gy < 16; gy++) {
                    for (gx = 0; gx < 16; gx++) {
                        const unsigned char b = g[gy * 8 + gx / 2];
                        if (((gx & 1) ? (b & 15) : (b >> 4)) >= 6) {
                            px_s(x + gx * k, y + gy * k, c);
                        }
                    }
                }
            }
            x += 16 * k;
            p += 2;
        } else {
            int len;
            const unsigned char* g = latin_glyph(p, &len);
            for (gy = 0; gy < 16; gy++) {
                for (gx = 0; gx < 8; gx++) {
                    if (g[gy] & (0x80 >> gx)) {
                        px_s(x + gx * k, y + gy * k, c);
                    }
                }
            }
            x += 8 * k;
            p += len;
        }
    }
    return x;
}

/* bytes e largura (pixels, escala 1) da letra em p */
static int char_size(const unsigned char* p, int* w) {
    int len;
    if (sjis_lead(*p) && p[1]) {
        *w = 16;
        return 2;
    }
    latin_glyph(p, &len);
    *w = 8;
    return len;
}

int miyoo_menu_text_width(const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    int w = 0, cw;
    while (*p) {
        p += char_size(p, &cw);
        w += cw;
    }
    return w;
}

/* para os avisos da tela (video_miyoo.c): o mesmo texto, numa pagina qualquer, em escala 1 ou 2 */
int miyoo_menu_draw_text(uint32_t* page, int x, int y, const char* s, uint32_t c, int scale) {
    int r;
    g_page = page;
    g_scale = scale < 1 ? 1 : scale;
    r = text(x, y, s, c);
    g_scale = 1;
    return r;
}

/* aviso curto por cima do jogo (video_miyoo.c: "Estado salvo"...), no alto da tela, com o visual do menu */
void miyoo_menu_draw_toast(uint32_t* page, const char* s) {
    int i, jp = 0, w, scale, x0, bw, bh;
    const int len = (int)strlen(s);
    for (i = 0; i < len; i++) {
        jp |= sjis_lead((unsigned char)s[i]);
    }
    g_page = page;
    g_pgW = miyoo_panel_w;
    g_pgH = miyoo_panel_h;
    g_clipX1 = g_pgW;
    g_th = &k_themes[port_opt_menu_theme == 1 ? 1 : 0];
    scale = jp ? 1 : 2;
    w = miyoo_menu_text_width(s) * scale;
    bh = 16 * scale + 16;
    bw = w + 56;
    x0 = (g_pgW - bw) / 2;
    rrect(x0, 10, bw, bh, bh / 2, TH(toastBg), 255); /* (pilula; so os cantos leem a tela) */
    circle((float)(x0 + 22), (float)(10 + bh / 2), 6.0f, TH(accent), 255);
    g_scale = scale;
    text(x0 + 38, 18, s, TH(toastText));
    g_scale = 1;
    g_pgW = W;
    g_pgH = H;
    g_clipX1 = W;
}

/* ---- letras grandes suavizadas (o menu) ---- */

#include "font_menu.h"

/* japones: a letra da fonte do jogo (16x16, 4 bits) ampliada para 24x24, calculada uma vez */
static unsigned char (*g_jp24)[24 * 24];
static unsigned char* g_jp24ok;

static const unsigned char* jp24(unsigned short code) {
    int lo = 0, hi = NGLYPHS - 1, idx = -1;
    if (!g_glyphsOk) {
        return NULL;
    }
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k_jp_glyphs[mid] == code) {
            idx = mid;
            break;
        }
        if (k_jp_glyphs[mid] < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (idx < 0) {
        return NULL;
    }
    if (g_jp24 == NULL) {
        g_jp24 = calloc(NGLYPHS, sizeof *g_jp24);
        g_jp24ok = calloc(NGLYPHS, 1);
        if (g_jp24 == NULL || g_jp24ok == NULL) {
            return NULL;
        }
    }
    if (!g_jp24ok[idx]) {
        /* media por area: cada pixel novo cobre 2/3 de pixel da letra original */
        const unsigned char* g = g_glyph[idx];
        int ty, tx;
        for (ty = 0; ty < 24; ty++) {
            for (tx = 0; tx < 24; tx++) {
                float acc = 0.0f;
                const float sy0 = ty * (16.0f / 24.0f), sy1 = (ty + 1) * (16.0f / 24.0f);
                const float sx0 = tx * (16.0f / 24.0f), sx1 = (tx + 1) * (16.0f / 24.0f);
                int sy, sx;
                for (sy = (int)sy0; sy < 16 && sy < sy1; sy++) {
                    const float oy = fminf(sy1, sy + 1.0f) - fmaxf(sy0, (float)sy);
                    for (sx = (int)sx0; sx < 16 && sx < sx1; sx++) {
                        const float ox = fminf(sx1, sx + 1.0f) - fmaxf(sx0, (float)sx);
                        const unsigned char b = g[sy * 8 + sx / 2];
                        const int v = (sx & 1) ? (b & 15) : (b >> 4);
                        acc += ox * oy * (float)v;
                    }
                }
                acc = acc * (255.0f / 15.0f) / ((16.0f / 24.0f) * (16.0f / 24.0f)) * 1.15f;
                g_jp24[idx][ty * 24 + tx] = (unsigned char)(acc > 255.0f ? 255.0f : acc);
            }
        }
        g_jp24ok[idx] = 1;
    }
    return g_jp24[idx];
}

/* indice da letra latina (UTF-8) na fonte do menu; *len = bytes */
static int fm_index(const unsigned char* p, int* len) {
    unsigned int c = p[0];
    *len = 1;
    if ((c == 0xC2 || c == 0xC3) && (p[1] & 0xC0) == 0x80) {
        *len = 2;
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return c >= 0xA0 ? 95 + (int)(c - 0xA0) : '?' - 32;
    }
    if (c < 32 || c > 126) {
        c = '?';
    }
    return (int)c - 32;
}

/* largura de uma letra grande e bytes dela */
static int big_char(const unsigned char* p, int* w) {
    int len;
    if (sjis_lead(*p) && p[1]) {
        *w = 24;
        return 2;
    }
    *w = k_fm_w[fm_index(p, &len)];
    return len;
}

static int big_width(const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    int w = 0, cw;
    while (*p) {
        p += big_char(p, &cw);
        w += cw;
    }
    return w;
}

/* texto grande suavizado (so entre g_clipX0 e g_clipX1); devolve o x do fim */
static int big(int x, int y, const char* s, uint32_t c) {
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        int gx, gy, cw;
        if (sjis_lead(*p) && p[1]) {
            const unsigned char* g = jp24((unsigned short)(p[0] << 8 | p[1]));
            if (g != NULL && x + 24 > g_clipX0 && x < g_clipX1) {
                for (gy = 0; gy < 24; gy++) {
                    if (y + 1 + gy < g_clipY0 || y + 1 + gy >= g_clipY1) {
                        continue;
                    }
                    for (gx = 0; gx < 24; gx++) {
                        if (x + gx >= g_clipX0 && x + gx < g_clipX1) {
                            px_a(x + gx, y + 1 + gy, c, g[gy * 24 + gx]);
                        }
                    }
                }
            }
            x += 24;
            p += 2;
        } else {
            int len;
            const int i = fm_index(p, &len);
            const unsigned char* g = k_fm_px + k_fm_off[i];
            cw = k_fm_w[i];
            if (x + cw > g_clipX0 && x < g_clipX1) {
                for (gy = 0; gy < FM_H; gy++) {
                    if (y + gy < g_clipY0 || y + gy >= g_clipY1) {
                        continue;
                    }
                    for (gx = 0; gx < cw; gx++) {
                        if (x + gx >= g_clipX0 && x + gx < g_clipX1) {
                            px_a(x + gx, y + gy, c, g[gy * cw + gx]);
                        }
                    }
                }
            }
            x += cw;
            p += len;
        }
    }
    return x;
}

static void big_right(int xr, int y, const char* s, uint32_t c) {
    big(xr - big_width(s), y, s, c);
}

/* texto grande que nao cabe entre x0 e x1: cortado; com 'roll', rola devagar (espera 1 s, anda, espera, volta) */
static void big_fit(int x0, int x1, int y, const char* s, uint32_t c, int roll) {
    const int w = big_width(s), room = x1 - x0;
    int off = 0;
    g_clipX0 = x0;
    g_clipX1 = x1;
    if (w > room && roll) {
        const double speed = 0.05; /* pixels por ms (50 por segundo) */
        const double over = (double)(w - room), period = 1200.0 + over / speed + 1200.0;
        const double t = fmod(miyoo_now_ms() - g_selAt, period);
        off = t < 1200.0 ? 0 : (int)fmin(over, (t - 1200.0) * speed);
        g_anim = 1;
    }
    big(x0 - off, y, s, c);
    g_clipX0 = 0;
    g_clipX1 = W;
}

/* ---- textos de cada opcao ---- */

/* nomes para opcoes de poucos valores (em vez do numero): menu_textos.h k_choices */
static const char* choice_name(const char* opt, int v) {
    int i;
    if (!strncmp(opt, "button_", 7)) {
        opt = "button_action"; /* (as acoes das teclas: a mesma lista para todos os botoes) */
    }
    for (i = 0; i < (int)(sizeof k_choices / sizeof k_choices[0]); i++) {
        if (!strcmp(k_choices[i].name, opt)) {
            if (v < 0 || v >= k_choices[i].n) {
                return NULL;
            }
            return lang() == 2   ? k_choices[i].jp[v]
                   : lang() == 1 ? k_choices[i].pt[v]
                   : lang() == 3 ? k_choices[i].de[v]
                   : lang() == 4 ? k_choices[i].fr[v]
                   : lang() == 5 ? k_choices[i].it[v]
                   : lang() == 6 ? k_choices[i].es[v]
                                 : k_choices[i].en[v];
        }
    }
    return NULL;
}

/* descricao de uma opcao liga/desliga: no options.txt ela diz "1 = ..., 0 = ..."; no menu a opcao mostra SIM/NAO.
 * "1 = " / "0 = " (e "1＝" do japones) no comeco ou depois de espaco/parentese viram "SIM: " / "NAO: " */
static const char* toggle_help(const char* s, char* out, size_t cap) {
    const char* on = tr("ON: ", "Ligado: ");
    const char* off = tr("OFF: ", "Desligado: ");
    const char* start = s;
    size_t n = 0;
    while (*s && n + 16 < cap) {
        const char* rep = NULL;
        int skip = 0;
        const int prevOk = s == start || s[-1] == ' ' || s[-1] == '(' || (unsigned char)s[-1] == 0x42 /* "。" fim */;
        if ((s[0] == '1' || s[0] == '0') && prevOk) {
            if (s[1] == ' ' && s[2] == '=' && s[3] == ' ') {
                rep = s[0] == '1' ? on : off;
                skip = 4;
            } else if ((unsigned char)s[1] == 0x81 && (unsigned char)s[2] == 0x81) { /* "1＝" */
                rep = s[0] == '1' ? on : off;
                skip = 3;
            }
        }
        if (rep != NULL) {
            const size_t l = strlen(rep);
            memcpy(out + n, rep, l);
            n += l;
            s += skip;
        } else {
            out[n++] = *s++;
        }
    }
    out[n] = 0;
    return out;
}

/* a descricao em linhas pela largura (nos espacos; o japones, em qualquer letra); devolve quantas */
#define HELP_MAXL 16
static int help_split(const char* s, int maxw, char lines[HELP_MAXL][256]) {
    int n = 0;
    while (*s && n < HELP_MAXL) {
        const unsigned char* p = (const unsigned char*)s;
        const unsigned char* lastSpace = NULL;
        int w = 0, len;
        while (*p) {
            int cw;
            const int cl = big_char(p, &cw);
            if (w + cw > maxw) {
                break;
            }
            if (*p == ' ') {
                lastSpace = p;
            }
            w += cw;
            p += cl;
        }
        if (*p && lastSpace != NULL && lastSpace > (const unsigned char*)s) {
            p = lastSpace;
        }
        len = (int)(p - (const unsigned char*)s);
        if (len <= 0) {
            break;
        }
        if (len > 255) {
            len = 255;
        }
        memcpy(lines[n], s, (size_t)len);
        lines[n][len] = 0;
        n++;
        s += len;
        while (*s == ' ') {
            s++;
        }
    }
    return n;
}

/* ---- tela ---- */

#define TAB_Y 8
#define TAB_H 30
#define LIST_Y 46
#define ROW_H 30
#define LIST_ROWS 9
#define LIST_H (LIST_ROWS * ROW_H + 10)
#define HELP_Y (LIST_Y + LIST_H + 6)
#define HELP_LINES 4
#define HELP_LH 26
#define HELP_H (HELP_LINES * HELP_LH + 10)
#define FOOT_Y (HELP_Y + HELP_H + 4)
#define VAL_X1 (W - 34) /* fim da coluna dos valores (a barra de rolagem fica depois) */

static int g_tabScroll; /* primeira aba visivel */

static void draw_tabs(void) {
    int x, i, wsum = 0, ws[TAB_COUNT];
    const int left = 30, right = W - 30;
    for (i = 0; i < TAB_COUNT; i++) {
        ws[i] = big_width(tr(k_tabEn[i], k_tabPt[i])) + 28;
        wsum += ws[i] + 6;
    }
    /* as abas que cabem a partir de g_tabScroll; a escolhida sempre visivel */
    if (g_tab < g_tabScroll) {
        g_tabScroll = g_tab;
    }
    for (;;) {
        int w = 0;
        for (i = g_tabScroll; i <= g_tab; i++) {
            w += ws[i] + 6;
        }
        if (w <= right - left || g_tabScroll >= g_tab) {
            break;
        }
        g_tabScroll++;
    }
    if (wsum <= right - left) {
        g_tabScroll = 0;
    }
    x = left;
    for (i = g_tabScroll; i < TAB_COUNT; i++) {
        const char* name = tr(k_tabEn[i], k_tabPt[i]);
        if (x + ws[i] > right) {
            break;
        }
        if (i == g_tab) {
            shadow(x, TAB_Y, ws[i], TAB_H, TAB_H / 2);
            rrect(x, TAB_Y, ws[i], TAB_H, TAB_H / 2, TH(tabOn), 255);
            big(x + 14, TAB_Y + 2, name, TH(tabOnText));
        } else {
            rrect(x, TAB_Y, ws[i], TAB_H, TAB_H / 2, TH(tabOff), TH(tabOffA));
            big(x + 14, TAB_Y + 2, name, TH(tabOffText));
        }
        x += ws[i] + 6;
    }
    if (g_tabScroll > 0) {
        chevron(14, TAB_Y + TAB_H / 2, 7, 0, TH(tabOffText));
    }
    if (i < TAB_COUNT) {
        chevron(W - 14, TAB_Y + TAB_H / 2, 7, 1, TH(tabOffText));
    }
}

static void draw_scrollbar(int top, int n) {
    const int x = W - 22, y0 = LIST_Y + 10, h = LIST_H - 20;
    int th, ty;
    if (n <= LIST_ROWS) {
        return;
    }
    rrect(x, y0, 5, h, 2, TH(track), 255);
    th = h * LIST_ROWS / n;
    if (th < 18) {
        th = 18;
    }
    ty = y0 + (h - th) * top / (n - LIST_ROWS);
    rrect(x, ty, 5, th, 2, TH(accent), 255);
}

/* o valor de uma opcao, a direita da linha; devolve onde comeca */
static int draw_value(const Item* it, int y, int isSel) {
    const int locked = item_locked(it);
    const int v = is_action(it) ? 0 : (locked && !strcmp(it->name, "max_resolution")) ? 320 : *port_option_ptr(it->name);
    const int cy = y + 13; /* meio da linha */
    const char* nm;
    char buf[64];
    int x0;
    if (is_action(it)) {
        if (g_restoreArmed == action_id(it->name) && miyoo_now_ms() - g_restoreAt < 4000.0) {
            const char* t = tr("A again to confirm", "A de novo: confirma");
            x0 = VAL_X1 - big_width(t);
            big(x0, y, t, TH(warn));
            return x0;
        }
        /* botao redondo "A" */
        circle((float)(VAL_X1 - 13), (float)cy, 12.0f, isSel ? TH(accent) : TH(track), 255);
        big(VAL_X1 - 13 - big_width("A") / 2, y, "A", isSel ? 0xFFFFFFu : TH(dim));
        return VAL_X1 - 26;
    }
    if (it->min == 0 && it->max == 1 && choice_name(it->name, 0) == NULL) {
        /* chave liga/desliga */
        const int sw = 48, sh = 26;
        x0 = VAL_X1 - sw;
        rrect(x0, cy - sh / 2, sw, sh, sh / 2, v ? TH(swOn) : TH(swOff), 255);
        shadow(v ? x0 + sw - sh + 3 : x0 + 3, cy - sh / 2 + 3, sh - 6, sh - 6, (sh - 6) / 2);
        circle((float)(v ? x0 + sw - sh / 2 : x0 + sh / 2), (float)cy, (float)(sh / 2 - 3), TH(knob), 255);
        return x0;
    }
    nm = !strcmp(it->name, "language") ? NULL : choice_name(it->name, v);
    if (!strcmp(it->name, "language")) {
        static const char* const en[7] = { "English", "Portuguese", "Japanese", "German", "French", "Italian", "Spanish" };
        static const char* const pt[7] = { "Inglês", "Português", "Japonês", "Alemão", "Francês", "Italiano", "Espanhol" };
        const int k = v < 0 || v > 6 ? 0 : v;
        nm = lang() == 1 ? pt[k] : tr(en[k], en[k]);
    }
    if (nm != NULL) {
        /* escolha com nome: pilula com setinhas */
        const int w = big_width(nm), pw = w + 44;
        x0 = VAL_X1 - pw;
        rrect(x0, cy - 13, pw, 26, 13, isSel ? 0xFFFFFFu : TH(track), isSel ? 40 : 255);
        chevron(x0 + 12, cy, 5, 0, isSel ? TH(accent) : TH(dim));
        big(x0 + 22, y, nm, isSel ? TH(selText) : TH(text));
        chevron(VAL_X1 - 12, cy, 5, 1, isSel ? TH(accent) : TH(dim));
        return x0;
    }
    {
        /* numero com barra fina e bolinha */
        const int bw = 104, bx = VAL_X1 - 60 - bw;
        int fill = it->max > it->min ? (v - it->min) * bw / (it->max - it->min) : 0;
        const int offv = (!strcmp(it->name, "autosave") || !strcmp(it->name, "dpad_smooth_turn") ||
                          !strcmp(it->name, "exact_filter_close")) ? 0
                         : !strcmp(it->name, "sprint_speed") ? 100 : -99999;
        fill = fill < 0 ? 0 : (fill > bw ? bw : fill);
        rrect(bx, cy - 3, bw, 6, 3, isSel ? 0xFFFFFFu : TH(track), isSel ? 60 : 255);
        if (fill > 0) {
            rrect(bx, cy - 3, fill, 6, 3, locked ? TH(dim) : TH(fill), 255);
        }
        circle((float)(bx + fill), (float)cy, 7.0f, locked ? TH(dim) : isSel ? TH(knob) : TH(fill), 255);
        if (v == offv) {
            snprintf(buf, sizeof buf, "%s", tr("off", "não"));
        } else {
            snprintf(buf, sizeof buf, "%d", v);
        }
        big_right(VAL_X1, y, buf, locked ? TH(dim) : isSel ? TH(selText) : TH(text));
        return bx - 8;
    }
}

/* descricao: no cartao de baixo cabem HELP_LINES linhas. Maior que isso: mostra as primeiras e, com a escolha parada
 * por HELP_GROW_MS, o cartao cresce para cima, por cima da lista, ate caber o texto inteiro (sem rolagem automatica:
 * era dificil de ler); qualquer botao volta ao tamanho normal (miyoo_menu_key renova g_selAt) */
#define HELP_GROW_MS 1000.0
#define HELP_GROW_ANIM_MS 320.0
/* desenho parcial enquanto o cartao cresce: o resto da tela nao muda, entao a imagem inteira de antes (g_base) e
 * copiada e so o cartao e desenhado de novo (redesenhar o menu todo, com o fundo translucido e as letras suavizadas,
 * pesava: a animacao saia aos trancos) */
static int g_helpState; /* (do ultimo desenho) 0 cabe no cartao, 1 esperando para crescer, 2 crescendo, 3 crescido */
static char g_helpText[1024];
static uint32_t* g_base;
static int g_baseOk, g_baseTab, g_baseSel;
static double g_baseAt;
static void draw_help(const char* s) {
    static char lines[HELP_MAXL][256];
    const int n = help_split(s, W - 64, lines);
    int i, shown = n < HELP_LINES ? n : HELP_LINES, y0 = HELP_Y;
    if (n > HELP_LINES) {
        const double t = miyoo_now_ms() - g_selAt;
        g_helpState = 3;
        if (t < HELP_GROW_MS) {
            g_helpState = 1;
            g_wakeAt = g_selAt + HELP_GROW_MS; /* (so redesenha na hora de crescer) */
            chevron(W - 28, HELP_Y + HELP_H - 14, 6, 3, TH(accent));
        } else {
            /* cresce para cima (animado), o fundo do cartao no mesmo lugar */
            /* (o texto sobe junto com a borda de cima; as linhas de baixo aparecem aos poucos pela borda de baixo, que
             * fica parada: as letras sao cortadas na altura do cartao) */
            int full = n * HELP_LH + 10, h = full;
            const float k = (float)((t - HELP_GROW_MS) / HELP_GROW_ANIM_MS);
            if (full > HELP_Y + HELP_H - LIST_Y) {
                full = HELP_Y + HELP_H - LIST_Y;
            }
            h = full;
            if (k < 1.0f) {
                const float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k); /* (sai rapido e freia no fim) */
                g_anim = 1;
                g_helpState = 2;
                h = HELP_H + (int)((float)(full - HELP_H) * e + 0.5f);
            }
            y0 = HELP_Y + HELP_H - h;
            shadow(10, y0, W - 20, h, 14);
            rrect(10, y0, W - 20, h, 14, TH(help), 255);
            shown = n;
            g_clipY0 = y0 + 2;
            g_clipY1 = HELP_Y + HELP_H - 2;
        }
    }
    for (i = 0; i < shown; i++) {
        big(24, y0 + 6 + i * HELP_LH, lines[i], TH(helpText));
    }
    g_clipY0 = 0;
    g_clipY1 = H;
}

/* dica do rodape: uma "tecla" (pilula com o nome do botao) e o que ela faz; devolve o x do fim */
static int key_hint(int x, const char* key, const char* what) {
    const int kw = miyoo_menu_text_width(key) + 12;
    rrect(x, FOOT_Y + 2, kw, 20, 6, TH(chipBg), 255);
    text(x + 6, FOOT_Y + 4, key, TH(chipText));
    x += kw + 6;
    x = text(x, FOOT_Y + 4, what, TH(foot));
    return x + 16;
}

static void draw_footer(void) {
    int x = 16;
    if (g_status[0] && miyoo_now_ms() < g_statusUntil) {
        g_anim = 1;
        big_fit(16, W - 16, FOOT_Y - 2, g_status, TH(status), 1); /* (aviso comprido rola, como os nomes) */
        return;
    }
    x = key_hint(x, "L1 R1", tr("tab", "aba"));
    x = key_hint(x, tr("Up Down", "cima baixo"), tr("choose", "escolhe"));
    x = key_hint(x, "A", tr("change", "muda"));
    key_hint(x, "B", tr("close", "fecha"));
}

/* chamado pela thread da tela com a pagina de tras ja com o fundo (a ultima imagem do jogo, escurecida) */
void miyoo_menu_render(uint32_t* page) {
    int first, n, i, *sel, *top;
    char buf[64];
    if (!g_dirty && g_baseOk && g_baseTab == g_tab && g_baseSel == g_sel[g_tab] && g_baseAt == g_selAt && !g_confirm && !g_upd &&
        (g_helpState == 1 || g_helpState == 2)) {
        /* so o cartao da descricao mudou (crescendo): a imagem de antes e o cartao por cima */
        g_anim = 0;
        g_page = page;
        memcpy(page, g_base, (size_t)W * H * 4);
        draw_help(g_helpText);
        return;
    }
    g_dirty = 0;
    g_anim = 0;
    g_helpState = 0;
    g_wakeAt = 0.0;
    g_page = page;
    g_th = &k_themes[port_opt_menu_theme == 1 ? 1 : 0];
    n = tab_count(g_tab, &first);
    sel = &g_sel[g_tab];
    top = &g_top[g_tab];
    if (*sel >= n) {
        *sel = n > 0 ? n - 1 : 0;
    }
    if (*sel < *top) {
        *top = *sel;
    } else if (*sel >= *top + LIST_ROWS) {
        *top = *sel - LIST_ROWS + 1;
    }
    if (*top > n - LIST_ROWS) {
        *top = n - LIST_ROWS > 0 ? n - LIST_ROWS : 0;
    }

    /* fundo e cartoes */
    rect_a(0, 0, W, H, TH(bg), TH(bgA));
    shadow(10, LIST_Y, W - 20, LIST_H, 14);
    rrect(10, LIST_Y, W - 20, LIST_H, 14, TH(card), TH(cardA));
    shadow(10, HELP_Y, W - 20, HELP_H, 14);
    rrect(10, HELP_Y, W - 20, HELP_H, 14, TH(help), TH(helpA));
    draw_tabs();

    if (g_upd) {
        /* "What's new": a versao nova e o texto do release */
        const int x0 = 24, y0 = 40, bw = W - 48, bh = 400, vis = 11, lh = 26;
        int j;
        rect_a(0, 0, W, H, 0x000000u, 90);
        shadow(x0, y0, bw, bh, 18);
        rrect(x0, y0, bw, bh, 18, TH(card), 255);
        if (!g_updWrapped) {
            /* o texto em linhas da largura da janela (cada paragrafo do release separado) */
            const char* p = g_updText;
            g_updN = 0;
            while (*p && g_updN < UPD_MAXL) {
                static char para[1024];
                static char tmp[HELP_MAXL][256];
                const char* e = strchr(p, '\n');
                const size_t k = e != NULL ? (size_t)(e - p) : strlen(p);
                const size_t kk = k < sizeof para - 1 ? k : sizeof para - 1;
                int m, q;
                memcpy(para, p, kk);
                para[kk] = 0;
                if (kk == 0) {
                    g_updLines[g_updN++][0] = 0;
                } else {
                    m = help_split(para, bw - 48, tmp);
                    for (q = 0; q < m && g_updN < UPD_MAXL; q++) {
                        snprintf(g_updLines[g_updN++], 256, "%s", tmp[q]);
                    }
                }
                p += k;
                if (*p == '\n') {
                    p++;
                }
            }
            g_updWrapped = 1;
        }
        big(x0 + 24, y0 + 16, g_updTitle, TH(accent));
        for (j = 0; j < vis && g_updTop + j < g_updN; j++) {
            big(x0 + 24, y0 + 56 + j * lh, g_updLines[g_updTop + j], TH(text));
        }
        if (g_updTop > 0) {
            chevron(x0 + bw - 24, y0 + 60, 6, 2, TH(accent));
        }
        if (g_updTop + vis < g_updN) {
            chevron(x0 + bw - 24, y0 + 56 + vis * lh - 10, 6, 3, TH(accent));
        }
        big(x0 + 24, y0 + bh - 36,
            tr("Up/Down: scroll   B: close", "Cima/baixo: rola   B: fecha"),
            TH(dim));
        return;
    }
    if (g_confirm) {
        /* essas opcoes so valem reiniciando: reinicia ou cancela */
        const int x0 = 44, y0 = 120, bw = W - 88, bh = 190;
        rect_a(0, 0, W, H, 0x000000u, 90);
        shadow(x0, y0, bw, bh, 18);
        rrect(x0, y0, bw, bh, 18, TH(card), 255);
        big(x0 + 24, y0 + 20, tr("These changes need a game restart.", "Essas mudanças exigem reiniciar o jogo."), TH(text));
        big(x0 + 24, y0 + 50, tr("Restart now?", "Reiniciar agora?"), TH(dim));
        rrect(x0 + 24, y0 + 96, bw - 48, 34, 17, TH(accent), 255);
        big(x0 + 40, y0 + 100, tr("A  Restart (the game goes on from where it is)", "A  Reiniciar (o jogo continua de onde está)"),
            0xFFFFFFu);
        rrect(x0 + 24, y0 + 140, bw - 48, 34, 17, TH(track), 255);
        big(x0 + 40, y0 + 144, tr("B  Cancel (the options go back)", "B  Cancelar (as opções voltam ao que eram)"),
            TH(text));
        return;
    }

    if (IS_STATES(g_tab)) {
        const int listX1 = 336;
        if (g_slotsDirty) {
            load_slots();
        }
        for (i = 0; i < LIST_ROWS && *top + i < NSLOTS; i++) {
            const int s0 = *top + i, y = LIST_Y + 5 + i * ROW_H, isSel = s0 == *sel;
            if (isSel) {
                rrect(18, y, listX1 - 18, ROW_H - 2, 10, TH(sel), 255);
            }
            snprintf(buf, sizeof buf, tr("State %2d", "Estado %2d"), s0 + 1);
            big(30, y + 1, buf, isSel ? TH(selText) : TH(text));
            if (g_slotState[s0] == 0) {
                big(170, y + 1, tr("(empty)", "(vazio)"), isSel ? TH(selDim) : TH(dim));
            } else if (g_slotState[s0] < 0) {
                big(170, y + 1, tr("(other version)", "(outra versão)"), isSel ? TH(selDim) : TH(dim));
            } else {
                const time_t t = (time_t)g_slotWhen[s0];
                struct tm tmv;
                localtime_r(&t, &tmv);
                snprintf(buf, sizeof buf, "%02d/%02d %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min);
                big(170, y + 1, buf, isSel ? TH(selText) : TH(text));
            }
        }
        draw_scrollbar(*top, NSLOTS);
        {
            /* miniatura do estado escolhido (256x192), com cantos redondos, centrada na altura da lista */
            const int tx0 = listX1 + 14, ty0 = LIST_Y + (LIST_H - THUMB_H) / 2, s0 = *sel, rr = 12;
            int x2, y2;
            shadow(tx0, ty0, THUMB_W, THUMB_H, rr);
            if (g_thumbOk[s0]) {
                for (y2 = 0; y2 < THUMB_H; y2++) {
                    for (x2 = 0; x2 < THUMB_W; x2++) {
                        const uint16_t c = g_thumbs[s0][y2 * THUMB_W + x2];
                        const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                        const int cxo = x2 < rr ? rr - x2 : (x2 >= THUMB_W - rr ? x2 - (THUMB_W - rr - 1) : 0);
                        const int cyo = y2 < rr ? rr - y2 : (y2 >= THUMB_H - rr ? y2 - (THUMB_H - rr - 1) : 0);
                        if (cxo && cyo && cxo * cxo + cyo * cyo > rr * rr) {
                            continue; /* (canto redondo) */
                        }
                        px(tx0 + x2, ty0 + y2, (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2)));
                    }
                }
            } else {
                const char* t = tr("no picture", "sem imagem");
                rrect(tx0, ty0, THUMB_W, THUMB_H, rr, TH(track), 255);
                big(tx0 + THUMB_W / 2 - big_width(t) / 2, ty0 + THUMB_H / 2 - 13, t, TH(dim));
            }
        }
        big(24, HELP_Y + 6,
            g_tab == TAB_LOAD ? tr("A loads the state (and resumes the game).", "A carrega o estado (e volta ao jogo).")
                              : tr("A saves here (press A again to overwrite).", "A salva aqui (A de novo para gravar por cima)."),
            TH(helpText));
        big(24, HELP_Y + 6 + HELP_LH,
            tr("Separate from the quick state (MENU+R2 / MENU+L2).", "Separados do estado rápido (MENU+R2 / MENU+L2)."),
            TH(helpText));
        draw_footer();
        return;
    }

    for (i = 0; i < LIST_ROWS && *top + i < n; i++) {
        const Item* it = tab_item(g_tab, *top + i);
        const int y = LIST_Y + 5 + i * ROW_H, isSel = *top + i == *sel;
        int vx;
        if (isSel) {
            rrect(18, y, W - 48, ROW_H - 2, 10, TH(sel), 255);
            rrect(22, y + 7, 4, ROW_H - 16, 2, TH(accent), 255);
        }
        if (is_header(it)) {
            /* titulo de uma parte da aba: texto na cor de destaque e um traco ate o fim da linha */
            const char* t = item_label(it);
            const int tw = big_width(t);
            big(34, y + 1, t, TH(accent));
            if (34 + tw + 12 < VAL_X1) {
                rrect(34 + tw + 12, y + ROW_H / 2 - 1, VAL_X1 - (34 + tw + 12), 2, 1, TH(accent), 255);
            }
            continue;
        }
        vx = draw_value(it, y + 1, isSel);
        big_fit(34, vx - 14, y + 1, item_label(it), item_locked(it) ? TH(dim) : isSel ? TH(selText) : TH(text), isSel);
    }
    draw_scrollbar(*top, n);

    /* descricao */
    if (n > 0) {
        static char tmp[1024];
        const Item* it = tab_item(g_tab, *sel);
        const char* h = (is_action(it) && !strcmp(it->name, "@update"))
                            ? tr("Looks for a new version of the port on GitHub. Updates fix bugs and bring new features, so it "
                                 "is worth checking now and then. Needs Wi-Fi turned on in OnionOS. Press A.",
                                 "Procura uma versão nova do port no GitHub. As atualizações corrigem bugs e trazem funções "
                                 "novas, então vale a pena procurar de vez em quando. Precisa do Wi-Fi ligado no OnionOS. "
                                 "Aperte A.")
                        : (is_action(it) && !strcmp(it->name, "@reset"))
                            ? tr("Saves the game (autosave) and starts it again from the beginning, with the intro, like the "
                                 "N64 reset button. Press A twice.",
                                 "Salva o jogo (salvar sozinho) e começa de novo do início, com a abertura, como o botão de "
                                 "reset do N64. Aperte A duas vezes.")
                        : (is_action(it) && !strcmp(it->name, "@keys"))
                            ? tr("Every button goes back to the port's layout: B jump, Y sword, L2 / X / R2 C-Left / C-Down / "
                                 "C-Right (R2 held: walk slowly), SELECT tap C-Up, L1 Z, R1 R. Press A twice.",
                                 "Todos os botões voltam ao esquema do port: B pulo, Y espada, L2 / X / R2 C-esquerda / "
                                 "C-baixo / C-direita (R2 segurado: andar devagar), toque no SELECT C-cima, L1 Z, R1 R. "
                                 "Aperte A duas vezes.")
                        : (is_action(it) && !strcmp(it->name, "@quit"))
                            ? tr("Saves the game (autosave) and quits, without the Game Switcher. Press A twice.",
                                 "Salva o jogo (salvar sozinho) e sai, sem o Game Switcher. Aperte A duas vezes.")
                        : is_action(it) ? tr("Every option in this menu goes back to the port's factory values (and the "
                                             "diagnostics are turned off). Press A twice.",
                                             "Todas as opções deste menu voltam aos valores de fábrica do port (e os "
                                             "diagnósticos são desligados). Aperte A duas vezes.")
                                        : item_help(it);
        if (!is_action(it) && it->min == 0 && it->max == 1 && choice_name(it->name, 0) == NULL) {
            h = toggle_help(h, tmp, sizeof tmp);
        }
        draw_help(h);
        if (g_helpState == 1) {
            /* vai crescer: guarda a imagem (o cartao crescido cobre o cartao pequeno) para o desenho parcial */
            snprintf(g_helpText, sizeof g_helpText, "%s", h);
            if (g_base == NULL) {
                g_base = (uint32_t*)malloc((size_t)W * H * 4);
            }
            g_baseOk = 0;
        }
    }
    draw_footer();
    if (g_helpState == 1 && g_base != NULL) {
        memcpy(g_base, page, (size_t)W * H * 4);
        g_baseOk = 1;
        g_baseTab = g_tab;
        g_baseSel = *sel;
        g_baseAt = g_selAt;
    }
}
