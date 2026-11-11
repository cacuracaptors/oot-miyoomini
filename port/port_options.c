/*
 * port_options.c -- opcoes de "qualidade de vida" (ideias do Ship of Harkinian), lidas do arquivo
 * options.txt na pasta do jogo (o opcoes.txt antigo, em portugues, e migrado uma vez). Se o arquivo nao existir, ele e criado com os valores padrao e a
 * explicacao de cada opcao. Formato: uma opcao por linha, "nome = 0 ou 1"; # comeca comentario.
 *
 * Os ajustes no codigo do jogo (tools/oot-patch.py) leem as variaveis port_opt_*.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef TARGET_MIYOO
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

int port_opt_segurar_b = 1;    /* segurar B passa os textos rapido */
int port_opt_pular_cenas = 1;  /* START pula as cenas */
int port_opt_alarme_vida = 1;  /* bipe de vida baixa */
int port_opt_sem_tremor = 0;   /* camera nao "treme" com vida baixa (SoH DisableCritWiggle) */
int port_opt_camera_distance = 100; /* distancia da camera que segue o Link, em % (70..130; z_camera.c) */
int port_opt_autosave = 10;   /* SoH: salvar sozinho a cada N minutos (0 = desligado) */
int port_opt_text_speed = 2; /* SoH: letras por quadro no texto (1 = original) */
/* volume da musica (sequencias da musica, das fanfarras e dos sons de ambiente) e dos efeitos, em % do original */
int port_opt_music_volume = 100;
int port_opt_sound_volume = 100;

/* src/audio/internal/effects.c (tools/oot-patch.py): o volume de cada tocador de sequencias. 2 = efeitos (sons do jogo,
 * vozes, ocarina); 0, 1 e 3 = musica principal, fanfarras e a musica de fundo extra */
int Port_AudioVolPct(int player) {
    const int p = player == 2 ? port_opt_sound_volume : port_opt_music_volume;
    return p < 0 ? 0 : p > 200 ? 200 : p;
}

/* o volume do jogo vale ao quadrado (effects.c: appliedVolume = volume^2): a raiz do fator da a amplitude pedida */
float Port_AudioVolScale(int player) {
    return sqrtf((float)Port_AudioVolPct(player) / 100.0f);
}
int port_opt_tot_medallion_colors = 0; /* SoH: medalhoes coloridos no chao do Templo do Tempo */
int port_opt_better_bombchu_shopping = 0; /* SoH: bombchus nao esgotam, pacote de 10 por 99 */
int port_opt_quit_fishing_at_door = 0; /* SoH: parar de pescar saindo pela porta */
int port_opt_bau_rapido = 0;   /* baus abrem com o chute rapido */
int port_opt_ocarina_rapida = 0; /* nao repete a melodia depois de tocar */
int port_opt_barra_inimigo = 0;  /* barra de vida do inimigo mirado */
int port_opt_atalho_roupa = 1;   /* SELECT+direita/esquerda troca botas/tunica */
int port_opt_inverter_mira = 1;  /* primeira pessoa: para cima olha para cima */
int port_opt_mira_suave = 1;     /* primeira pessoa: aceleracao no direcional */
int port_opt_caixas_opacas = 0;  /* caixas de dialogo e minimapa sem transparencia */
int port_opt_retro_brightness_base = -1; /* brilho de antes do filtro retro (menu_miyoo.c); -1 = nenhum */
int port_opt_halo_folga = 600;   /* folga do teste de profundidade dos halos de luz (z do jogo, 15.3) */
/* melhorias do Ship of Harkinian (tools/oot-patch-soh.py) */
int port_opt_remember_save_location = 0;
int port_opt_gs_token_count = 0;
int port_opt_heart_piece_count = 0;
int port_opt_heart_container_count = 0;
int port_opt_better_owl = 0;
int port_opt_instant_putaway = 0;
int port_opt_sleeping_waterfall = 0;
int port_opt_faster_heavy_lift = 0;
int port_opt_skip_breath_animation = 0;
int port_opt_climb_speed = 0;
int port_opt_block_push_speed = 0;
int port_opt_crawl_speed = 1;
int port_opt_king_zora_speed = 1;
int port_opt_instant_scarecrow = 0;
int port_opt_no_skulltula_freeze = 0;
int port_opt_skip_save_confirmation = 0;
int port_opt_forge_time_days = 3;
int port_opt_door_locks_both_sides = 0;
int port_opt_remember_minimap = 0;
int port_opt_file_select_time_flow = 0;
int port_opt_dynamic_wallet_icon = 0;
int port_opt_visual_agony = 0;
int port_opt_no_dropped_ocarina_inputs = 0;
int port_opt_nuts_explode_bombs = 0;
int port_opt_persistent_masks = 0;
int port_opt_no_first_person_bombchus = 0;
int port_opt_fast_boomerang = 0;
/* lote 127: melhorias de outros ports (SoH; tools/oot-patch-soh.py) */
int port_opt_faster_pause_menu = 0;
int port_opt_faster_bottle_empty = 0;
int port_opt_faster_rupee_counter = 0;
int port_opt_faster_shadow_ship = 0;
int port_opt_rebottle_blue_fire = 0;
int port_opt_no_heart_animation = 0;
int port_opt_disable_title_card = 0;
int port_opt_dampe_all_night = 0;
int port_opt_daytime_gold_skulltulas = 0;
int port_opt_shops_open_all_hours = 0;
int port_opt_fast_drops = 0;
int port_opt_skip_navi_forced_talk = 0;
int port_opt_skip_npc_forced_talk = 0;
int port_opt_skip_warp_cutscenes = 0;
int port_opt_skip_tower_escape = 0;
int port_opt_skip_child_stealth = 0;
int port_opt_skip_jabu_fish = 0;
int port_opt_market_sneak = 0;
int port_opt_arrow_cycle = 0;
int port_opt_bow_reticle = 0;
int port_opt_hookshot_reticle_color = 0;
int port_opt_time_travel = 0;
int port_opt_pause_warp = 0;
int port_opt_mask_select = 0;
int port_opt_mm_bunny_hood = 0;
int port_opt_ask_to_equip = 0;
int port_opt_minimal_ui = 0;
int port_tt_novoid; /* (codigo do jogo: viagem no tempo sem o dano da queda) */

/* teclas personalizaveis (opcoes button_*_tap / _hold; port/miyoo/input_miyoo.c): por botao (A, B, X, Y, L1, R1, L2, R2,
 * START, SELECT) a acao do toque e a de segurar. Os valores iniciais sao os da tabela k_padrao */
int port_opt_btn_hold_ms = 250; /* a partir de quanto tempo o botao conta como segurado */
int port_opt_btn[20] = { 1, 1, 3, 3, 9, 9, 2, 2, 4, 4, 5, 5, 10, 10, 11, 12, 7, 7, 8, 0 };
/* as opcoes antigas manual_jump, jump_on_b e slow_walk (ate o lote 196): so lidas de um options.txt antigo, para
 * montar as teclas iguais (port_options_load) */
static int g_oldJump = -1, g_oldJumpB = -1, g_oldSlow = -1, g_sawButtons;
int port_opt_ledge_grab = 1; /* z_player.c: 0 = ao sair andando de uma beirada, cai em vez de se agarrar nela */
int port_opt_air_steering = 1; /* z_player.c: direcao no ar (Port_AirSteer) */
int port_opt_better_roll = 0; /* z_player.c: rolada encadeada e com direcao */
int port_opt_fast_farores = 0;
int port_opt_skip_arrow_animation = 0;
int port_opt_better_farore = 0;
int port_opt_fix_zora_hint = 1;
int port_opt_fix_camera_surface_hop = 1;
int port_opt_fix_enemy_spawns_water = 1;
int port_opt_fix_camera_drift = 1;
int port_opt_fix_camera_swing = 1;
int port_opt_fix_ledge_swing_rate = 1;
int port_opt_fix_menu_lr = 1;
int port_opt_fix_dungeon_entrance_icons = 1;
int port_opt_fix_gerudo_warrior_clothes = 1;
int port_opt_fix_eyes_open_sleeping = 1;
int port_opt_fix_silver_rupee_jingle = 1;
int port_opt_fix_darunia_dance = 1;
int port_opt_red_ganon_blood = 0;
int port_opt_pulsate_boss_icon = 0;
int port_opt_fix_gravedigging_tour = 1;
int port_opt_fix_floor_switches = 1;
int port_opt_fix_deku_nut_upgrade = 1;
int port_opt_fix_broken_giants_knife = 1;
int port_opt_fix_vine_fall = 1;
int port_opt_fix_two_handed_idle = 1;
int port_opt_cheat_dampe_always_win = 0;
int port_opt_frogs_note_timer = 1;
int port_opt_lost_woods_customize = 0;
int port_opt_lost_woods_starting_notes = 5;
int port_opt_lost_woods_round1_notes = 5;
int port_opt_lost_woods_round2_notes = 6;
int port_opt_lost_woods_round3_notes = 8;
int port_opt_fishing_guarantee_bite = 0;
int port_opt_fishing_fish_never_escape = 0;
int port_opt_fishing_min_weight_child = 0;
int port_opt_fishing_min_weight_adult = 0;
int port_opt_cheat_infinite_money = 0;
int port_opt_cheat_infinite_health = 0;
int port_opt_cheat_infinite_ammo = 0;
int port_opt_cheat_infinite_magic = 0;
int port_opt_cheat_infinite_nayrus_love = 0;
int port_opt_cheat_freeze_time = 0;
int port_opt_cheat_no_item_restrictions = 0;
int port_opt_cheat_disable_sandstorm = 0;
int port_opt_cheat_moon_jump_on_l = 0;
int port_input_select_a;
int port_opt_manual_jump = 1; /* algum botao e o pulo (input_miyoo.c calcula pelas teclas; z_player.c le) */
int port_opt_auto_jump = 1; /* 0 (com um botao de pulo) = Link nao pula sozinho nas beiradas (z_player.c) */
int port_opt_sprint_speed = 150;
int port_opt_jump_strength = 56; /* forca minima do pulo manual (x10) */ /* corrida segurando A depois da rolada (%) */ /* pulo manual (input_miyoo.c, z_player.c) */
int port_input_jump, port_input_frames, port_jump_stamp, port_jump_mode;
int port_scene_transition = 1; /* z_play.c: troca de cena em andamento (gfx_hd.c) */
int port_text_open, port_text_choice; /* (z_play.c: caixa de texto aberta / de escolha; segurar B) */ /* (o jogo avisa quando Link pode agir) */ /* port/miyoo/input_miyoo.c: SELECT+A segurados (pulo da lua) */
int port_opt_cheat_bomb_timer_percent = 100;
int port_opt_cheat_hookshot_reach_percent = 100;
int port_opt_cheat_no_redead_freeze = 0;
int port_opt_cheat_fireproof_deku_shield = 0;
int port_opt_cheat_infinite_epona_boost = 0;
int port_opt_cheat_deku_stick = 0;
int port_opt_cheat_hookshot_everything = 0;
int port_opt_cheat_super_tunic = 0;
int port_opt_cheat_shield_two_handed = 0;
int port_opt_cheat_drops_dont_despawn = 0;
int port_opt_cheat_no_fish_despawn = 0;
int port_opt_cheat_no_bugs_despawn = 0;
int port_opt_cheat_gs_targetable = 0;
int port_opt_cheat_no_keese_guay_target = 0;
extern int gx_fused, gx_stream; /* port/gfx/gfx_fast.inc: desenho (para comparar no aparelho) */
extern int gx_tex_prefilter;    /* port/gfx/gfx_fast.inc: filtro de textura pre-calculado */
extern int gx_sort_opaque, gx_sort_chunk, gx_ge_on;      /* port/gfx/gfx_fast.inc: opacos do mais perto para o mais longe */
extern int port_opt_hd_logo;    /* port/gfx/gfx_hd.c: logo da abertura com as texturas novas (texturas_hd.pak) */
extern int gx_hd_filtro;        /* port/gfx/gfx_fast.inc: filtro nas texturas novas */
extern int gx_menu_320, gx_logo_opaca, gx_logo_320, gx_hr_reducao_sempre, gx_hr_direto, gx_hr_zpontos, gx_ge_alfa, gx_hr_le, gx_hr_ctl2, port_texgen_old, gx_pontilhado, gx_arquivos_320, gx_filtro_exato_perto, gx_filtro_perto_fino, gx_ge_pequenas, gx_ge_mudando, gx_bg_ge, gx_tri_lazy; /* port/gfx/gfx_hires.c, gfx_rdp.c */
extern int gx_menu_opaco;
extern int gx_brilho;             /* port/gfx/gfx_rdp.c: brilho (gamma) */        /* port/gfx/gfx_rdp.c: menu de pausa sem transparencia */
extern int gx_ge_texto, gx_texto_320_opt;          /* port/gfx/gfx_ge.c: caixa de texto pelo acelerador */
extern int gx_fix_vanishing_paths; /* port/gfx/gfx_tri.c */
extern int gx_hd_perto;         /* port/gfx/gfx_fast.inc: filtro leve nas novas so perto da camera */
extern int os_rt;               /* platform/os_core.c: threads do jogo, som e tela em tempo real */
extern int os_rt_draw;          /* platform/os_core.c: desenho em tempo real */
extern int gx_slowcap_ms, gx_slowcap_after;       /* port/gfx/gfx_hook.c: grava quadros lentos */
extern int gx_trace;
int port_opt_test_warp = -1;      /* teste: abrir o jogo direto num lugar (numero da entrada), com o save de teste */
int port_opt_test_warp_adult = 0; /* ...com Link adulto */
extern int gx_hidden_stats; /* port/gfx/gfx_fast.inc: medicao dos triangulos escondidos no log */
extern int port_opt_reload_overlays; /* port/ovl_reset.c */
extern int gx_tex_meia; /* port/gfx/gfx_fast.inc: texturas ampliadas amostradas nos pixels pares */
int port_opt_lens_flare = 1;
int port_opt_light_hit_marks = 1; /* marca de acerto recortada (z_eff_ss_hitmark.c, tools/oot-patch-soh.py) */ /* reflexo do sol na lente (z_kankyo.c, tools/oot-patch.py) */
int port_opt_menu_theme = 1; /* cores do menu de opcoes (port/miyoo/menu_miyoo.c) */
/* contraste, matiz e saturacao da tela, pelo ajuste de cor do chip (port/miyoo/screen_miyoo.c): 0..20 como no Tweaks
 * do OnionOS; -1 = o do sistema */
int port_opt_screen_contrast = -1;
int port_opt_screen_hue = -1;
int port_opt_screen_saturation = -1;
extern int port_opt_perf_trace; /* port/perf_trace.c */            /* port/gfx/gfx_hook.c: registro quadro a quadro no log */
extern int gx_hr_max_opt, gx_hr_alvo, gx_hr_texto, gx_retro, gx_retro_forca, gx_retro_tv; /* port/gfx/gfx_hires.c: resolucao maior (dinamica) */
int port_opt_nucleos_fixos = 1;  /* desenho no nucleo 0, auxiliar do desenho no 1 */
int port_opt_clock = 1600;          /* MHz da CPU ao abrir o jogo (0 = o que o sistema deixar, 1200) */
int port_opt_clock_guard = 1; /* 1 = se o clock da CPU cair sozinho durante o jogo, pede de novo (a cada 10 s) */
extern int gx_agua_meia, gx_mistura_meia; /* gfx_fast.inc: texturas em meia resolucao (lote 247) */
extern int gx_mistura_desliza;           /* gfx_fast.inc: texturas que deslizam pre-misturadas (lote 183) */
extern int gx_ceu_mistura, gx_corte; /* gfx_fast.inc: ceu pre-misturado e recorte (teste da lentidao) */
extern int port_fi_enabled;      /* port/frame_interp.c: quadros intermediarios (30 por segundo) */
extern int port_opt_idioma;      /* port/lang.c: pacote de idioma */
extern int port_opt_extra_adiado; /* port/gfx/gfx_hook.c: quadro do jogo desenhado depois do 1/3 */
extern int port_opt_degraus;      /* port/gfx/gfx_hook.c: degraus entre 30 e 20 fps */
extern int port_opt_folga;        /* port/gfx/gfx_hook.c: vsyncs de atraso extra das imagens */
extern int gx_steal;              /* port/gfx/gfx_fast.inc: a principal pega linhas da auxiliar no fim */
#ifdef TARGET_MIYOO
extern int port_opt_direcional_suave; /* port/miyoo/input_miyoo.c: a direcao gira aos poucos */
extern int port_opt_rumble;           /* port/miyoo/rumble_miyoo.c: o motor do aparelho como Rumble Pak */
#endif

/* sec: secao no arquivo (cabecalho escrito quando muda); old: o nome no opcoes.txt antigo (em portugues),
 * lido uma vez para migrar os valores para o options.txt */
static const struct {
    const char* sec;
    const char* name;
    const char* old;
    int* var;
    const char* help;
} k_opts[] = {
    /* ---------------------------------------------------------------- General */
    { "General", "language", "idioma", &port_opt_idioma,
      "0 = English (original), 1 = Brazilian Portuguese (fan translation Zelda 64 BR v1.8, file idiomas/pt.oot), 2 = Japanese (the original Japanese text, also in the US cartridge), 3 = German, 4 = French (3 and 4 come from the European ROM: put the \"(Europe) (En,Fr,De) (Rev A)\" ROM, .zip or not, in the game folder once; the game makes idiomas/de.oot and fr.oot from it), 5 = Italian (fan translation Zelda 64 ITA v1.00 by XXcentury, file idiomas/it.oot), 6 = Spanish (fan translation by eduardo_a2j v2.2, file idiomas/es.oot). Takes effect when the game starts" },
    { "General", "frame_interpolation", "quadros_extras", &port_fi_enabled,
      "1 = 30 frames per second on screen (draws in-between frames; game speed does not change), 0 = 20 fps like the original" },
    { "General", "frame_rate_steps", "quadros_degraus", &port_opt_degraus,
      "1 = in heavy scenes the frame rate drops in steps (30, 26, 25, 24... down to 20 fps) instead of going straight to 20; 0 = only 30 or 20" },
    { "General", "cpu_clock", "clock", &port_opt_clock,
      "CPU MHz set when the game starts (e.g. 1500 = overclock; runs hotter and uses more battery); 0 = system default (1200). Default: 1800 on the Miyoo Mini Plus, 1600 on the other devices (Mini, Mini v4, Mini Flip)" },
    { "Performance", "clock_guard", "", &port_opt_clock_guard,
      "1 = if the CPU clock falls back by itself during the game (something in the system resets it), it is set again (checked every 10 s); 0 = off" },
    { "General", "menu_theme", "", &port_opt_menu_theme,
      "colors of this options menu (MENU+START) and of the short notices: 0 = dark green and gold, 1 = light (lime green, white and navy)" },
    /* ---------------------------------------------------------------- Controls */
#ifdef TARGET_MIYOO
    { "Controls", "dpad_smooth_turn", "direcional_suave", &port_opt_direcional_suave,
      "N = the D-pad direction turns gradually towards the new one (N ms per 45 degrees, e.g. 60), so turns are smooth like an analog stick; speed and stopping stay instant. 0 = 8 fixed directions" },
    { "Controls", "rumble", "", &port_opt_rumble,
      "1 = the device vibrates like the N64 Rumble Pak (hits, the Stone of Agony, fishing...); 0 = no vibration (the game sees no Rumble Pak)" },
#endif
    { "Controls", "natural_aiming", "inverter_mira", &port_opt_inverter_mira,
      "1 = when aiming and in first person (C-Up, slingshot, bow...) pressing up looks up; 0 = like the original (inverted)" },
    { "Controls", "smooth_aiming", "mira_suave", &port_opt_mira_suave,
      "1 = when aiming and in first person the D-pad accelerates gradually (a tap moves a little, holding speeds up); 0 = like the original" },
    /* teclas personalizaveis (port/miyoo/input_miyoo.c): o que cada botao faz num toque e segurado (lote 197) */
    { "Controls", "button_hold_ms", "", &port_opt_btn_hold_ms,
      "buttons with a different action for tap and hold (button_*): held this many ms or more counts as hold; released before that, as a tap (250 = default)" },
    { "Controls", "button_a_tap", "", &port_opt_btn[0],
      "what each button does. button_N_tap: a quick tap (released before button_hold_ms, without another button with it); button_N_hold: held longer. 0 = nothing, 1 = A, 2 = B (sword), 3 = jump (B in menus, text boxes and outside gameplay), 4 = Z (target), 5 = R (shield), 6 = L (minimap), 7 = START, 8 = C-Up, 9 = C-Down, 10 = C-Left, 11 = C-Right, 12 = walk slowly. The same number for tap and hold = the button works at once, like a normal button (no waiting). SELECT+another button are the fixed shortcuts (brightness, minimap, tunic/boots...), and MENU is not changeable" },
    { "Controls", "button_a_hold", "", &port_opt_btn[1],
      "A held (see button_a_tap)" },
    { "Controls", "button_b_tap", "", &port_opt_btn[2],
      "B tapped (see button_a_tap)" },
    { "Controls", "button_b_hold", "", &port_opt_btn[3],
      "B held (see button_a_tap)" },
    { "Controls", "button_x_tap", "", &port_opt_btn[4],
      "X tapped (see button_a_tap)" },
    { "Controls", "button_x_hold", "", &port_opt_btn[5],
      "X held (see button_a_tap)" },
    { "Controls", "button_y_tap", "", &port_opt_btn[6],
      "Y tapped (see button_a_tap)" },
    { "Controls", "button_y_hold", "", &port_opt_btn[7],
      "Y held (see button_a_tap)" },
    { "Controls", "button_l1_tap", "", &port_opt_btn[8],
      "L1 tapped (see button_a_tap)" },
    { "Controls", "button_l1_hold", "", &port_opt_btn[9],
      "L1 held (see button_a_tap)" },
    { "Controls", "button_r1_tap", "", &port_opt_btn[10],
      "R1 tapped (see button_a_tap)" },
    { "Controls", "button_r1_hold", "", &port_opt_btn[11],
      "R1 held (see button_a_tap)" },
    { "Controls", "button_l2_tap", "", &port_opt_btn[12],
      "L2 tapped (see button_a_tap)" },
    { "Controls", "button_l2_hold", "", &port_opt_btn[13],
      "L2 held (see button_a_tap)" },
    { "Controls", "button_r2_tap", "", &port_opt_btn[14],
      "R2 tapped (see button_a_tap)" },
    { "Controls", "button_r2_hold", "", &port_opt_btn[15],
      "R2 held (see button_a_tap)" },
    { "Controls", "button_start_tap", "", &port_opt_btn[16],
      "START tapped (see button_a_tap)" },
    { "Controls", "button_start_hold", "", &port_opt_btn[17],
      "START held (see button_a_tap)" },
    { "Controls", "button_select_tap", "", &port_opt_btn[18],
      "SELECT tapped (see button_a_tap)" },
    { "Controls", "button_select_hold", "", &port_opt_btn[19],
      "SELECT held (see button_a_tap)" },
    { "Controls", "auto_jump", "", &port_opt_auto_jump,
      "1 = Link jumps by himself when he runs off a ledge (like the original); 0 = no automatic jump: he falls off the ledge, and only the jump button jumps. The jump button is chosen in the button settings (button_*)" },
    { "Controls", "jump_strength", "", &port_opt_jump_strength,
      "manual jump: minimum jump strength x10 (56 = 5.6; higher = taller jump). The running jump uses the game's own value when it is stronger" },
    { "Controls", "ledge_grab", "", &port_opt_ledge_grab,
      "1 = like the original: walking off a ledge, Link grabs the edge and hangs from it; 0 = he just falls (useful with the manual jump and the automatic jump off). Vines, ladders and grabbing a ledge in mid-air still work" },
    { "Controls", "air_steering", "", &port_opt_air_steering,
      "1 = in the air (jumping or falling) the D-pad steers Link a little, up to 45 degrees to each side of the direction he left the ground in (a nudge, not a turn); 0 = like the original" },
    { "Controls", "sprint_speed", "", &port_opt_sprint_speed,
      "sprint: A makes Link roll; keep holding A after the roll and he runs faster, N% of the normal speed (150 = 1.5x); release A or stop to walk normally. 100 = off" },
    { "Controls", "quick_gear_swap", "atalho_roupa", &port_opt_atalho_roupa,
      "1 = SELECT+Right cycles boots and SELECT+Left cycles tunics without opening the menu" },
    /* ---------------------------------------------------------------- Enhancements */
    { "Enhancements", "text_speed", "", &port_opt_text_speed,
      "letters the text shows per frame: 1 = original, 2 = twice as fast (like the Redux hack), up to 5. Slow parts the text asks for stay slow" },
    { "Enhancements", "music_volume", "", &port_opt_music_volume,
      "music volume in % of the original (music, fanfares and ambience); 100 = original. Above 100 can distort loud parts" },
    { "Enhancements", "sound_volume", "", &port_opt_sound_volume,
      "sound effects volume in % of the original (effects, voices, ocarina); 100 = original" },
    { "Enhancements", "autosave", "", &port_opt_autosave,
      "N = the game saves itself (like Save in the pause menu) every N minutes of play (10 = default) and when you leave it (MENU, power-off); a short notice shows. 0 = off" },
    { "Enhancements", "hold_b_fast_text", "segurar_b", &port_opt_segurar_b,
      "1 = holding B (half a second) fast-forwards text. A tap on A or B shows the whole line; another tap goes to the next one" },
    { "Enhancements", "skip_cutscenes", "pular_cenas", &port_opt_pular_cenas, "1 = START skips cutscenes" },
    { "Enhancements", "fast_chests", "bau_rapido", &port_opt_bau_rapido,
      "1 = every chest opens quickly (no long big-chest animation)" },
    { "Enhancements", "tot_medallion_colors", "medalhoes_coloridos", &port_opt_tot_medallion_colors,
      "1 = the medallions on the Temple of Time floor light up in their colors as you collect them" },
    { "Enhancements", "better_bombchu_shopping", "bombchus_melhor", &port_opt_better_bombchu_shopping,
      "1 = bombchu packs in shops never sell out, and the 10-pack costs 99 rupees (affordable with the starting wallet)" },
    { "Enhancements", "quit_fishing_at_door", "parar_pesca_porta", &port_opt_quit_fishing_at_door,
      "1 = walking out of the fishing pond with the rod, the owner asks if you want to quit (Yes ends the session; No sends you back in)" },
    { "Enhancements", "fast_ocarina_playback", "ocarina_rapida", &port_opt_ocarina_rapida,
      "1 = after you play a song on the ocarina, the game does not play the melody back (except Sun's Song, Song of Storms and Song of Time)" },
    { "Enhancements", "enemy_health_bar", "barra_inimigo", &port_opt_barra_inimigo,
      "1 = health bar above the enemy targeted with Z (L1)" },
    { "Enhancements", "low_health_beep", "alarme_vida", &port_opt_alarme_vida,
      "1 = low health beep (like the original), 0 = no beep" },
    { "Enhancements", "disable_crit_wiggle", "sem_tremor", &port_opt_sem_tremor,
      "1 = the camera does not wiggle randomly at low health" },
    { "Enhancements", "camera_distance", "", &port_opt_camera_distance,
      "distance of the camera that follows Link, in % of the original: 70 = closer, 130 = farther (cutscenes and fixed room cameras stay as in the original). 100 = original" },
    /* Ship of Harkinian enhancements (Enhancements > Quality) */
    { "Enhancements", "remember_save_location", "", &port_opt_remember_save_location,
      "1 = when loading a save, Link starts at the last entrance he went through (not in grottos or fairy fountains; dungeons work as in the original); 0 = like the original (Link's house / Temple of Time)" },
    { "Enhancements", "gs_token_count", "", &port_opt_gs_token_count,
      "1 = the Gold Skulltula Token message shows how many you have collected (N/100)" },
    { "Enhancements", "heart_piece_count", "", &port_opt_heart_piece_count,
      "1 = the Piece of Heart message shows how many you have collected (N/36)" },
    { "Enhancements", "heart_container_count", "", &port_opt_heart_container_count,
      "1 = the Heart Container message shows how many you have collected (N/8)" },
    { "Enhancements", "better_owl", "", &port_opt_better_owl,
      "1 = the default answer to Kaepora Gaebora is always that you understood what he said" },
    { "Enhancements", "instant_putaway", "", &port_opt_instant_putaway,
      "1 = Link puts items away without having to wait around" },
    { "Enhancements", "sleeping_waterfall", "", &port_opt_sleeping_waterfall,
      "Zora's Domain waterfall: 0 = always play Zelda's Lullaby (original); 1 = play it once and the waterfall stays open; 2 = never needed (just learn the song and have an ocarina)" },
    /* (Enhancements > Skips & Speed-ups) */
    { "Enhancements", "faster_heavy_block_lift", "", &port_opt_faster_heavy_lift,
      "1 = lifting silver boulders and obelisks is much faster and skips their camera cutscene" },
    { "Enhancements", "skip_breath_animation", "", &port_opt_skip_breath_animation,
      "1 = skip Link's taking-breath animation after coming up from water (does not interfere with getting items underwater)" },
    { "Enhancements", "climb_speed", "", &port_opt_climb_speed,
      "vines and ladders: N = climb speed +N (0 = original)" },
    { "Enhancements", "block_push_speed", "", &port_opt_block_push_speed,
      "pushable blocks, statues and rotating walls: N = speed +N and less waiting between pushes (0 = original)" },
    { "Enhancements", "crawl_speed", "", &port_opt_crawl_speed,
      "crawlspaces: N = crawl N times faster (1 = original)" },
    { "Enhancements", "king_zora_speed", "", &port_opt_king_zora_speed,
      "N = King Zora moves out of the way N times faster (1 = original)" },
    { "Enhancements", "instant_scarecrow", "", &port_opt_instant_scarecrow,
      "1 = Pierre appears as soon as you take out the ocarina (you must have taught him the Scarecrow's Song first)" },
    { "Enhancements", "no_skulltula_freeze", "", &port_opt_no_skulltula_freeze,
      "1 = picking up a Gold Skulltula Token doesn't freeze Link (the message disappears by itself)" },
    { "Enhancements", "skip_save_confirmation", "", &port_opt_skip_save_confirmation,
      "1 = skip the 'Game Saved' confirmation (0.1 s instead of 3 s)" },
    { "Enhancements", "forge_time_days", "", &port_opt_forge_time_days,
      "N = days Biggoron takes to forge the Biggoron's Sword (3 = original)" },
    /* (Enhancements > Graphics) */
    { "Enhancements", "door_locks_both_sides", "", &port_opt_door_locks_both_sides,
      "1 = the chains of locked doors also show on the inside of the room" },
    { "Enhancements", "remember_minimap", "", &port_opt_remember_minimap,
      "1 = the minimap stays hidden (L) when you change areas" },
    { "Enhancements", "dynamic_wallet_icon", "", &port_opt_dynamic_wallet_icon,
      "1 = the rupee icon changes color with your wallet (green, blue, red)" },
    { "Enhancements", "visual_agony", "", &port_opt_visual_agony,
      "1 = with the Stone of Agony, an icon shows on the HUD near a secret and flashes (with a sound) where the controller would rumble" },
    { "Enhancements", "file_select_time_flow", "", &port_opt_file_select_time_flow,
      "1 = time passes (day/night sky) on the file select screen" },
    /* (Enhancements > Items) */
    { "Enhancements", "no_dropped_ocarina_inputs", "", &port_opt_no_dropped_ocarina_inputs,
      "1 = two ocarina notes pressed on consecutive frames are not dropped" },
    { "Enhancements", "persistent_masks", "", &port_opt_persistent_masks,
      "1 = a mask stays on when it leaves the buttons and comes back after changing areas (remembered while the game is open)" },
    { "Enhancements", "nuts_explode_bombs", "", &port_opt_nuts_explode_bombs, "1 = Deku Nuts make bombs explode" },
    { "Enhancements", "no_first_person_bombchus", "", &port_opt_no_first_person_bombchus,
      "1 = dropping a Bombchu doesn't force the first-person view" },
    { "Enhancements", "skip_arrow_animation", "", &port_opt_skip_arrow_animation,
      "1 = equipping magic arrows skips the animation of joining them to the bow" },
    { "Enhancements", "fast_boomerang", "", &port_opt_fast_boomerang,
      "1 = pressing the boomerang button while it's flying brings it back instantly" },
    { "Enhancements", "better_roll", "", &port_opt_better_roll,
      "1 = pressing A again near the end of a roll chains another roll (towards the D-pad direction), and the D-pad steers Link gently during a roll; 0 = like the original" },
    { "Enhancements", "better_farore", "", &port_opt_better_farore,
      "1 = Farore's Wind also works in the Gerudo Training Ground and inside Ganon's Castle, and child and adult Link each keep their own warp point (the other age's point is remembered while the game is open)" },
    { "Enhancements", "fast_farores", "", &port_opt_fast_farores, "1 = Farore's Wind casts and warps faster" },
    /* (Enhancements > Fixes, Difficulty, Minigames) */
    { "Enhancements", "faster_pause_menu", "", &port_opt_faster_pause_menu,
      "1 = pause menu animations (opening, closing, turning pages) twice as fast, like Majora's Mask (SoH Faster Pause Menu); 0 = like the original" },
    { "Enhancements", "faster_bottle_empty", "", &port_opt_faster_bottle_empty,
      "1 = emptying a bottle (fish, bugs, fairy, blue fire...) is 3 times faster (SoH Faster Bottle Emptying); 0 = like the original" },
    { "Enhancements", "faster_rupee_counter", "", &port_opt_faster_rupee_counter,
      "1 = the rupee counter goes up/down 10 at a time when you get or spend a lot (SoH Faster Rupee Accumulator); 0 = like the original" },
    { "Enhancements", "faster_shadow_ship", "", &port_opt_faster_shadow_ship,
      "1 = the Shadow Temple boat goes much faster while you are aboard and no Stalfos is near (SoH Faster Shadow Ship); 0 = like the original" },
    { "Enhancements", "rebottle_blue_fire", "", &port_opt_rebottle_blue_fire,
      "1 = blue fire you released from a bottle can be caught again (SoH Rebottle Blue Fire); 0 = like the original" },
    { "Enhancements", "no_heart_animation", "", &port_opt_no_heart_animation,
      "1 = the heart being filled on the life meter does not pulse (SoH No HUD Heart Animation); 0 = like the original" },
    { "Enhancements", "disable_title_card", "", &port_opt_disable_title_card,
      "1 = the area name does not show when you enter a place (SoH Disable Title Card); 0 = like the original" },
    { "Enhancements", "dampe_all_night", "", &port_opt_dampe_all_night,
      "1 = Dampe the gravekeeper is in the graveyard all night (not only 18:00-21:00) and his hut is open (SoH Dampe All Night); 0 = like the original" },
    { "Enhancements", "daytime_gold_skulltulas", "", &port_opt_daytime_gold_skulltulas,
      "1 = the Gold Skulltulas that only appear at night also appear during the day (SoH Night GS Always Spawn); 0 = like the original" },
    { "Enhancements", "shops_open_all_hours", "", &port_opt_shops_open_all_hours,
      "1 = shops and games that only open by day or by night (Bombchu shop, chest game, potion shop, mask shop, bazaars, Kakariko archery...) are always open (SoH Open All Hours); 0 = like the original" },
    { "Enhancements", "fast_drops", "", &port_opt_fast_drops,
      "1 = ammo and consumables dropped by bushes, rocks and enemies are picked up without the 'you got' scene, even the first time (SoH Fast Drops); 0 = like the original" },
    { "Enhancements", "skip_navi_forced_talk", "", &port_opt_skip_navi_forced_talk,
      "1 = Navi does not stop you to talk when you enter some places (SoH Skip Forced Dialog: Navi); 0 = like the original" },
    { "Enhancements", "skip_npc_forced_talk", "", &port_opt_skip_npc_forced_talk,
      "1 = characters that stop you to talk when you get close (hidden messages, the Gerudo with the card, Goron Link) do not (SoH Skip Forced Dialog: NPCs); 0 = like the original" },
    { "Enhancements", "skip_warp_cutscenes", "", &port_opt_skip_warp_cutscenes,
      "1 = warp songs take you straight to the destination, without the departure and arrival scenes (SoH Skip Warp Cutscenes); 0 = like the original" },
    { "Enhancements", "skip_tower_escape", "", &port_opt_skip_tower_escape,
      "1 = after Ganondorf, go straight to the final battle, without the escape from the collapsing tower (SoH Skip Tower Escape); 0 = like the original" },
    { "Enhancements", "skip_child_stealth", "", &port_opt_skip_child_stealth,
      "1 = the crawlspace into Hyrule Castle (child) goes straight to Zelda, skipping the guards (SoH Skip Child Stealth); 0 = like the original" },
    { "Enhancements", "skip_jabu_fish", "", &port_opt_skip_jabu_fish,
      "1 = Lord Jabu-Jabu opens his mouth when you get close, no fish needed (SoH Skip Feeding Jabu-Jabu); 0 = like the original" },
    { "Enhancements", "market_sneak", "", &port_opt_market_sneak,
      "1 = at night, the guard at the Market entrance asks child Link if he wants to go out for a walk; yes takes you to Hyrule Field (SoH Allow Exiting Market at Night); 0 = like the original" },
    { "Enhancements", "arrow_cycle", "", &port_opt_arrow_cycle,
      "1 = while aiming the bow (adult), R1 switches the arrow: normal, fire, ice, light (only the ones you have, with enough magic); magic is spent when the arrow is shot (SoH Arrow Cycle); 0 = like the original" },
    { "Enhancements", "bow_reticle", "", &port_opt_bow_reticle,
      "1 = aiming the bow or the slingshot shows a red dot where the shot will land, like the Hookshot (SoH Bow Reticle); 0 = like the original" },
    { "Enhancements", "hookshot_reticle_color", "", &port_opt_hookshot_reticle_color,
      "1 = the Hookshot dot turns green on surfaces it can grab and stays red on the others (SoH Targetable Hookshot Reticle); 0 = like the original (always red)" },
    { "Enhancements", "time_travel", "", &port_opt_time_travel,
      "1 = with the Ocarina of Time, playing the Song of Time away from the places where it already does something switches Link's age; he comes back in the same place (SoH Time Travel with the Song of Time); 0 = like the original" },
    { "Enhancements", "pause_warp", "", &port_opt_pause_warp,
      "1 = with an ocarina, A on a learned warp song in the Quest page plays it and asks where to warp (SoH Pause Warp); 0 = like the original" },
    { "Enhancements", "mask_select", "", &port_opt_mask_select,
      "1 = after paying for the masks of the Happy Mask Shop, A on the mask in the item menu switches to the next mask (SoH Mask Select); 0 = like the original" },
    { "Enhancements", "mm_bunny_hood", "", &port_opt_mm_bunny_hood,
      "1 = the Bunny Hood makes Link run 1.5x faster, and characters don't react to it (SoH MM Bunny Hood); 0 = like the original" },
    { "Enhancements", "ask_to_equip", "", &port_opt_ask_to_equip,
      "1 = when Link gets a sword, shield or tunic he can use, the game asks whether to equip it now (SoH Ask to Equip); 0 = like the original" },
    { "Enhancements", "minimal_ui", "", &port_opt_minimal_ui,
      "1 = with the minimap off (SELECT+L1), the buttons (with their items), rupees and keys are hidden; hearts only show when not full and magic only while it is used. Turning the minimap on shows everything. Takes effect on the next area (SoH Minimal UI); 0 = like the original" },
    { "Enhancements", "fix_gravedigging_tour", "", &port_opt_fix_gravedigging_tour,
      "1 = Dampe's grave-digging heart piece stays available until you actually pick it up (the original could lose it)" },
    { "Enhancements", "fix_floor_switches", "", &port_opt_fix_floor_switches,
      "1 = two floor switches that sat slightly above the floor are lowered to it" },
    { "Enhancements", "fix_zora_hint", "", &port_opt_fix_zora_hint,
      "1 = the Zora stops telling you to look for Ruto in the lake once you have her letter" },
    { "Enhancements", "fix_vine_fall", "", &port_opt_fix_vine_fall,
      "1 = Link no longer starts climbing a vine/ladder edge and immediately falls off" },
    { "Enhancements", "fix_enemy_spawns_water", "", &port_opt_fix_enemy_spawns_water,
      "1 = field enemies (Peahats, Leevers...) spawn correctly when Link is near water (the original check had the sign flipped)" },
    { "Enhancements", "fix_deku_nut_upgrade", "", &port_opt_fix_deku_nut_upgrade,
      "1 = getting the Poacher's Saw no longer makes the Deku Nut upgrade (Lost Woods stage) impossible; if it already happened, it becomes available again" },
    { "Enhancements", "fix_broken_giants_knife", "", &port_opt_fix_broken_giants_knife,
      "1 = the Broken Giant's Knife leaves the inventory when you get the Biggoron's Sword even if you lack the Kokiri or Master Sword" },
    { "Enhancements", "fix_camera_drift", "", &port_opt_fix_camera_drift,
      "1 = the camera no longer drifts slowly by itself (exact sine/cosine and no rounding creep)" },
    { "Enhancements", "fix_camera_swing", "", &port_opt_fix_camera_swing,
      "1 = the camera no longer swings around when it hits a wall behind Link" },
    { "Enhancements", "fix_camera_surface_hop", "", &port_opt_fix_camera_surface_hop,
      "1 = the camera no longer hops when Link surfaces after diving (the water wobble fades out instead of stopping at once)" },
    { "Enhancements", "fix_ledge_swing_rate", "", &port_opt_fix_ledge_swing_rate,
      "1 = the camera turns smoothly when Link hangs from a ledge" },
    { "Enhancements", "fix_menu_lr", "", &port_opt_fix_menu_lr,
      "1 = the L button in the pause menu has the same color as R" },
    { "Enhancements", "fix_dungeon_entrance_icons", "", &port_opt_fix_dungeon_entrance_icons,
      "1 = hide dungeon entrance icons that had no position (they showed in the minimap's corner)" },
    { "Enhancements", "fix_two_handed_idle", "", &port_opt_fix_two_handed_idle,
      "1 = with a two-handed sword Link doesn't do the 'adjust shield' idle pose" },
    { "Enhancements", "fix_gerudo_warrior_clothes", "", &port_opt_fix_gerudo_warrior_clothes,
      "1 = the Gerudo warrior's clothes don't change color (fixed in PAL 1.1)" },
    { "Enhancements", "fix_eyes_open_sleeping", "", &port_opt_fix_eyes_open_sleeping,
      "1 = Link's eyes are closed while sleeping in the opening cutscenes" },
    { "Enhancements", "fix_silver_rupee_jingle", "", &port_opt_fix_silver_rupee_jingle,
      "1 = the silver rupee jingle keeps rising after the 5th rupee" },
    { "Enhancements", "fix_darunia_dance", "", &port_opt_fix_darunia_dance,
      "1 = Darunia dances to the rhythm of Saria's Song (the original animation was too fast)" },
    { "Enhancements", "fix_vanishing_paths", "", &gx_fix_vanishing_paths,
      "1 = dirt roads and other markings painted on the ground (Hyrule Field, Kokiri Forest...) no longer vanish and come back as the camera moves; 0 = like the console at 320x240 (they vanish more at higher resolutions)" },
    { "Enhancements", "red_ganon_blood", "", &port_opt_red_ganon_blood,
      "1 = red blood on Ganondorf and Ganon (as in the earliest N64 versions; later changed to green)" },
    { "Enhancements", "pulsate_boss_icon", "", &port_opt_pulsate_boss_icon,
      "1 = the boss icon also pulses on the pause map inside boss rooms" },
    { "Enhancements", "frogs_note_timer", "", &port_opt_frogs_note_timer,
      "frogs' ocarina game: N = N times more time for each note (1 = original)" },
    { "Enhancements", "lost_woods_customize", "", &port_opt_lost_woods_customize,
      "1 = use the settings below for the Skull Kids' ocarina memory game in the Lost Woods; 0 = original (3 starting notes; rounds of 5, 6 and 8)" },
    { "Enhancements", "lost_woods_starting_notes", "", &port_opt_lost_woods_starting_notes,
      "Lost Woods ocarina game: notes played at the start of each round (1-8)" },
    { "Enhancements", "lost_woods_round1_notes", "", &port_opt_lost_woods_round1_notes,
      "Lost Woods ocarina game: notes in round 1 (1-8)" },
    { "Enhancements", "lost_woods_round2_notes", "", &port_opt_lost_woods_round2_notes,
      "Lost Woods ocarina game: notes in round 2 (1-8)" },
    { "Enhancements", "lost_woods_round3_notes", "", &port_opt_lost_woods_round3_notes,
      "Lost Woods ocarina game: notes in round 3 (1-8)" },
    { "Enhancements", "fishing_guarantee_bite", "", &port_opt_fishing_guarantee_bite,
      "1 = fish always bite the lure" },
    { "Enhancements", "fishing_fish_never_escape", "", &port_opt_fishing_fish_never_escape,
      "1 = a hooked fish never escapes" },
    { "Enhancements", "fishing_min_weight_child", "", &port_opt_fishing_min_weight_child,
      "N = minimum fish weight (lbs) for the child prize (Piece of Heart); 0 = original" },
    { "Enhancements", "fishing_min_weight_adult", "", &port_opt_fishing_min_weight_adult,
      "N = minimum fish weight (lbs) for the adult prize (Golden Scale); 0 = original" },
    /* ---------------------------------------------------------------- Cheats (SoH; all off by default) */
    { "Cheats", "cheat_infinite_money", "", &port_opt_cheat_infinite_money, "1 = wallet always full" },
    { "Cheats", "cheat_infinite_health", "", &port_opt_cheat_infinite_health, "1 = health always full" },
    { "Cheats", "cheat_infinite_ammo", "", &port_opt_cheat_infinite_ammo, "1 = sticks, nuts, bombs, arrows, seeds and bombchus always full" },
    { "Cheats", "cheat_infinite_magic", "", &port_opt_cheat_infinite_magic, "1 = magic always full" },
    { "Cheats", "cheat_infinite_nayrus_love", "", &port_opt_cheat_infinite_nayrus_love, "1 = Nayru's Love never runs out" },
    { "Cheats", "cheat_freeze_time", "", &port_opt_cheat_freeze_time, "1 = the time of day stops" },
    { "Cheats", "cheat_no_item_restrictions", "", &port_opt_cheat_no_item_restrictions, "1 = every item can be used everywhere (except the Sun's Song)" },
    { "Cheats", "cheat_disable_sandstorm", "", &port_opt_cheat_disable_sandstorm, "1 = no sandstorm in the Haunted Wasteland" },
    { "Cheats", "cheat_moon_jump_on_l", "", &port_opt_cheat_moon_jump_on_l, "1 = holding SELECT+A makes Link float upwards (moon jump)" },
    { "Cheats", "cheat_bomb_timer_percent", "", &port_opt_cheat_bomb_timer_percent, "bomb fuse time in % (100 = original, 200 = twice as long)" },
    { "Cheats", "cheat_hookshot_reach_percent", "", &port_opt_cheat_hookshot_reach_percent, "hookshot/longshot reach in % (100 = original)" },
    { "Cheats", "cheat_no_redead_freeze", "", &port_opt_cheat_no_redead_freeze, "1 = ReDeads and Gibdos don't paralyze Link with their scream" },
    { "Cheats", "cheat_fireproof_deku_shield", "", &port_opt_cheat_fireproof_deku_shield, "1 = the Deku Shield doesn't burn" },
    { "Cheats", "cheat_infinite_epona_boost", "", &port_opt_cheat_infinite_epona_boost, "1 = Epona's carrots never run out" },
    { "Cheats", "cheat_deku_stick", "", &port_opt_cheat_deku_stick, "1 = Deku Sticks don't break; 2 = don't break and are always on fire" },
    { "Cheats", "cheat_hookshot_everything", "", &port_opt_cheat_hookshot_everything, "1 = the hookshot/longshot attaches to any surface" },
    { "Cheats", "cheat_super_tunic", "", &port_opt_cheat_super_tunic, "1 = any tunic protects from heat and water (like Goron and Zora Tunics)" },
    { "Cheats", "cheat_shield_two_handed", "", &port_opt_cheat_shield_two_handed, "1 = the shield can be used while holding two-handed weapons (Biggoron Sword, Megaton Hammer)" },
    { "Cheats", "cheat_drops_dont_despawn", "", &port_opt_cheat_drops_dont_despawn, "1 = dropped items (rupees, hearts, ammo) never disappear" },
    { "Cheats", "cheat_no_fish_despawn", "", &port_opt_cheat_no_fish_despawn, "1 = fish released from bottles never disappear" },
    { "Cheats", "cheat_no_bugs_despawn", "", &port_opt_cheat_no_bugs_despawn, "1 = bugs released from bottles never disappear" },
    { "Cheats", "cheat_gs_targetable", "", &port_opt_cheat_gs_targetable, "1 = Gold Skulltulas can be targeted with Z" },
    { "Cheats", "cheat_no_keese_guay_target", "", &port_opt_cheat_no_keese_guay_target, "1 = Keese and Guay don't attack Link (they fly away)" },
    { "Cheats", "cheat_dampe_always_win", "", &port_opt_cheat_dampe_always_win,
      "1 = every dig in Dampe's game gives the grand prize (the heart piece first, then purple rupees)" },
    /* ---------------------------------------------------------------- Graphics */
    { "Graphics", "max_resolution", "resolucao", &gx_hr_max_opt,
      "maximum width of the 3D image: 640 (640x480) down to 320 (320x240, like the N64), in steps of 32. Above 320 the resolution is dynamic: it steps down in heavy scenes and back up in light ones. With retro_filter on it stays at 320" },
    { "Graphics", "retro_filter", "", &gx_retro,
      "retro filter (CRT TV look): 1 = scanlines, 2 = scanlines and an RGB mask; 0 = off. The game is drawn at 320x240 like the N64 (max_resolution is not used) and every game line takes 2 screen lines, so the scanlines match the game's lines. Turning it on in the menu raises the brightness by 3 (scanlines) or 4 (scanlines and mask); turning it off brings back the brightness from before" },
    { "Graphics", "retro_filter_strength", "", &gx_retro_forca,
      "retro filter: how much the scanlines (and the mask) darken, in % (25 to 75; 50 = default)" },
    { "Graphics", "retro_filter_tv", "", &gx_retro_tv,
      "retro filter: 1 = rounded corners and slightly darker edges, like a CRT TV screen; 0 = square picture" },
    { "Graphics", "retro_filter_brightness_base", "", &port_opt_retro_brightness_base,
      "(set by the game) brightness from before the retro filter was turned on in the menu, restored when it is turned off; -1 = none" },
    { "Graphics", "resolution_target_ms", "resolucao_alvo", &gx_hr_alvo,
      "dynamic resolution: drawing time (ms) it tries not to exceed. Lower = steps down sooner (smoother, less sharp); 30 = default" },
    { "Graphics", "brightness", "", &gx_brilho,
      "image brightness (gamma), like the gamma of Quake/Hexen II: dark areas get brighter; 0 = original, up to 10. No performance cost. In game, SELECT+Up / SELECT+Down raise / lower it and save it here" },
    { "Graphics", "screen_contrast", "", &port_opt_screen_contrast,
      "screen contrast, like Tweaks > Display in OnionOS (0 to 20); -1 = the system's. Only while the game runs; no performance cost" },
    { "Graphics", "screen_hue", "", &port_opt_screen_hue,
      "screen hue, like Tweaks > Display in OnionOS (0 to 20); -1 = the system's. Only while the game runs; no performance cost" },
    { "Graphics", "screen_saturation", "", &port_opt_screen_saturation,
      "screen color saturation, like Tweaks > Display in OnionOS (0 to 20); -1 = the system's. Only while the game runs; no performance cost" },
    { "Graphics", "texture_filter", "filtro_rapido", &gx_tex_prefilter,
      "pre-computed texture filter: 1 = light (blending between texels in steps of 1/2 instead of 1/32; see exact_filter_close for close-ups), 2 = 4x4 (steps of 1/4; 4x more memory), 0 = exact (slower)" },
    { "Graphics", "exact_filter_close", "filtro_exato_perto", &gx_filtro_exato_perto,
      "with the pre-computed filter: N = triangles where one texel covers N pixels or more (close to the camera, where it would look blocky) use a finer filter (see close_filter_mode); 0 = never" },
    { "Graphics", "close_filter_mode", "filtro_perto_fino", &gx_filtro_perto_fino,
      "filter for close-up triangles (exact_filter_close): 0 = exact N64 filter (smooth, a bit slower); 1 = 4x4 table (faster, slightly blocky)" },
    { "Graphics", "dither", "pontilhado", &gx_pontilhado,
      "color dithering when writing the 16-bit image (removes color banding in gradients: sky, fog, dark scenes): 1 = uniform pattern (Bayer) on opaque surfaces only (no dither on smoke, dust and glows); 2 = the pattern the game asks for, everywhere (like the console); 0 = none (visible banding)" },
    { "Graphics", "pause_menu_320", "menu_320", &gx_menu_320,
      "1 = the pause menu is drawn at 320x240 (original resolution; much lighter, the game resolution does not drop with the menu open); 0 = at the game resolution" },
    { "Graphics", "file_select_320", "arquivos_320", &gx_arquivos_320,
      "1 = the file select screen (blue menu) is drawn at 320x240, without the resolution changing steps; 0 = at the game resolution" },
    { "Graphics", "sharp_text_boxes", "texto_nitido", &gx_hr_texto,
      "1 = dialog boxes the 2D accelerator can't take are drawn apart at 640x480 (sharper, but the CPU cores have to sync twice per frame: the dynamic resolution drops); 0 = drawn with the 3D image (default, cheaper)" },
    { "Graphics", "opaque_text_boxes", "caixas_opacas", &port_opt_caixas_opacas,
      "1 = dialog boxes and minimap are opaque (no transparency; lighter to draw); 0 = like the original" },
    { "Graphics", "opaque_pause_menu", "menu_opaco", &gx_menu_opaco,
      "1 = the pause menu pages are opaque: the game is not visible through them; 0 = like the original (default; the menu is drawn at 320x240, so the transparency is cheap)" },
    { "Graphics", "opaque_title_logo", "logo_opaca", &gx_logo_opaca,
      "1 = the title logo without transparency (cut-out shield and 100% opaque texts); 0 = like the original (default)" },
    { "Graphics", "light_halo_tolerance", "halo_folga", &port_opt_halo_folga,
      "tolerance of the test that decides whether a light halo (Navi, torches) shows: N = accept the halo up to N depth units behind what is on screen (600 = torch flames touching their holder don't vanish; 0 = the game's exact test)" },
    { "Graphics", "old_reflections", "reflexo_antigo", &port_texgen_old,
      "0 = reflections (shiny textures: the Nintendo N, metals) mapped like on the console; 1 = old mapping (half the texture)" },
    { "Graphics", "fast_magnified_textures", "", &gx_tex_meia,
      "(test) 1 = textures seen up close (stretched over several pixels) read on half of the pixels, the ones in between averaged; measured slower on the device, so 0 (default) = every pixel" },
    { "Graphics", "light_hit_marks", "", &port_opt_light_hit_marks,
      "1 = the flash when the sword hits an enemy or a liftable rock is drawn only where the image has something, and its last almost-empty frames are skipped (lighter in combat; looks the same); 0 = like the original" },
    { "Graphics", "half_res_water", "", &gx_agua_meia,
      "1 = water surfaces (like Lake Hylia) read their texture on every other pixel and each point is used for the pixel next to it too: lighter where the water fills the screen, the water may look a bit softer (default); 0 = every pixel, like the original" },
    { "Graphics", "half_res_blend", "", &gx_mistura_meia,
      "1 = surfaces that blend two textures (moving walls and floor of Zora's Domain, waterfalls) read them on every other pixel and each point is used for the pixel next to it too: lighter there, may look a bit softer (default); 0 = every pixel, like the original" },
    { "Graphics", "lens_flare", "", &port_opt_lens_flare,
      "1 = the sun's lens flare (the colored circles and the glare when looking at the sun) like the original; 0 = off (lighter: when the sun shows they cover a good part of the screen with transparency)" },
    { "Graphics", "hd_title_logo", "", &port_opt_hd_logo,
      "1 = the title screen logo uses the new high-resolution textures from texturas_hd.pak (except the copyright line), 0 = the original logo" },
    /* ---------------------------------------------------------------- Performance */
    { "Performance", "present_delay", "atraso_extra", &port_opt_folga,
      "N = each image is shown N vsyncs (16.7 ms each) later, giving room for drawing spikes (the image reacts that much later to the buttons); 1 = default; 0 = no extra room" },
    { "Performance", "resolution_controller", "controle_resolucao", &gx_hr_ctl2,
      "how the dynamic resolution decides: 2 = predicts the time of each step, separating the fixed part of drawing (vertices, commands) from the per-pixel cost, and steps up sooner when it fits; 1 = older controller" },
    { "Performance", "hud_accelerator", "hud_acelerador", &gx_ge_on,
      "1 = the HUD and the title logo are drawn by the 2D accelerator, always at 640x480 (sharper and less work for the CPU cores); 0 = by the CPU cores" },
    { "Performance", "hud_small_on_accelerator", "hud_pequenas", &gx_ge_pequenas,
      "1 = small HUD pieces (hearts, numbers, icons) also go to the 2D accelerator while they don't change; 0 = drawn by the CPU cores" },
    { "Performance", "hud_changing_on_accelerator", "hud_mudando", &gx_ge_mudando,
      "1 = HUD pieces that change (beating heart, counters, buttons lighting up) stay on the 2D accelerator, recomputed when they change, instead of jumping to the CPU cores and back (the HUD and title logo 'flicker'); 0 = go to the CPU cores while changing" },
    { "Performance", "lazy_triangle_setup", "", &gx_tri_lazy,
      "1 = the triangle setup (gradients, edges) goes to the drawing queue and is done by whichever core draws the triangle first (usually the helper, which used to wait), leaving the main drawing thread only the commands and vertices; 0 = done by the main drawing thread" },
    { "Performance", "bg_accelerator", "", &gx_bg_ge,
      "1 = the pre-rendered JPEG backgrounds (Market square, Temple of Time front, Castle Town entrance) are scaled onto the image by the 2D accelerator (smoother, almost no work for the CPU cores); 0 = copied by the CPU cores" },
    { "Performance", "hud_fade_on_accelerator", "hud_alfa", &gx_ge_alfa,
      "1 = the HUD (and other 2D on the accelerator) stays on the accelerator while fading in or out (only the transparency changes); 0 = goes back to the CPU cores" },
    { "Graphics", "text_box_320", "", &gx_texto_320_opt,
      "1 = dialog boxes (box and text) are built at 320x240 like the rest of the interface (costs CPU every time the text changes); 0 = built at 640x480 (default, cheaper: only the new letters are added)" },
    { "Performance", "text_box_accelerator", "texto_acelerador", &gx_ge_texto,
      "1 = the whole dialog box goes through the 2D accelerator (rebuilt only when the text changes; almost no work for the CPU cores); 0 = by the CPU cores (sharp_text_boxes)" },
    { "Performance", "title_logo_320", "logo_320", &gx_logo_320,
      "1 = the title logo and its texts stay on the 2D accelerator (320x240 smoothly scaled, over the 3D at the game resolution) also while fading and with PRESS START blinking; 0 = like the other 2D" },
    { "Performance", "direct_color", "cor_direta", &gx_hr_le,
      "1 = the high-resolution image is stored in a format the screen's 2D chip reads directly (no per-image conversion on the CPU); 0 = convert (use if colors look wrong)" },
    { "Performance", "direct_render", "desenho_direto", &gx_hr_direto,
      "1 = high-resolution drawing goes straight into the image sent to the screen (no ~600 KB copy after each draw); 0 = copy (use if a wrong or flickering image shows up)" },
    { "Performance", "depth_points", "z_pontos", &gx_hr_zpontos,
      "1 = the game reads only the depth points it uses (sun, light halos) from the high-resolution image instead of copying all depth every frame; 0 = full copy (use if halos or the sun glare behave differently)" },
    { "Performance", "always_downscale", "reducao_sempre", &gx_hr_reducao_sempre,
      "0 = the high-resolution image is copied to the N64 screen only when the game needs it (pause background, transitions) and depth only on real frames (lighter); 1 = copy everything after every draw (use if a wrong background shows on pause or in a transition)" },
    { "Performance", "deferred_real_frame", "extra_adiado", &port_opt_extra_adiado,
      "with frame_interpolation: 1 = the real game frame is drawn after the in-between one, in the wait for the flip (30 fps holds in heavier scenes), 0 = both before" },
    { "Performance", "sort_opaque", "ordenar_opacos", &gx_sort_opaque,
      "1 = opaque objects drawn nearest first (what is behind is not painted), 0 = in the game's order" },
    { "Performance", "sort_batch", "ordenar_lote", &gx_sort_chunk,
      "with sort_opaque = 1: sort in batches of N triangles (the 2nd core starts drawing earlier); 0 = all at once. 120 = default" },
    { "Performance", "fused_draw", "desenho_fundido", &gx_fused, "1 = newer span drawing (8 pixels at a time), 0 = the older one" },
    { "Performance", "sky_premix", "", &gx_ceu_mistura,
      "1 = textures that the game blends by a constant (sky, some grounds) are blended once and reused; 0 = blended on every pixel" },
    { "Performance", "premix_scrolling", "", &gx_mistura_desliza,
      "with sky_premix: 1 = two sliding textures blended by a constant (water of Lake Hylia and Zora's Domain) are also blended once per frame and the water reads one texture instead of two (lighter; one layer may look a tiny bit softer); 2 = also the larger ones (Lake Hylia floor; default); 0 = blended on every pixel" },
    { "Performance", "crop_transparent", "", &gx_corte,
      "1 = the empty (transparent) part of mostly-empty textures is not drawn; 0 = drawn like before" },
    { "Performance", "stream_draw", "desenho_junto", &gx_stream,
      "1 = the 2nd core draws while the frame is still being built, 0 = only at the end" },
    { "Performance", "steal_lines", "pegar_linhas", &gx_steal,
      "1 = when the main drawing thread finishes first, it takes half of the helper's remaining lines (instead of just waiting), 0 = just wait" },
    { "Performance", "pin_cores", "nucleos_fixos", &port_opt_nucleos_fixos,
      "1 = the drawing thread stays on core 0 and its helper on core 1 (they don't fight for the same core), 0 = free" },
    { "Performance", "realtime_priority", "prioridade_real", &os_rt,
      "1 = game logic, sound and screen threads run with real-time priority (they preempt drawing), 0 = normal priority" },
    { "Performance", "realtime_draw", "desenho_tempo_real", &os_rt_draw,
      "1 = drawing also real-time (below game, sound and screen; other system programs don't take the core mid-frame), 0 = normal priority" },
    /* ---------------------------------------------------------------- Diagnostics */
    { "Diagnostics", "reload_overlays", "", &port_opt_reload_overlays,
      "1 = each character's/object's data starts fresh when it appears, like on the N64 (some characters, e.g. the Zora of the diving game, otherwise vanish on later visits); 0 = data kept between visits (for comparing)" },
    { "Diagnostics", "perf_trace", "", &port_opt_perf_trace,
      "1 = detailed measurement of the CPU cores (what each thread does, where, and the time per function); SELECT+R1 saves perf_NN.bin (the last ~30 s), also saved when quitting; tools/oot-perf-report.py turns it into a page. Costs a little CPU: leave 0 when just playing" },
    { "Diagnostics", "frame_trace", "registro_quadros", &gx_trace,
      "1 = every draw and every image shown becomes a [q] line in log.txt (per-frame timings), 0 = off" },
    { "Diagnostics", "test_warp", "", &port_opt_test_warp,
      "testing: N = the game opens straight at entrance N (decimal), skipping the title screen, with a test save that has every item, kept in saves/oot_test.sav (your own saves are not touched; save states are off in this mode). -1 = off (normal game). Some entrances: 205 Hyrule Field, 219 Kakariko, 234 Zora's River, 258 Lake Hylia, 264 Zora's Domain, 270 Zora's Fountain, 16 Water Temple (the full list, in hexadecimal, is in tools/oot-tour-list.txt)" },
    { "Diagnostics", "test_warp_adult", "", &port_opt_test_warp_adult, "with test_warp: 1 = adult Link, 0 = child Link" },
    { "Diagnostics", "hidden_stats", "", &gx_hidden_stats,
      "1 = every 3 s an [oculto] line in log.txt: how many triangles end up fully hidden behind what was already drawn and how much drawing time they take (measuring costs a little CPU), 0 = off" },
    { "Diagnostics", "slow_capture_ms", "captura_lenta", &gx_slowcap_ms,
      "N = save (lento_NN.cap) the frame after one that took more than N ms to draw; 0 = off" },
    { "Diagnostics", "slow_capture_after", "captura_inicio", &gx_slowcap_after,
      "with slow_capture_ms: only save after the game has been open N seconds (skips the intro); 0 = from the start" },
};
#define NOPTS ((int)(sizeof k_opts / sizeof k_opts[0]))

static void write_default(const char* path) {
    FILE* f = fopen(path, "w");
    const char* sec = "";
    int i;
    if (f == NULL) {
        return;
    }
    fprintf(f, "# Zelda: Ocarina of Time (Miyoo Mini) options. Change a number and save; it applies the next time the game starts.\n"
               "# Lines starting with # are comments.\n");
    for (i = 0; i < NOPTS; i++) {
        if (strcmp(sec, k_opts[i].sec) != 0) {
            sec = k_opts[i].sec;
            fprintf(f, "\n# ==================== %s ====================\n", sec);
        }
        fprintf(f, "\n# %s\n%s = %d\n", k_opts[i].help, k_opts[i].name, *k_opts[i].var);
    }
    fclose(f);
}

/* le "nome = valor" de um arquivo; aceita o nome novo e (legacy = 1) o antigo, em portugues. 1 = arquivo existia */
static int read_file(const char* path, int legacy) {
    char line[512];
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char name[64];
        int v, i;
        char* p = strchr(line, '#');
        if (p != NULL) {
            *p = 0;
        }
        if (sscanf(line, " %63[a-z0-9_] = %d", name, &v) != 2) {
            continue;
        }
        for (i = 0; i < NOPTS; i++) {
            if (!strcmp(name, k_opts[i].name) || (legacy && !strcmp(name, k_opts[i].old))) {
                *k_opts[i].var = v;
            }
        }
        if (!strncmp(name, "button_", 7)) {
            g_sawButtons = 1;
        } else if (!strcmp(name, "manual_jump")) {
            g_oldJump = v;
        } else if (!strcmp(name, "jump_on_b")) {
            g_oldJumpB = v;
        } else if (!strcmp(name, "slow_walk")) {
            g_oldSlow = v;
        }
    }
    fclose(f);
    return 1;
}

static const char* g_optPath = "options.txt";

/* os valores de fabrica (os do codigo), guardados antes de ler o options.txt: "Restaurar padrao" do menu */
static int g_def[NOPTS];
static int g_defOk;

int port_option_default(const char* name, int* out) {
    int i;
    for (i = 0; g_defOk && i < NOPTS; i++) {
        if (!strcmp(name, k_opts[i].name)) {
            *out = g_def[i];
            return 1;
        }
    }
    return 0;
}

/* volta ao padrao as opcoes de diagnostico (as do menu, menu_miyoo.c volta uma a uma) */
void port_options_default_diagnostics(void) {
    int i;
    for (i = 0; g_defOk && i < NOPTS; i++) {
        if (!strcmp(k_opts[i].sec, "Diagnostics")) {
            *k_opts[i].var = g_def[i];
        }
    }
}

/* menu de opcoes no jogo (port/miyoo/menu_miyoo.c): a variavel e a descricao de uma opcao pelo nome */
int* port_option_ptr(const char* name) {
    int i;
    for (i = 0; i < NOPTS; i++) {
        if (!strcmp(name, k_opts[i].name)) {
            return k_opts[i].var;
        }
    }
    return NULL;
}

const char* port_option_help(const char* name) {
    int i;
    for (i = 0; i < NOPTS; i++) {
        if (!strcmp(name, k_opts[i].name)) {
            return k_opts[i].help;
        }
    }
    return "";
}

/* grava o options.txt com os valores atuais (ex.: brilho trocado no jogo com SELECT+cima) */
void port_options_save(void) {
    write_default(g_optPath);
}

/* Os padroes de fabrica das opcoes do menu (General, Controls, Enhancements, Cheats, Graphics): as escolhas do
 * Regis (options.txt dele em 01/11/2026; brilho 1, autosave 10, idioma ingles). Ficam aqui, num lugar so, e valem
 * por cima dos valores iniciais espalhados pelo codigo: o "Restaurar padrao" do menu volta sempre para eles, mesmo
 * que algum arquivo do port esteja com um valor inicial antigo. */
static const struct {
    const char* name;
    int value;
} k_padrao[] = {
    { "language", 0 },
    { "frame_interpolation", 1 },
    { "frame_rate_steps", 1 },
    { "cpu_clock", 1800 },
    { "clock_guard", 1 },
    { "menu_theme", 1 },
    { "dpad_smooth_turn", 70 },
    { "rumble", 1 },
    { "natural_aiming", 1 },
    { "smooth_aiming", 1 },
    { "button_hold_ms", 250 },
    { "button_a_tap", 1 },
    { "button_a_hold", 1 },
    { "button_b_tap", 3 },
    { "button_b_hold", 3 },
    { "button_x_tap", 9 },
    { "button_x_hold", 9 },
    { "button_y_tap", 2 },
    { "button_y_hold", 2 },
    { "button_l1_tap", 4 },
    { "button_l1_hold", 4 },
    { "button_r1_tap", 5 },
    { "button_r1_hold", 5 },
    { "button_l2_tap", 10 },
    { "button_l2_hold", 10 },
    { "button_r2_tap", 11 },
    { "button_r2_hold", 12 },
    { "button_start_tap", 7 },
    { "button_start_hold", 7 },
    { "button_select_tap", 8 },
    { "button_select_hold", 0 },
    { "auto_jump", 1 },
    { "jump_strength", 70 },
    { "ledge_grab", 1 },
    { "air_steering", 1 },
    { "sprint_speed", 150 },
    { "quick_gear_swap", 0 },
    { "text_speed", 3 },
    { "music_volume", 100 },
    { "sound_volume", 100 },
    { "autosave", 10 },
    { "hold_b_fast_text", 1 },
    { "skip_cutscenes", 1 },
    { "fast_chests", 0 },
    { "tot_medallion_colors", 0 },
    { "better_bombchu_shopping", 0 },
    { "quit_fishing_at_door", 0 },
    { "fast_ocarina_playback", 0 },
    { "enemy_health_bar", 0 },
    { "low_health_beep", 1 },
    { "disable_crit_wiggle", 0 },
    { "camera_distance", 100 },
    { "remember_save_location", 0 },
    { "gs_token_count", 0 },
    { "heart_piece_count", 0 },
    { "heart_container_count", 0 },
    { "better_owl", 0 },
    { "instant_putaway", 0 },
    { "sleeping_waterfall", 0 },
    { "faster_heavy_block_lift", 0 },
    { "skip_breath_animation", 0 },
    { "climb_speed", 0 },
    { "block_push_speed", 0 },
    { "crawl_speed", 1 },
    { "king_zora_speed", 1 },
    { "instant_scarecrow", 0 },
    { "no_skulltula_freeze", 0 },
    { "skip_save_confirmation", 0 },
    { "forge_time_days", 3 },
    { "door_locks_both_sides", 0 },
    { "remember_minimap", 0 },
    { "dynamic_wallet_icon", 0 },
    { "visual_agony", 0 },
    { "file_select_time_flow", 0 },
    { "no_dropped_ocarina_inputs", 0 },
    { "persistent_masks", 0 },
    { "nuts_explode_bombs", 0 },
    { "no_first_person_bombchus", 0 },
    { "skip_arrow_animation", 0 },
    { "fast_boomerang", 0 },
    { "better_roll", 0 },
    { "better_farore", 0 },
    { "fast_farores", 0 },
    { "faster_pause_menu", 0 },
    { "faster_bottle_empty", 0 },
    { "faster_rupee_counter", 0 },
    { "faster_shadow_ship", 0 },
    { "rebottle_blue_fire", 0 },
    { "no_heart_animation", 0 },
    { "disable_title_card", 0 },
    { "dampe_all_night", 0 },
    { "daytime_gold_skulltulas", 0 },
    { "shops_open_all_hours", 0 },
    { "fast_drops", 0 },
    { "skip_navi_forced_talk", 0 },
    { "skip_npc_forced_talk", 0 },
    { "skip_warp_cutscenes", 0 },
    { "skip_tower_escape", 0 },
    { "skip_child_stealth", 0 },
    { "skip_jabu_fish", 0 },
    { "market_sneak", 0 },
    { "arrow_cycle", 0 },
    { "bow_reticle", 0 },
    { "hookshot_reticle_color", 0 },
    { "time_travel", 0 },
    { "pause_warp", 0 },
    { "mask_select", 0 },
    { "mm_bunny_hood", 0 },
    { "ask_to_equip", 0 },
    { "minimal_ui", 0 },
    { "fix_gravedigging_tour", 1 },
    { "fix_floor_switches", 1 },
    { "fix_zora_hint", 1 },
    { "fix_vine_fall", 1 },
    { "fix_enemy_spawns_water", 1 },
    { "fix_deku_nut_upgrade", 1 },
    { "fix_broken_giants_knife", 1 },
    { "fix_camera_drift", 1 },
    { "fix_camera_swing", 1 },
    { "fix_camera_surface_hop", 1 },
    { "fix_ledge_swing_rate", 1 },
    { "fix_menu_lr", 1 },
    { "fix_dungeon_entrance_icons", 1 },
    { "fix_two_handed_idle", 1 },
    { "fix_gerudo_warrior_clothes", 1 },
    { "fix_eyes_open_sleeping", 1 },
    { "fix_silver_rupee_jingle", 1 },
    { "fix_darunia_dance", 1 },
    { "fix_vanishing_paths", 1 },
    { "red_ganon_blood", 0 },
    { "pulsate_boss_icon", 0 },
    { "frogs_note_timer", 1 },
    { "lost_woods_customize", 0 },
    { "lost_woods_starting_notes", 5 },
    { "lost_woods_round1_notes", 5 },
    { "lost_woods_round2_notes", 6 },
    { "lost_woods_round3_notes", 8 },
    { "fishing_guarantee_bite", 0 },
    { "fishing_fish_never_escape", 0 },
    { "fishing_min_weight_child", 0 },
    { "fishing_min_weight_adult", 0 },
    { "cheat_infinite_money", 0 },
    { "cheat_infinite_health", 0 },
    { "cheat_infinite_ammo", 0 },
    { "cheat_infinite_magic", 0 },
    { "cheat_infinite_nayrus_love", 0 },
    { "cheat_freeze_time", 0 },
    { "cheat_no_item_restrictions", 0 },
    { "cheat_disable_sandstorm", 0 },
    { "cheat_moon_jump_on_l", 0 },
    { "cheat_bomb_timer_percent", 100 },
    { "cheat_hookshot_reach_percent", 100 },
    { "cheat_no_redead_freeze", 0 },
    { "cheat_fireproof_deku_shield", 0 },
    { "cheat_infinite_epona_boost", 0 },
    { "cheat_deku_stick", 0 },
    { "cheat_hookshot_everything", 0 },
    { "cheat_super_tunic", 0 },
    { "cheat_shield_two_handed", 0 },
    { "cheat_drops_dont_despawn", 0 },
    { "cheat_no_fish_despawn", 0 },
    { "cheat_no_bugs_despawn", 0 },
    { "cheat_gs_targetable", 0 },
    { "cheat_no_keese_guay_target", 0 },
    { "cheat_dampe_always_win", 0 },
    { "max_resolution", 640 },
    { "retro_filter", 0 },
    { "retro_filter_strength", 50 },
    { "retro_filter_tv", 1 },
    { "retro_filter_brightness_base", -1 },
    { "screen_contrast", -1 },
    { "screen_hue", -1 },
    { "screen_saturation", -1 },
    { "resolution_target_ms", 30 },
    { "brightness", 0 },
    { "texture_filter", 1 },
    { "exact_filter_close", 6 },
    { "close_filter_mode", 0 },
    { "dither", 1 },
    { "pause_menu_320", 1 },
    { "file_select_320", 1 },
    { "sharp_text_boxes", 0 },
    { "opaque_text_boxes", 0 },
    { "opaque_pause_menu", 0 },
    { "opaque_title_logo", 0 },
    { "light_halo_tolerance", 100 },
    { "old_reflections", 0 },
    { "lens_flare", 1 },
    { "half_res_water", 1 },
    { "half_res_blend", 1 },
    { "light_hit_marks", 1 },
    { "fast_magnified_textures", 0 },
    { "hd_title_logo", 1 },
    { "text_box_320", 0 },
    /* (desempenho e diagnostico: tambem os do options.txt do Regis, 03/11) */
    { "present_delay", 1 },
    { "resolution_controller", 2 },
    { "hud_accelerator", 1 },
    { "hud_small_on_accelerator", 1 },
    { "hud_changing_on_accelerator", 1 },
    { "lazy_triangle_setup", 0 },
    { "bg_accelerator", 1 },
    { "hud_fade_on_accelerator", 1 },
    { "text_box_accelerator", 1 },
    { "title_logo_320", 1 },
    { "direct_color", 1 },
    { "direct_render", 1 },
    { "depth_points", 1 },
    { "always_downscale", 0 },
    { "deferred_real_frame", 1 },
    { "premix_scrolling", 2 },
    { "sort_opaque", 1 },
    { "sort_batch", 120 },
    { "fused_draw", 1 },
    { "sky_premix", 1 },
    { "crop_transparent", 1 },
    { "stream_draw", 1 },
    { "steal_lines", 1 },
    { "pin_cores", 1 },
    { "realtime_priority", 1 },
    { "realtime_draw", 0 },
    { "reload_overlays", 1 },
    { "perf_trace", 0 },
    { "frame_trace", 0 },
    { "hidden_stats", 0 },
    { "test_warp", -1 },
    { "test_warp_adult", 0 },
    { "slow_capture_ms", 0 },
    { "slow_capture_after", 0 },
};

static void apply_padrao(void) {
    int i, j;
    for (j = 0; j < (int)(sizeof k_padrao / sizeof k_padrao[0]); j++) {
        for (i = 0; i < NOPTS; i++) {
            if (!strcmp(k_opts[i].name, k_padrao[j].name)) {
                if (*k_opts[i].var != k_padrao[j].value) {
                    fprintf(stderr, "opcoes: padrao de %s: %d no codigo, %d na tabela\n", k_padrao[j].name,
                            *k_opts[i].var, k_padrao[j].value);
                }
                *k_opts[i].var = k_padrao[j].value;
                break;
            }
        }
        if (i == NOPTS) {
            fprintf(stderr, "opcoes: padrao de opcao desconhecida: %s\n", k_padrao[j].name);
        }
    }
}


/* clock padrao pelo aparelho: 1800 MHz so no Miyoo Mini Plus; 1600 nos outros (Mini, Mini v4, Mini Flip), que nao
 * aguentam mais. O OnionOS grava o modelo em /tmp/deviceModel (283 = Mini, 354 = Mini Plus; ele descobre pelo
 * /customer/app/axp_test, que so o Plus tem). O Mini Flip roda uma versao do Onion feita por outra pessoa e nao se sabe
 * o que ela grava ali; por isso tambem conta a tela: o Plus tem 640x480; o Mini v4 e o Flip, 750x560. */
static int default_clock(void) {
#ifdef TARGET_MIYOO
    int model = 0, w = 0, h = 0;
    FILE* f = fopen("/tmp/deviceModel", "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &model) != 1) {
            model = 0;
        }
        fclose(f);
    }
    if (model == 0) {
        FILE* t = fopen("/customer/app/axp_test", "rb");
        if (t != NULL) {
            model = 354;
            fclose(t);
        }
    }
    {
        int fd = open("/dev/fb0", O_RDONLY);
        if (fd >= 0) {
            struct fb_var_screeninfo vi;
            if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0) {
                w = (int)vi.xres;
                h = (int)vi.yres;
            }
            close(fd);
        }
    }
    fprintf(stderr, "[clock] aparelho: modelo %d, tela %dx%d -> padrao %d MHz\n", model, w, h,
            (model == 354 && ((w == 640 && h == 480) || (w == 480 && h == 640))) ? 1800 : 1600);
    return (model == 354 && ((w == 640 && h == 480) || (w == 480 && h == 640))) ? 1800 : 1600;
#else
    return 1800;
#endif
}

void port_options_load(const char* path) {
    int i;
    /* padrao do jogo (o programa de testes oot_replay continua com o filtro exato): filtro leve, aprovado */
    gx_tex_prefilter = 1;
    apply_padrao();
    port_opt_clock = default_clock(); /* (depende do aparelho; o options.txt, se tiver, vale por cima) */
    for (i = 0; i < NOPTS; i++) {
        g_def[i] = *k_opts[i].var;
    }
    g_defOk = 1;
    /* o arquivo antigo (opcoes.txt, nomes em portugues): os valores passam para o options.txt e ele sai */
    if (read_file("opcoes.txt", 1)) {
        remove("opcoes.txt");
        fprintf(stderr, "opcoes: opcoes.txt antigo migrado para %s\n", path);
    }
    g_optPath = path;
    read_file(path, 1);
    if (!g_sawButtons && (g_oldJump >= 0 || g_oldJumpB >= 0 || g_oldSlow >= 0)) {
        /* options.txt de antes das teclas personalizaveis: as teclas ficam como estavam (pulo manual ou nao, pulo no B
         * ou no X, R2 segurado andando devagar ou nao) */
        static const int k_noJump[20] = { 1, 1, 2, 2, 9, 9, 10, 10, 4, 4, 5, 5, 8, 8, 11, 12, 7, 7, 6, 0 };
        static const int k_jumpX[20] = { 1, 1, 2, 2, 3, 3, 10, 10, 4, 4, 5, 5, 9, 9, 11, 12, 7, 7, 8, 0 };
        if (g_oldJump == 0) {
            memcpy(port_opt_btn, k_noJump, sizeof k_noJump);
        } else if (g_oldJumpB == 0) {
            memcpy(port_opt_btn, k_jumpX, sizeof k_jumpX);
        }
        if (g_oldSlow == 0) {
            port_opt_btn[15] = 11; /* R2 segurado: C-direita, como o toque */
        }
        fprintf(stderr, "opcoes: teclas montadas das opcoes antigas (manual_jump %d, jump_on_b %d, slow_walk %d)\n",
                g_oldJump, g_oldJumpB, g_oldSlow);
    }
    write_default(path); /* reescreve com os valores lidos: opcoes novas aparecem no arquivo */
    {
        extern void gx_hr_sync_opts(void); /* gfx_hires.c: resolucao em uso (filtro retro) */
        gx_hr_sync_opts();
    }
    fprintf(stderr, "opcoes:");
    for (i = 0; i < NOPTS; i++) {
        fprintf(stderr, " %s=%d", k_opts[i].name, *k_opts[i].var);
    }
    fprintf(stderr, "\n");
}

/* diagnostico das melhorias (codigo do jogo ajustado por tools/oot-patch-soh.py): vai para o log do jogo */
void Port_Diag(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
