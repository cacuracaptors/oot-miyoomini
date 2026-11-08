# oot-patch-soh.py: melhorias do Ship of Harkinian (SoH) no codigo do jogo. Executado de dentro do
# oot-patch.py (usa patch_file / read / sub de la). Cada melhoria segue o que o SoH faz (o codigo dele ja foi
# testado por muita gente), com uma opcao port_opt_* (port/port_options.c, secao Enhancements do options.txt).
# As do SoH que usam "ganchos" (GameInteractor_Should(VB_..., padrao)) viram um teste da opcao no mesmo ponto.


# letras acentuadas da fonte do jogo (as mesmas da versao europeia: nes_font_static 0x80-0x9E)
_PALCH = "\u00c0\u00ce\u00c2\u00c4\u00c7\u00c8\u00c9\u00ca\u00cb\u00cf\u00d4\u00d6\u00d9\u00db\u00dc\u00df" \
         "\u00e0\u00e1\u00e2\u00e4\u00e7\u00e8\u00e9\u00ea\u00eb\u00ef\u00f4\u00f6\u00f9\u00fb\u00fc"


def _pal(text):
    """texto (alemao/frances) -> bytes do texto do jogo"""
    return [0x80 + _PALCH.index(c) if c in _PALCH else ord(c) for c in text]


# espanhol (idiomas/es.oot, traducao de eduardo_a2j pelo Patcher64+): a fonte do pacote desenha o "¡" no "<", o "ñ"
# no "\\" e o "¿" no 0x82; os acentos ficam onde a fonte do jogo os tem (Languages.json do Patcher64+)
_ESCH = {"\u00a1": 0x3C, "\u00f1": 0x5C, "\u00bf": 0x82, "\u00c1": 0x81, "\u00c9": 0x86, "\u00cd": 0x89,
         "\u00d3": 0x8A, "\u00da": 0x8D, "\u00dc": 0x8E, "\u00e1": 0x91, "\u00e9": 0x96, "\u00ed": 0x99,
         "\u00f3": 0x9A, "\u00fa": 0x9C, "\u00fc": 0x9E}


def _es(text):
    """texto espanhol -> bytes do texto do jogo (com a fonte do pacote espanhol)"""
    return [_ESCH[c] if c in _ESCH else ord(c) for c in text]


def _cbytes(b):
    return ", ".join("0x%02X" % x for x in b)


def O(name):
    """expressao C que le a opcao port_opt_<name> (sem precisar de cabecalho)"""
    return "({ extern int port_opt_%s; port_opt_%s; })" % (name, name)


# ---------------------------------------------------------------- Quality
# Remember Save Location: ao carregar o jogo, Link volta para a ultima entrada por onde passou (menos grutas e
# fontes das fadas, onde a entrada salva nao leva de volta)
patch_file("src/code/z_sram.c", [
    ("lembrar o lugar do save",
     "        default:\n"
     "            if (gSaveContext.save.info.playerData.savedSceneId != SCENE_LINKS_HOUSE) {\n",
     "        default:\n"
     "            if (" + O("remember_save_location") + " && /* port: SoH RememberSaveLocation */\n"
     "                gSaveContext.save.info.playerData.savedSceneId != SCENE_FAIRYS_FOUNTAIN &&\n"
     "                gSaveContext.save.info.playerData.savedSceneId != SCENE_GROTTOS) {\n"
     "                break;\n"
     "            }\n"
     "            if (gSaveContext.save.info.playerData.savedSceneId != SCENE_LINKS_HOUSE) {\n"),
])

# Contagem nas mensagens (SoH InjectItemCounts) e No Skulltula Freeze. O SoH troca a mensagem inteira (em ingles,
# alemao e frances); aqui a mensagem do jogo (ou da traducao) fica e ganha "N/100" no fim - vale para qualquer
# idioma. Pedacos de coracao: o jogo nao guarda o total; ele sai da vida maxima, dos pedacos no inventario e dos
# coracoes dos chefes ja pegos (bandeira 0x1F da sala de cada chefe).
PORT_MSG = r'''
/* port: melhorias do SoH nas mensagens (tools/oot-patch-soh.py) */
static s32 Port_MsgBossHearts(PlayState* play) {
    static const s16 bossScenes[] = { SCENE_DEKU_TREE_BOSS,     SCENE_DODONGOS_CAVERN_BOSS, SCENE_JABU_JABU_BOSS,
                                      SCENE_FOREST_TEMPLE_BOSS, SCENE_FIRE_TEMPLE_BOSS,     SCENE_WATER_TEMPLE_BOSS,
                                      SCENE_SPIRIT_TEMPLE_BOSS, SCENE_SHADOW_TEMPLE_BOSS };
    s32 i, n = 0;
    for (i = 0; i < ARRAY_COUNT(bossScenes); i++) {
        u32 collect = (play->sceneId == bossScenes[i]) ? play->actorCtx.flags.collect
                                                        : gSaveContext.save.info.sceneFlags[bossScenes[i]].collect;
        if (collect & (1u << 0x1F)) {
            n++;
        }
    }
    return n;
}

/* fim da ultima caixa da mensagem: bytes ate o MESSAGE_END e linhas na ultima caixa */
static s32 Port_MsgEnd(u8* buf, s32 len, s32* lines) {
    s32 i = 0;
    *lines = 1;
    while (i < len) {
        u8 c = buf[i];
        if (c == MESSAGE_END) {
            return i;
        }
        if (c == MESSAGE_NEWLINE) {
            (*lines)++;
        } else if (c == MESSAGE_BOX_BREAK || c == MESSAGE_BOX_BREAK_DELAYED) {
            *lines = 1;
        }
        switch (c) {
            case MESSAGE_COLOR: case MESSAGE_SHIFT: case MESSAGE_BOX_BREAK_DELAYED: case MESSAGE_FADE:
            case MESSAGE_ITEM_ICON: case MESSAGE_TEXT_SPEED: case MESSAGE_HIGHSCORE:
                i += 2;
                break;
            case MESSAGE_TEXTID: case MESSAGE_FADE2: case MESSAGE_SFX:
                i += 3;
                break;
            case MESSAGE_BACKGROUND:
                i += 4;
                break;
            default:
                i++;
                break;
        }
    }
    return -1;
}


/* port: SoH MarketSneak - fala nova do guarda da entrada do mercado a noite (0x7003) com "sim/nao" */
static s32 Port_MsgMarketSneak(PlayState* play, u16 textId, u8* buf, s32 cap) {
    static const u8 en[] = { 0x59, 0x6F, 0x75, 0x20, 0x6C, 0x6F, 0x6F, 0x6B, 0x20, 0x62, 0x6F, 0x72, 0x65, 0x64, 0x2E, 0x01, 0x57, 0x61, 0x6E, 0x6E, 0x61, 0x20, 0x67, 0x6F, 0x20, 0x6F, 0x75, 0x74, 0x20, 0x66, 0x6F, 0x72, 0x20, 0x61, 0x20, 0x77, 0x61, 0x6C, 0x6B, 0x3F, 0x01, 0x1B, 0x05, 0x42, 0x59, 0x65, 0x73, 0x01, 0x4E, 0x6F, 0x05, 0x40, 0x02 };
    static const u8 pt[] = { 0x45, 0x6E, 0x74, 0x65, 0x64, 0x69, 0x61, 0x64, 0x6F, 0x2C, 0x20, 0x67, 0x61, 0x72, 0x6F, 0x74, 0x6F, 0x3F, 0x01, 0x51, 0x75, 0x65, 0x72, 0x20, 0x64, 0x61, 0x72, 0x20, 0x75, 0x6D, 0x61, 0x20, 0x76, 0x6F, 0x6C, 0x74, 0x61, 0x3F, 0x01, 0x1B, 0x05, 0x42, 0x53, 0x69, 0x6D, 0x2C, 0x20, 0x76, 0x61, 0x6D, 0x6F, 0x73, 0x01, 0x46, 0x69, 0x63, 0x6F, 0x20, 0x70, 0x6F, 0x72, 0x20, 0x61, 0x71, 0x75, 0x69, 0x05, 0x40, 0x02 };
    static const u8 de[] = { 0x44, 0x75, 0x20, 0x73, 0x69, 0x65, 0x68, 0x73, 0x74, 0x20, 0x67, 0x65, 0x6C, 0x61, 0x6E, 0x67, 0x77, 0x65, 0x69, 0x6C, 0x74, 0x20, 0x61, 0x75, 0x73, 0x2E, 0x01, 0x4C, 0x75, 0x73, 0x74, 0x20, 0x61, 0x75, 0x66, 0x20, 0x65, 0x69, 0x6E, 0x65, 0x6E, 0x20, 0x53, 0x70, 0x61, 0x7A, 0x69, 0x65, 0x72, 0x67, 0x61, 0x6E, 0x67, 0x3F, 0x01, 0x1B, 0x05, 0x42, 0x4A, 0x61, 0x01, 0x4E, 0x65, 0x69, 0x6E, 0x05, 0x40, 0x02 };
    static const u8 fr[] = { 0x54, 0x75, 0x20, 0x61, 0x73, 0x20, 0x6C, 0x27, 0x61, 0x69, 0x72, 0x20, 0x64, 0x65, 0x20, 0x74, 0x27, 0x65, 0x6E, 0x6E, 0x75, 0x79, 0x65, 0x72, 0x2E, 0x01, 0x54, 0x75, 0x20, 0x76, 0x65, 0x75, 0x78, 0x20, 0x61, 0x6C, 0x6C, 0x65, 0x72, 0x20, 0x66, 0x61, 0x69, 0x72, 0x65, 0x20, 0x75, 0x6E, 0x20, 0x74, 0x6F, 0x75, 0x72, 0x3F, 0x01, 0x1B, 0x05, 0x42, 0x4F, 0x75, 0x69, 0x01, 0x4E, 0x6F, 0x6E, 0x05, 0x40, 0x02 };
    static const u8 it[] = { 0x53, 0x65, 0x6D, 0x62, 0x72, 0x69, 0x20, 0x61, 0x6E, 0x6E, 0x6F, 0x69, 0x61, 0x74, 0x6F, 0x2E, 0x01, 0x56, 0x75, 0x6F, 0x69, 0x20, 0x66, 0x61, 0x72, 0x65, 0x20, 0x75, 0x6E, 0x20, 0x67, 0x69, 0x72, 0x6F, 0x3F, 0x01, 0x1B, 0x05, 0x42, 0x53, 0x99, 0x01, 0x4E, 0x6F, 0x05, 0x40, 0x02 };
    static const u8 es[] = { @@ESMS@@ };
    static const u16 jp[] = { 0x82D0, 0x82DC, 0x82BB, 0x82A4, 0x82BE, 0x82C8, 0x8140, 0x82B3, 0x82F1, 0x82DB, 0x82C9, 0x8140, 0x82A2, 0x82AD, 0x82A9, 0x8148, 0x000A, 0x81BC, 0x000B, 0x0C02, 0x82A4, 0x82F1, 0x000A, 0x82E2, 0x82DF, 0x82C6, 0x82AD, 0x000B, 0x0C00, 0x8170 };
    MessageContext* msgCtx = &play->msgCtx;
    s32 i;

    if (textId != 0x7003 || !({ extern int port_opt_market_sneak; port_opt_market_sneak; }) ||
        play->sceneId != SCENE_MARKET_ENTRANCE_NIGHT || GET_PLAYER(play)->currentMask != PLAYER_MASK_NONE) {
        return 0;
    }
    if (gSaveContext.language == LANGUAGE_JPN) {
        if ((s32)sizeof(jp) > cap) {
            return 0;
        }
        for (i = 0; i < (s32)ARRAY_COUNT(jp); i++) {
            ((u16*)buf)[i] = jp[i]; /* (ja na ordem do processador, como o texto japones depois da troca) */
        }
        msgCtx->msgLength = sizeof(jp);
    } else {
        extern int port_opt_idioma;
        const u8* s = port_opt_idioma == 1 ? pt : port_opt_idioma == 3 ? de : port_opt_idioma == 4 ? fr
                    : port_opt_idioma == 5 ? it : port_opt_idioma == 6 ? es : en;
        s32 n = port_opt_idioma == 1 ? (s32)sizeof(pt) : port_opt_idioma == 3 ? (s32)sizeof(de)
              : port_opt_idioma == 4 ? (s32)sizeof(fr) : port_opt_idioma == 5 ? (s32)sizeof(it)
              : port_opt_idioma == 6 ? (s32)sizeof(es) : (s32)sizeof(en);
        if (n > cap) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            buf[i] = s[i];
        }
        msgCtx->msgLength = n;
    }
    play->msgCtx.font.msgLength = msgCtx->msgLength;
    return 1;
}

static void Port_MsgInject(PlayState* play, u16 textId, u8* buf, s32 cap) {
    MessageContext* msgCtx = &play->msgCtx;
    char add[72];
    s32 n = 0, end, lines, i, total = -1, count = 0, fade = 0;

    if (Port_MsgMarketSneak(play, textId, buf, cap)) {
        return;
    }
    if ((textId == 0xB4 || textId == 0xB5) && ({ extern int port_opt_gs_token_count; port_opt_gs_token_count; })) {
        count = gSaveContext.save.info.inventory.gsTokens;
        total = 100;
    } else if (textId >= 0xC2 && textId <= 0xC5 && ({ extern int port_opt_heart_piece_count; port_opt_heart_piece_count; })) {
        count = (gSaveContext.save.info.playerData.healthCapacity / 0x10 - 3 - Port_MsgBossHearts(play)) * 4 +
                ((gSaveContext.save.info.inventory.questItems & 0xF0000000) >> 28) + 1; /* (o jogo da o item depois de abrir a caixa) */
        total = 36;
    } else if (textId == 0xC6 && ({ extern int port_opt_heart_container_count; port_opt_heart_container_count; })) {
        count = Port_MsgBossHearts(play) + 1; /* (a bandeira do chefe e marcada depois) */
        total = 8;
    }
    if ((textId == 0xB4 || textId == 0xB5) && ({ extern int port_opt_no_skulltula_freeze; port_opt_no_skulltula_freeze; })) {
        fade = 1; /* sem o congelamento a caixa some sozinha (Link ja esta andando) */
    }
    if ((textId == 0x8C || textId == 0xBC) && ({ extern int port_opt_better_bombchu_shopping; port_opt_better_bombchu_shopping; })) {
        /* SoH BetterBombchuShopping: pacote de 10 bombchus por 99 (cabe na carteira inicial): "100" -> "99" */
        for (i = 0; i + 2 < msgCtx->msgLength; i++) {
            if (buf[i] == '1' && buf[i + 1] == '0' && buf[i + 2] == '0' && (i == 0 || buf[i - 1] < '0' || buf[i - 1] > '9') &&
                (i + 3 >= msgCtx->msgLength || buf[i + 3] < '0' || buf[i + 3] > '9')) {
                buf[i] = '9';
                buf[i + 1] = '9';
                for (end = i + 2; end + 1 < msgCtx->msgLength; end++) {
                    buf[end] = buf[end + 1];
                }
                msgCtx->msgLength -= 1;
                play->msgCtx.font.msgLength -= 1;
                break;
            }
        }
        return;
    }
    if (textId == 0x409E && ({ extern int port_opt_quit_fishing_at_door; port_opt_quit_fishing_at_door; })) {
        /* SoH QuitFishingAtDoor: "nao pode levar a vara" + pergunta se quer parar (Sim/Nao; 0x93 = a com til na fonte da
         * traducao) */
        static const char ptQ[] = "Quer parar de pescar?";
        static const char enQ[] = "Do you want to quit?";
        static const char deQ[] = "Willst Du aufh\x9Bren?";
        static const char frQ[] = "Tu veux arr\x97ter de p\x97" "cher?";
        static const char itQ[] = "Vuoi smettere di pescare?";
        static const char esQ[] = "\x82Quieres dejar de pescar?"; /* (0x82: o "¿" no pacote espanhol) */
        static const char* const yes[7] = { "Yes", "Sim", "Yes", "Ja", "Oui", "S\x99", "S\x99" }; /* (0x99: i com acento grave no pacote italiano; i com agudo no espanhol) */
        static const char* const no[7] = { "No", "N\x93o", "No", "Nein", "Non", "No", "No" };
        const int lg = ({ extern int port_opt_idioma; port_opt_idioma; }) >= 0 &&
                               ({ extern int port_opt_idioma; port_opt_idioma; }) <= 6
                           ? ({ extern int port_opt_idioma; port_opt_idioma; })
                           : 0;
        const char* q = lg == 1 ? ptQ : lg == 3 ? deQ : lg == 4 ? frQ : lg == 5 ? itQ : lg == 6 ? esQ : enQ;
        end = Port_MsgEnd(buf, msgCtx->msgLength, &lines);
        if (end < 0) {
            return;
        }
        add[n++] = MESSAGE_BOX_BREAK;
        for (i = 0; q[i] != 0 && n < 40; i++) {
            add[n++] = q[i];
        }
        add[n++] = MESSAGE_NEWLINE;
        add[n++] = MESSAGE_TWO_CHOICE;
        add[n++] = MESSAGE_COLOR;
        add[n++] = 0x42; /* verde */
        for (i = 0; yes[lg][i] != 0; i++) {
            add[n++] = yes[lg][i];
        }
        add[n++] = MESSAGE_NEWLINE;
        for (i = 0; no[lg][i] != 0; i++) {
            add[n++] = no[lg][i];
        }
        add[n++] = MESSAGE_COLOR;
        add[n++] = 0x40;
        if (end + n + 1 > cap) {
            return;
        }
        for (i = msgCtx->msgLength - 1; i >= end; i--) {
            if (i + n < cap) {
                buf[i + n] = buf[i];
            }
        }
        for (i = 0; i < n; i++) {
            buf[end + i] = add[i];
        }
        msgCtx->msgLength += n;
        play->msgCtx.font.msgLength += n;
        return;
    }
    if (total < 0 && !fade) {
        return;
    }
    end = Port_MsgEnd(buf, msgCtx->msgLength, &lines);
    if (end < 0) {
        return;
    }
    if (total >= 0) {
        if (count < 0) {
            count = 0;
        }
        /* na mesma caixa: linha nova se couber (4 linhas), senao no fim da ultima */
        if (lines < 4) {
            add[n++] = MESSAGE_NEWLINE;
        } else {
            add[n++] = ' ';
        }
        add[n++] = MESSAGE_COLOR;
        add[n++] = 0x41; /* vermelho */
        if (count >= 100) {
            add[n++] = '0' + count / 100;
        }
        if (count >= 10) {
            add[n++] = '0' + (count / 10) % 10;
        }
        add[n++] = '0' + count % 10;
        add[n++] = MESSAGE_COLOR;
        add[n++] = 0x40;
        add[n++] = '/';
        if (total >= 100) {
            add[n++] = '0' + total / 100;
        }
        if (total >= 10) {
            add[n++] = '0' + (total / 10) % 10;
        }
        add[n++] = '0' + total % 10;
    }
    if (fade) {
        add[n++] = MESSAGE_FADE;
        add[n++] = 0x3C; /* 60 quadros (como o SoH) */
    }
    if (end + n + 1 > cap) {
        return;
    }
    for (i = msgCtx->msgLength - 1; i >= end; i--) {
        if (i + n < cap) {
            buf[i + n] = buf[i];
        }
    }
    for (i = 0; i < n; i++) {
        buf[end + i] = add[i];
    }
    msgCtx->msgLength += n;
    play->msgCtx.font.msgLength += n;
}

'''
PORT_MSG = PORT_MSG.replace("@@ESMS@@", _cbytes(_es("Pareces aburrido.") + [0x01] + _es("\u00bfQuieres dar un paseo?") +
                                                [0x01, 0x1B, 0x05, 0x42] + _es("S\u00ed") + [0x01] + _es("No") +
                                                [0x05, 0x40, 0x02]))
patch_file("src/code/z_message.c", [
    ("SoH: funcoes das mensagens",
     "void Message_OpenText(PlayState* play, u16 textId) {\n",
     PORT_MSG + "void Message_OpenText(PlayState* play, u16 textId) {\n"),
    ("SoH: contagem e caixa que some sozinha",
     "                             font->msgLength, \"../z_message_PAL.c\", UNK_LINE);\n#endif\n        }\n#else\n"
     "        if (gSaveContext.language == LANGUAGE_ENG) {\n",
     "                             font->msgLength, \"../z_message_PAL.c\", UNK_LINE);\n#endif\n        }\n"
     "        Port_MsgInject(play, textId, MSG_BUF, sizeof(font->msgBuf)); /* port: SoH */\n#else\n"
     "        if (gSaveContext.language == LANGUAGE_ENG) {\n"),
])

# No Skulltula Freeze: pegar o simbolo da Skulltula Dourada nao congela o Link
patch_file("src/overlays/actors/ovl_En_Si/z_en_si.c", [
    ("SoH: sem congelar (pegando)",
     "                Item_Give(play, ITEM_SKULL_TOKEN);\n                player->actor.freezeTimer = 10;\n",
     "                Item_Give(play, ITEM_SKULL_TOKEN);\n"
     "                if (!" + O("no_skulltula_freeze") + ") { /* port: SoH SkulltulaFreeze */\n"
     "                    player->actor.freezeTimer = 10;\n                }\n"),
    ("SoH: sem congelar (gancho)",
     "        Item_Give(play, ITEM_SKULL_TOKEN);\n        player->actor.freezeTimer = 10;\n",
     "        Item_Give(play, ITEM_SKULL_TOKEN);\n"
     "        if (!" + O("no_skulltula_freeze") + ") { /* port: SoH SkulltulaFreeze */\n"
     "            player->actor.freezeTimer = 10;\n        }\n"),
    ("SoH: sem congelar (esperando a caixa)",
     "    if (Message_GetState(&play->msgCtx) != TEXT_STATE_CLOSING) {\n        player->actor.freezeTimer = 10;\n",
     "    if (Message_GetState(&play->msgCtx) != TEXT_STATE_CLOSING && !" + O("no_skulltula_freeze") + ") { /* port */\n"
     "        player->actor.freezeTimer = 10;\n"),
])

# Instant Putaway / Skip Water Take Breath Animation / Vine-Ladder Climb Speed
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: guardar o item na hora",
     "        } else if (this->putAwayCooldownTimer != 0) {\n"
     "            // Replace the \"Put Away\" Do Action label with a blank label while\n"
     "            // the cooldown timer is counting down\n"
     "            doAction = DO_ACTION_NONE;\n"
     "            this->putAwayCooldownTimer--;\n",
     "        } else if (this->putAwayCooldownTimer != 0) {\n"
     "            if (" + O("instant_putaway") + ") { /* port: SoH InstantPutaway */\n"
     "                this->putAwayCooldownTimer = 0;\n"
     "            } else {\n"
     "            doAction = DO_ACTION_NONE;\n"
     "            this->putAwayCooldownTimer--;\n"
     "            }\n"),
    ("SoH: sem a animacao de respirar ao subir da agua",
     "                func_80832340(play, this);\n"
     "                Player_AnimChangeOnceMorph(play, this,\n"
     "                                           (this->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)\n"
     "                                               ? &gPlayerAnim_link_swimer_swim_get\n"
     "                                               : &gPlayerAnim_link_swimer_swim_deep_end);\n",
     "                func_80832340(play, this);\n"
     "                if (" + O("skip_breath_animation") + " && !(this->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {\n"
     "                    /* port: SoH SkipSwimDeepEndAnim */\n"
     "                    s32 portLast = Animation_GetLastFrame(&gPlayerAnim_link_swimer_swim_deep_end);\n"
     "                    LinkAnimation_Change(play, &this->skelAnime, &gPlayerAnim_link_swimer_swim_deep_end, 1.0f,\n"
     "                                         portLast, portLast, ANIMMODE_ONCE, -6.0f);\n"
     "                } else {\n"
     "                Player_AnimChangeOnceMorph(play, this,\n"
     "                                           (this->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)\n"
     "                                               ? &gPlayerAnim_link_swimer_swim_get\n"
     "                                               : &gPlayerAnim_link_swimer_swim_deep_end);\n"
     "                }\n"),
    ("SoH: subir mais rapido em trepadeiras e escadas",
     "    this->skelAnime.playSpeed = phi_f2 * phi_f0;\n",
     "    this->skelAnime.playSpeed = phi_f2 * phi_f0 + phi_f2 * " + O("climb_speed") + "; /* port: SoH ClimbSpeed */\n"),
])

# Skip Save Confirmation: depois de salvar, a mensagem "salvo" some em 0,1 s (em vez de 3 s)
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("SoH: sem a mensagem de salvo (desenho)",
     "        } else if (((pauseCtx->state == PAUSE_STATE_SAVE_PROMPT) &&\n"
     "                    (pauseCtx->savePromptState >= PAUSE_SAVE_PROMPT_STATE_SAVED)) ||\n"
     "                   pauseCtx->state == PAUSE_STATE_GAME_OVER_SAVED) {\n",
     "        } else if (!" + O("skip_save_confirmation") + " && /* port: SoH SkipSaveConfirmation */\n"
     "                   (((pauseCtx->state == PAUSE_STATE_SAVE_PROMPT) &&\n"
     "                    (pauseCtx->savePromptState >= PAUSE_SAVE_PROMPT_STATE_SAVED)) ||\n"
     "                   pauseCtx->state == PAUSE_STATE_GAME_OVER_SAVED)) {\n"),
    ("SoH: sem a mensagem de salvo (pausa)",
     "                            pauseCtx->savePromptState = PAUSE_SAVE_PROMPT_STATE_SAVED;\n"
     "#if !PLATFORM_GC\n                            sDelayTimer = 90;\n",
     "                            pauseCtx->savePromptState = PAUSE_SAVE_PROMPT_STATE_SAVED;\n"
     "#if !PLATFORM_GC\n                            sDelayTimer = " + O("skip_save_confirmation") + " ? 3 : 90; /* port */\n"),
    ("SoH: sem a mensagem de salvo (fim de jogo)",
     "                    pauseCtx->state = PAUSE_STATE_GAME_OVER_SAVED;\n#if !PLATFORM_GC\n                    sDelayTimer = 90;\n",
     "                    pauseCtx->state = PAUSE_STATE_GAME_OVER_SAVED;\n#if !PLATFORM_GC\n"
     "                    sDelayTimer = " + O("skip_save_confirmation") + " ? 3 : 90; /* port */\n"),
])

# ---------------------------------------------------------------- Skips & Speed-ups
# Block Pushing Speed (SoH FasterBlockPush): os blocos e paredes empurraveis andam mais rapido e esperam menos
BP = O("block_push_speed")
patch_file("src/overlays/actors/ovl_Obj_Oshihiki/z_obj_oshihiki.c", [
    ("SoH: empurrar mais rapido", "    this->pushSpeed += 0.5f;\n",
     "    this->pushSpeed += 0.5f + " + BP + " * 0.25f; /* port: SoH FasterBlockPush */\n"),
    ("SoH: empurrar mais rapido (maximo)", "    this->pushSpeed = CLAMP_MAX(this->pushSpeed, 2.0f);\n",
     "    this->pushSpeed = CLAMP_MAX(this->pushSpeed, 2.0f + " + BP + " * 0.5f); /* port */\n"),
    ("SoH: empurrar mais rapido (espera)", "        this->timer = 10;\n",
     "        this->timer = 10 - (" + BP + " * 3) / 2; /* port */\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Spot15_Rrbox/z_bg_spot15_rrbox.c", [
    ("SoH: empurrar mais rapido", "    this->unk_174 += 0.5f;\n",
     "    this->unk_174 += 0.5f + (" + BP + " / 2) * 0.5f; /* port: SoH FasterBlockPush */\n"),
    ("SoH: empurrar mais rapido (maximo)", "    this->unk_174 = CLAMP_MAX(this->unk_174, 2.0f);\n",
     "    this->unk_174 = CLAMP_MAX(this->unk_174, 2.0f + " + BP + " * 0.5f); /* port */\n"),
    ("SoH: empurrar mais rapido (espera)", "        this->unk_168 = 10;\n",
     "        this->unk_168 = 10 - (" + BP + " * 3) / 2; /* port */\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.c", [
    ("SoH: empurrar mais rapido", "    this->dyna.actor.speed += 0.1f;\n    this->dyna.actor.speed = CLAMP_MAX(this->dyna.actor.speed, 2.0f);\n",
     "    this->dyna.actor.speed += 0.1f + " + BP + " * 0.3f; /* port: SoH FasterBlockPush */\n"
     "    this->dyna.actor.speed = CLAMP_MAX(this->dyna.actor.speed, 2.0f + " + BP + " * 0.5f);\n"),
    ("SoH: empurrar mais rapido (espera)", "        this->dyna.actor.speed = 0.0f;\n        this->direction = 5;\n",
     "        this->dyna.actor.speed = 0.0f;\n        this->direction = 5 - (" + BP + " * 3) / 5; /* port */\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Hidan_Rock/z_bg_hidan_rock.c", [
    ("SoH: empurrar mais rapido",
     "            this->dyna.actor.speed += 0.05f;\n            this->dyna.actor.speed = CLAMP_MAX(this->dyna.actor.speed, 2.0f);\n",
     "            this->dyna.actor.speed += 0.05f + " + BP + " * 0.3f; /* port: SoH FasterBlockPush */\n"
     "            this->dyna.actor.speed = CLAMP_MAX(this->dyna.actor.speed, 2.0f + " + BP + " * 0.5f);\n"),
    ("SoH: empurrar mais rapido (espera)", "                this->dyna.actor.speed = 0.0f;\n                this->timer = 5;\n",
     "                this->dyna.actor.speed = 0.0f;\n                this->timer = 5 - (" + BP + " * 3) / 5; /* port */\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Haka_Gate/z_bg_haka_gate.c", [
    ("SoH: girar mais rapido", "        this->vTurnRateDeg10 = 0;\n",
     "        this->vTurnRateDeg10 = " + BP + " * 2; /* port: SoH FasterBlockPush */\n"),
    ("SoH: girar mais rapido (espera)", "        this->vTimer = 5;\n",
     "        this->vTimer = 5 - (" + BP + " * 3) / 5; /* port */\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Mori_Kaitenkabe/z_bg_mori_kaitenkabe.c", [
    ("SoH: girar mais cedo", "        if ((this->timer > 28) && !Player_InCsMode(play)) {\n",
     "        if ((this->timer > 28 - " + BP + " * 4) && !Player_InCsMode(play)) { /* port: SoH FasterBlockPush */\n"),
    ("SoH: girar mais rapido", "    this->rotSpeed = 0.0f;\n    this->rotYdeg = 0.0f;\n",
     "    this->rotSpeed = " + BP + " * 0.1f; /* port */\n    this->rotYdeg = 0.0f;\n"),
])
patch_file("src/overlays/actors/ovl_Bg_Jya_Cobra/z_bg_jya_cobra.c", [
    ("SoH: girar mais cedo", "        if (this->unk_168 >= 15) {\n",
     "        if (this->unk_168 >= 15 - " + BP + " * 2) { /* port: SoH FasterBlockPush */\n"),
    ("SoH: girar mais rapido", "    this->unk_170 = this->unk_16E = 0;\n    this->unk_172 = true;\n",
     "    this->unk_170 = this->unk_16E = 0;\n    this->unk_16E = " + BP + " * 20; /* port */\n    this->unk_172 = true;\n"),
])

# Biggoron Forge Time: dias para o Biggoron forjar a espada
patch_file("src/overlays/actors/ovl_En_Go2/z_en_go2.c", [
    ("SoH: dias da forja", "Environment_GetBgsDayCount() >= 3", "Environment_GetBgsDayCount() >= " + O("forge_time_days"), 2),
])

# Better Owl: na conversa da coruja (Kaepora Gaebora) a resposta ja vem em "entendi" (nao repete a fala)
patch_file("src/code/z_message.c", [
    ("SoH: coruja",
     "            case MESSAGE_TWO_CHOICE:\n"
     "                msgCtx->textboxEndType = TEXTBOX_ENDTYPE_2_CHOICE;\n"
     "                if (msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) {\n"
     "                    msgCtx->choiceTextId = msgCtx->textId;\n"
     "                    msgCtx->stateTimer = 4;\n"
     "                    msgCtx->choiceIndex = 0;\n",
     "            case MESSAGE_TWO_CHOICE:\n"
     "                msgCtx->textboxEndType = TEXTBOX_ENDTYPE_2_CHOICE;\n"
     "                if (msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) {\n"
     "                    msgCtx->choiceTextId = msgCtx->textId;\n"
     "                    msgCtx->stateTimer = 4;\n"
     "                    msgCtx->choiceIndex = 0;\n"
     "                    if (" + O("better_owl") + " && /* port: SoH BetterOwl */\n"
     "                        (msgCtx->textId == 0x2066 || msgCtx->textId == 0x607B || msgCtx->textId == 0x10C2 ||\n"
     "                         msgCtx->textId == 0x10C6 || msgCtx->textId == 0x206A)) {\n"
     "                        msgCtx->choiceIndex = 1;\n"
     "                    }\n"),
])

# Play Zelda's Lullaby to Open Sleeping Waterfall: 1 = tocar uma vez e a cachoeira fica aberta para sempre;
# 2 = nunca precisa (basta ter aprendido a musica e ter a ocarina)
patch_file("src/overlays/actors/ovl_Bg_Spot03_Taki/z_bg_spot03_taki.c", [
    ("SoH: cachoeira (cabecalhos)", '#include "play_state.h"\n', '#include "play_state.h"\n#include "save.h" /* port */\n#include "item.h" /* port */\n'),
    ("SoH: cachoeira aberta",
     "void BgSpot03Taki_HandleWaterfallState(BgSpot03Taki* this, PlayState* play) {\n",
     "void BgSpot03Taki_HandleWaterfallState(BgSpot03Taki* this, PlayState* play) {\n"
     "    { /* port: SoH TimeSavers.SleepingWaterfall */\n"
     "        s32 portMode = " + O("sleeping_waterfall") + ";\n"
     "        if ((portMode == 1 && GET_EVENTCHKINF(EVENTCHKINF_39)) ||\n"
     "            (portMode == 2 && CHECK_QUEST_ITEM(QUEST_SONG_LULLABY) && INV_CONTENT(ITEM_OCARINA_OF_TIME) != ITEM_NONE)) {\n"
     "            if (this->state != WATERFALL_OPENED || this->openingAlpha != 0) {\n"
     "                Flags_SetSwitch(play, this->switchFlag);\n"
     "                DynaPoly_DisableCollision(play, &play->colCtx.dyna, this->dyna.bgId);\n"
     "                this->state = WATERFALL_OPENED;\n"
     "                this->openingAlpha = 0;\n"
     "                BgSpot03Taki_ApplyOpeningAlpha(this, 0);\n"
     "                BgSpot03Taki_ApplyOpeningAlpha(this, 1);\n"
     "            }\n"
     "            this->timer = 400;\n"
     "            return;\n"
     "        }\n"
     "    }\n"),
])

# Skip Playing Scarecrow's Song: o Pierre aparece quando a ocarina e tirada (precisa ter ensinado a musica)
patch_file("src/overlays/actors/ovl_En_Kakasi2/z_en_kakasi2.c", [
    ("SoH: espantalho na hora",
     "    if (DEBUG_FEATURES && (BREG(1) != 0) && (this->actor.xzDistToPlayer < this->maxSpawnDistance.x) &&\n",
     "    if (((DEBUG_FEATURES && (BREG(1) != 0)) || /* port: SoH InstantScarecrow */\n"
     "         (" + O("instant_scarecrow") + " && play->msgCtx.msgMode == MSGMODE_OCARINA_PLAYING &&\n"
     "          gSaveContext.save.info.scarecrowSpawnSongSet)) &&\n"
     "        (this->actor.xzDistToPlayer < this->maxSpawnDistance.x) &&\n"),
])

# King Zora Speed: o Rei Zora sai do caminho N vezes mais rapido
patch_file("src/overlays/actors/ovl_En_Kz/z_en_kz.c", [
    ("SoH: rei zora mais rapido",
     "void EnKz_Mweep(EnKz* this, PlayState* play) {\n    UNUSED Vec3f zeroVec = { 0.0f, 0.0f, 0.0f };\n    Vec3f subCamAt;\n    Vec3f subCamEye;\n\n    subCamAt = this->actor.world.pos;\n    subCamEye = this->actor.home.pos;\n",
     "void EnKz_Mweep(EnKz* this, PlayState* play) {\n    UNUSED Vec3f zeroVec = { 0.0f, 0.0f, 0.0f };\n    Vec3f subCamAt;\n    Vec3f subCamEye;\n\n"
     "    if (" + O("king_zora_speed") + " > 1) { /* port: SoH MweepSpeed */\n"
     "        this->actor.speed = 0.1f * " + O("king_zora_speed") + ";\n"
     "    }\n"
     "    subCamAt = this->actor.world.pos;\n    subCamEye = this->actor.home.pos;\n"),
])

# Crawl Speed: engatinhar nos tuneis N vezes mais rapido (entrada, saida e a camera da saida acompanham)
CR = O("crawl_speed")
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: engatinhar (entrada)",
     "                this->actor.prevPos = this->actor.world.pos;\n"
     "                Player_AnimPlayOnce(play, this, &gPlayerAnim_link_child_tunnel_start);\n",
     "                this->actor.prevPos = this->actor.world.pos;\n"
     "                LinkAnimation_Change(play, &this->skelAnime, &gPlayerAnim_link_child_tunnel_start, /* port: SoH CrawlSpeed */\n"
     "                                     (" + CR + " + 1.0f) / 2.0f, 0.0f,\n"
     "                                     Animation_GetLastFrame(&gPlayerAnim_link_child_tunnel_start), ANIMMODE_ONCE, 0.0f);\n"),
    ("SoH: engatinhar (saida)",
     "                Player_AnimPlayOnce(play, this, &gPlayerAnim_link_child_tunnel_end);\n",
     "                LinkAnimation_Change(play, &this->skelAnime, &gPlayerAnim_link_child_tunnel_end, /* port: SoH CrawlSpeed */\n"
     "                                     (" + CR + " + 1.0f) / 2.0f, 0.0f,\n"
     "                                     Animation_GetLastFrame(&gPlayerAnim_link_child_tunnel_end), ANIMMODE_ONCE, 0.0f);\n"),
    ("SoH: engatinhar (saida de costas)",
     "                LinkAnimation_Change(play, &this->skelAnime, &gPlayerAnim_link_child_tunnel_start, -1.0f,\n",
     "                LinkAnimation_Change(play, &this->skelAnime, &gPlayerAnim_link_child_tunnel_start,\n"
     "                                     -1.0f * ((" + CR + " + 1.0f) / 2.0f), /* port: SoH CrawlSpeed */\n"),
    ("SoH: engatinhar (velocidade)",
     "                this->speedXZ = sControlInput->rel.stick_y * 0.03f;\n",
     "                this->speedXZ = sControlInput->rel.stick_y * 0.03f * (" + CR + " > 1 ? " + CR + " : 1); /* port */\n"),
])
patch_file("src/code/z_onepointdemo.c", [
    ("SoH: camera da saida do tunel",
     "sCrawlspaceActionParam | 0x1000, sCrawlspaceTimer,",
     "sCrawlspaceActionParam | 0x1000, sCrawlspaceTimer / (" + CR + " > 1 ? " + CR + " : 1),", 2),
])

# Faster Heavy Block Lift: levantar as pedras prateadas grandes e os obeliscos sem a cena e bem mais rapido
patch_file("src/overlays/actors/ovl_Bg_Heavy_Block/z_bg_heavy_block.c", [
    ("SoH: levantar sem a cena 1", "                OnePointCutscene_Init(play, 4020, 270, &this->dyna.actor, CAM_ID_MAIN);\n",
     "                if (!" + O("faster_heavy_lift") + ") { /* port: SoH FasterHeavyBlockLift */\n"
     "                    OnePointCutscene_Init(play, 4020, 270, &this->dyna.actor, CAM_ID_MAIN);\n                }\n"),
    ("SoH: levantar sem a cena 2", "                OnePointCutscene_Init(play, 4021, 220, &this->dyna.actor, CAM_ID_MAIN);\n",
     "                if (!" + O("faster_heavy_lift") + ") { /* port */\n"
     "                    OnePointCutscene_Init(play, 4021, 220, &this->dyna.actor, CAM_ID_MAIN);\n                }\n"),
    ("SoH: levantar sem a cena 3", "                OnePointCutscene_Init(play, 4022, 210, &this->dyna.actor, CAM_ID_MAIN);\n",
     "                if (!" + O("faster_heavy_lift") + ") { /* port */\n"
     "                    OnePointCutscene_Init(play, 4022, 210, &this->dyna.actor, CAM_ID_MAIN);\n                }\n"),
    ("SoH: levantar sem prender o Link",
     "    this->timer++;\n\n    Player_SetCsActionWithHaltedActors(play, &player->actor, PLAYER_CSACTION_8);\n",
     "    this->timer++;\n\n    if (!" + O("faster_heavy_lift") + ") { /* port */\n"
     "        Player_SetCsActionWithHaltedActors(play, &player->actor, PLAYER_CSACTION_8);\n    }\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: levantar rapido (animacao)",
     "                Player_SetupAction(play, this, Player_Action_80846050, 0);\n"
     "                anim = GET_PLAYER_ANIM(PLAYER_ANIMGROUP_carryB, this->modelAnimType);\n"
     "            }\n\n"
     "            Player_AnimPlayOnce(play, this, anim);\n",
     "                Player_SetupAction(play, this, Player_Action_80846050, 0);\n"
     "                anim = GET_PLAYER_ANIM(PLAYER_ANIMGROUP_carryB, this->modelAnimType);\n"
     "            }\n\n"
     "            if (" + O("faster_heavy_lift") + " && (interactRangeActor->id == ACTOR_BG_HEAVY_BLOCK ||\n"
     "                (interactRangeActor->id == ACTOR_EN_ISHI && PARAMS_GET_U(interactRangeActor->params, 0, 4) == 1))) {\n"
     "                LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, anim, 5.0f); /* port: SoH FasterHeavyBlockLift */\n"
     "            } else {\n"
     "            Player_AnimPlayOnce(play, this, anim);\n"
     "            }\n"),
    ("SoH: levantar rapido (arremesso)",
     "    } else if (LinkAnimation_OnFrame(&this->skelAnime, 229.0f)) {\n"
     "        Actor* heldActor = this->heldActor;\n\n",
     "    } else if (LinkAnimation_OnFrame(&this->skelAnime, 229.0f)) {\n"
     "        Actor* heldActor = this->heldActor;\n\n"
     "        if (" + O("faster_heavy_lift") + ") { /* port: SoH FasterHeavyBlockLift */\n"
     "            heldActor->shape.rot.x -= 3510;\n"
     "        }\n"),
])

# ---------------------------------------------------------------- Graphics
# Show Chains on Both Sides of Locked Doors: as correntes da porta trancada tambem do lado de dentro
patch_file("src/overlays/actors/ovl_En_Door/z_en_door.c", [
    ("SoH: correntes (cabecalho)", '#include "z_en_door.h"\n', '#include "z_en_door.h"\n#include "sys_matrix.h" /* port */\n'),
    ("SoH: correntes dos dois lados",
     "        if (this->lockTimer != 0) {\n            Actor_DrawDoorLock(play, this->lockTimer, DOORLOCK_NORMAL);\n        }\n",
     "        if (this->lockTimer != 0) {\n"
     "            if (" + O("door_locks_both_sides") + ") { /* port: SoH ShowDoorLocksOnBothSides */\n"
     "                Matrix_Push();\n"
     "                Actor_DrawDoorLock(play, this->lockTimer, DOORLOCK_NORMAL);\n"
     "                Matrix_Pop();\n"
     "                Matrix_RotateZYX(0, 0x8000, 0, MTXMODE_APPLY);\n"
     "            }\n"
     "            Actor_DrawDoorLock(play, this->lockTimer, DOORLOCK_NORMAL);\n        }\n"),
])

# Remember Minimap State Between Areas: o minimapa desligado (L) continua desligado ao trocar de area
patch_file("src/code/z_construct.c", [
    ("SoH: lembrar o minimapa", "    R_MINIMAP_DISABLED = false;\n",
     "    if (!" + O("remember_minimap") + ") { /* port: SoH RememberMapToggleState */\n"
     "        R_MINIMAP_DISABLED = false;\n    }\n"),
])

# Enable Passage of Time on File Select: o ceu da tela de arquivos passa do dia para a noite
patch_file("src/overlays/gamestates/ovl_file_choose/z_file_choose.c", [
    ("SoH: tempo passando na tela de arquivos",
     "    FileSelectState* this = (FileSelectState*)thisx;\n    Input* input = &this->state.input[0];\n\n"
     "    OPEN_DISPS(this->state.gfxCtx, \"../z_file_choose.c\", 2898);\n",
     "    FileSelectState* this = (FileSelectState*)thisx;\n    Input* input = &this->state.input[0];\n\n"
     "    if (" + O("file_select_time_flow") + ") { /* port: SoH FileSelect.TimeFlow */\n"
     "        gSaveContext.skyboxTime += 0x10;\n    }\n"
     "    OPEN_DISPS(this->state.gfxCtx, \"../z_file_choose.c\", 2898);\n"),
])

# Dynamic Wallet Icon (cor do icone de rupia pela carteira) e Visual Stone of Agony (icone da Pedra da Agonia no
# HUD: aparece perto de um segredo e pisca quando o controle tremeria - o Miyoo nao tem vibracao)
PORT_AGONY = r'''
/* port: SoH VisualAgony (Player_DetectRumbleSecrets marca; desenhado aqui com o HUD) */
f32 Port_AgonyDist = 1.0e9f;
s32 Port_AgonyFlash;
static u64 sPortAgonyTex[24 * 24 * 4 / 8];
static s32 sPortAgonyLoaded;

static Gfx* Port_DrawVisualAgony(PlayState* play, Gfx* gfx) {
    Player* player = GET_PLAYER(play);
    s32 alpha = 50;
    if (!({ extern int port_opt_visual_agony; port_opt_visual_agony; }) || !CHECK_QUEST_ITEM(QUEST_STONE_OF_AGONY) ||
        Port_AgonyDist > 80000.0f ||
        (player->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD | PLAYER_STATE1_29)) ||
        play->interfaceCtx.magicAlpha == 0) {
        Port_AgonyFlash = 0;
        return gfx;
    }
    if (!sPortAgonyLoaded) {
        DMA_REQUEST_SYNC(sPortAgonyTex, (uintptr_t)_icon_item_24_staticSegmentRomStart + 0x5100, sizeof(sPortAgonyTex),
                         "port", 0);
        sPortAgonyLoaded = 1;
    }
    if (Port_AgonyFlash > 0) {
        Port_AgonyFlash--;
        alpha = 255;
    }
    alpha = alpha * play->interfaceCtx.magicAlpha / 255;
    gDPPipeSync(gfx++);
    gDPSetCombineLERP(gfx++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0, PRIMITIVE,
                      ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
    gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, alpha);
    gDPSetEnvColor(gfx++, 0, 0, 0, 255);
    gDPLoadTextureBlock(gfx++, sPortAgonyTex, G_IM_FMT_RGBA, G_IM_SIZ_32b, 24, 24, 0, G_TX_NOMIRROR | G_TX_WRAP,
                        G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPTextureRectangle(gfx++, 26 << 2, 60 << 2, (26 + 24) << 2, (60 + 24) << 2, G_TX_RENDERTILE, 0, 0, 1 << 10,
                        1 << 10);
    gDPPipeSync(gfx++);
    return gfx;
}

'''
patch_file("src/code/z_parameter.c", [
    ("SoH: pedra da agonia (funcao)", "void Interface_Draw(PlayState* play) {\n",
     PORT_AGONY + "void Interface_Draw(PlayState* play) {\n"),
    ("SoH: cor da rupia pela carteira e pedra da agonia",
     "        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 200, 255, 100, interfaceCtx->magicAlpha);\n"
     "        gDPSetEnvColor(OVERLAY_DISP++, 0, 80, 0, 255);\n"
     "        OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gRupeeCounterIconTex, 16, 16, 26, 206, 16, 16, 1 << 10, 1 << 10);\n",
     "        if (" + O("dynamic_wallet_icon") + " && CUR_UPG_VALUE(UPG_WALLET) == 1) { /* port: SoH DynamicWalletIcon */\n"
     "            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 0x82, 0x82, 0xFF, interfaceCtx->magicAlpha);\n"
     "        } else if (" + O("dynamic_wallet_icon") + " && CUR_UPG_VALUE(UPG_WALLET) >= 2) {\n"
     "            gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 0xFF, 0x64, 0x64, interfaceCtx->magicAlpha);\n"
     "        } else {\n"
     "        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 200, 255, 100, interfaceCtx->magicAlpha);\n"
     "        }\n"
     "        gDPSetEnvColor(OVERLAY_DISP++, 0, 80, 0, 255);\n"
     "        OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gRupeeCounterIconTex, 16, 16, 26, 206, 16, 16, 1 << 10, 1 << 10);\n"
     "        OVERLAY_DISP = Port_DrawVisualAgony(play, OVERLAY_DISP); /* port: SoH VisualAgony */\n"
     "        Gfx_SetupDL_39Overlay(play->state.gfxCtx);\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: pedra da agonia visual",
     "        this->unk_6A0 += temp;\n        if (this->unk_6A0 > 4000000.0f) {\n",
     "        this->unk_6A0 += temp;\n"
     "        { /* port: SoH VisualAgony (o icone e desenhado em z_parameter.c) */\n"
     "            extern f32 Port_AgonyDist;\n"
     "            extern s32 Port_AgonyFlash;\n"
     "            Port_AgonyDist = this->closestSecretDistSq;\n"
     "            if (this->unk_6A0 > 4000000.0f && " + O("visual_agony") + ") {\n"
     "                Port_AgonyFlash = 3;\n"
     "                if (Port_AgonyDist <= 80000.0f) {\n"
     "                    Sfx_PlaySfxCentered(NA_SE_SY_MESSAGE_WOMAN);\n"
     "                }\n"
     "            }\n"
     "        }\n"
     "        if (this->unk_6A0 > 4000000.0f) {\n"),
])

# ---------------------------------------------------------------- Items
# Prevent Dropped Ocarina Inputs: duas notas seguidas em quadros seguidos (no direcional) nao se perdem
patch_file("src/audio/game/general.c", [
    ("SoH: ocarina sem perder notas",
     "    if ((sOcarinaFlags != 0) && (sOcarinaDropInputTimer != 0)) {\n        sOcarinaDropInputTimer--;\n        return;\n    }\n",
     "    if ((sOcarinaFlags != 0) && (sOcarinaDropInputTimer != 0)) {\n        sOcarinaDropInputTimer--;\n"
     "        if (!" + O("no_dropped_ocarina_inputs") + ") { /* port: SoH DpadNoDropOcarinaInput */\n"
     "            return;\n        }\n    }\n"),
])

# Deku Nuts Explode Bombs: a noz de Deku faz a bomba explodir
patch_file("src/overlays/actors/ovl_En_Bom/z_en_bom.c", [
    ("SoH: noz explode bomba",
     "    this->explosionColliderElements[0].base.atDmgInfo.damage += (thisx->shape.rot.z & 0xFF00) >> 8;\n",
     "    this->explosionColliderElements[0].base.atDmgInfo.damage += (thisx->shape.rot.z & 0xFF00) >> 8;\n"
     "    if (" + O("nuts_explode_bombs") + ") { /* port: SoH NutsExplodeBombs */\n"
     "        this->bombCollider.elem.acDmgInfo.dmgFlags |= 1;\n    }\n"),
])

PORT_PLAYER_ITEMS = r'''
/* port: SoH PersistentMasks (lembrada so enquanto o jogo esta aberto) e FastBoomerang */
extern s32 Port_MaskMemory; /* (em port/ovl_reset.c: sobrevive a recarga do overlay do Link) */
s32 Port_BoomerangRecall;
'''
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: variaveis", "void Player_ProcessItemButtons(Player* this, PlayState* play) {\n",
     PORT_PLAYER_ITEMS + "void Player_ProcessItemButtons(Player* this, PlayState* play) {\n"),
    # Persistent Masks: a mascara continua no rosto mesmo saindo dos botoes, e volta depois de trocar de area
    ("SoH: mascara fica",
     "    s32 i;\n\n    if (this->currentMask != PLAYER_MASK_NONE) {\n        maskItemAction = this->currentMask - 1 + PLAYER_IA_MASK_KEATON;\n",
     "    s32 i;\n\n    if (this->currentMask != PLAYER_MASK_NONE && !" + O("persistent_masks") + ") { /* port: SoH PersistentMasks */\n"
     "        maskItemAction = this->currentMask - 1 + PLAYER_IA_MASK_KEATON;\n"),
    ("SoH: mascara lembrada",
     "                    this->currentMask = itemAction - PLAYER_IA_MASK_KEATON + 1;\n                }\n\n"
     "                func_808328EC(this, NA_SE_PL_CHANGE_ARMS);\n",
     "                    this->currentMask = itemAction - PLAYER_IA_MASK_KEATON + 1;\n                }\n"
     "                Port_MaskMemory = this->currentMask; /* port: SoH PersistentMasks */\n\n"
     "                func_808328EC(this, NA_SE_PL_CHANGE_ARMS);\n"),
    ("SoH: mascara de volta ao entrar na area",
     "    Map_SavePlayerInitialInfo(play);\n    MREG(64) = 0;\n}\n",
     "    if (" + O("persistent_masks") + ") { /* port: SoH PersistentMasks */\n"
     "        if (INV_CONTENT(ITEM_TRADE_CHILD) == ITEM_SOLD_OUT || LINK_IS_ADULT) {\n"
     "            Port_MaskMemory = PLAYER_MASK_NONE;\n"
     "        }\n"
     "        this->currentMask = Port_MaskMemory;\n"
     "    }\n"
     "    Map_SavePlayerInitialInfo(play);\n    MREG(64) = 0;\n}\n"),
    # Prevent Bombchus Forcing First-Person
    ("SoH: bombchu sem primeira pessoa", "            if (heldActor->id == ACTOR_EN_BOM_CHU) {\n",
     "            if (heldActor->id == ACTOR_EN_BOM_CHU && !" + O("no_first_person_bombchus") + ") { /* port: SoH DisableFirstPersonChus */\n"),
    # Instant Boomerang Recall: apertar o botao do bumerangue com ele no ar faz ele voltar na hora
    ("SoH: bumerangue volta na hora",
     "        Player_PlaySfx(this, NA_SE_PL_CATCH_BOOMERANG);\n        Player_PlayVoiceSfx(this, NA_SE_VO_LI_SWORD_N);\n\n"
     "        return true;\n    }\n\n    return false;\n}\n",
     "        Player_PlaySfx(this, NA_SE_PL_CATCH_BOOMERANG);\n        Player_PlayVoiceSfx(this, NA_SE_VO_LI_SWORD_N);\n\n"
     "        return true;\n    }\n\n"
     "    if (sUseHeldItem && " + O("fast_boomerang") + ") { /* port: SoH FastBoomerang */\n"
     "        Port_BoomerangRecall = true;\n    }\n\n"
     "    return false;\n}\n"),
    # Faster Farore's Wind: a magia de Farore com metade da animacao
    ("SoH: farore rapido (inicio)",
     "    LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, &gPlayerAnim_link_magic_tame, 0.83f);\n\n"
     "    if (magicSpell == 5) {\n",
     "    if (" + O("fast_farores") + " && this->itemAction == PLAYER_IA_FARORES_WIND) { /* port: SoH FastFarores */\n"
     "        LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, &gPlayerAnim_link_magic_tame, 0.83f * 2);\n"
     "        return;\n    }\n"
     "    LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, &gPlayerAnim_link_magic_tame, 0.83f);\n\n"
     "    if (magicSpell == 5) {\n"),
    ("SoH: farore rapido (fim)",
     "            if ((this->itemAction == PLAYER_IA_NAYRUS_LOVE) || (gSaveContext.magicState == MAGIC_STATE_IDLE)) {\n",
     "            if ((this->itemAction == PLAYER_IA_NAYRUS_LOVE) || (gSaveContext.magicState == MAGIC_STATE_IDLE) ||\n"
     "                (" + O("fast_farores") + " && this->itemAction == PLAYER_IA_FARORES_WIND)) { /* port */\n"),
    ("SoH: farore rapido (magia)",
     "                LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, D_80854A58[this->av1.actionVar1], 0.83f);\n",
     "                LinkAnimation_PlayOnceSetSpeed(play, &this->skelAnime, D_80854A58[this->av1.actionVar1],\n"
     "                                               0.83f * ((" + O("fast_farores") + " && this->itemAction == PLAYER_IA_FARORES_WIND) ? 2 : 1));\n"),
    ("SoH: farore rapido (laco)",
     "                LinkAnimation_PlayLoopSetSpeed(play, &this->skelAnime, D_80854A64[this->av1.actionVar1], 0.83f);\n",
     "                LinkAnimation_PlayLoopSetSpeed(play, &this->skelAnime, D_80854A64[this->av1.actionVar1],\n"
     "                                               0.83f * ((" + O("fast_farores") + " && this->itemAction == PLAYER_IA_FARORES_WIND) ? 2 : 1));\n"),
])
patch_file("src/code/z_actor.c", [
    ("SoH: farore rapido (ponto de volta)", "            D_8015BC14--;\n",
     "            D_8015BC14 -= " + O("fast_farores") + " ? (D_8015BC14 >= 5 ? 5 : D_8015BC14) : 1; /* port: SoH FastFarores */\n"),
])
patch_file("src/overlays/actors/ovl_En_Boom/z_en_boom.c", [
    ("SoH: bumerangue volta na hora",
     "    if (DECR(this->returnTimer) == 0) {\n",
     "    if (DECR(this->returnTimer) == 0 || ({ extern s32 Port_BoomerangRecall; Port_BoomerangRecall; })) { /* port: SoH FastBoomerang */\n"),
    ("SoH: bumerangue pego na hora",
     "        if (distFromLink < 40.0f) {\n",
     "        if (distFromLink < 40.0f || ({ extern s32 Port_BoomerangRecall; Port_BoomerangRecall; })) { /* port */\n"
     "            { extern s32 Port_BoomerangRecall; Port_BoomerangRecall = false; }\n"),
])

# Skip Magic Arrow Equip Animation: equipar flechas magicas sem a animacao de juntar com o arco
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_item.c", [
    ("SoH: flecha magica sem animacao",
     "                            sEquipMoveTimer = 10;\n"
     "                            if ((pauseCtx->equipTargetItem == ITEM_ARROW_FIRE) ||\n",
     "                            sEquipMoveTimer = 10;\n"
     "                            if (!" + O("skip_arrow_animation") + " && /* port: SoH SkipArrowAnimation */\n"
     "                                ((pauseCtx->equipTargetItem == ITEM_ARROW_FIRE) ||\n"),
    ("SoH: flecha magica sem animacao (fim da condicao)",
     "                                (pauseCtx->equipTargetItem == ITEM_ARROW_LIGHT)) {\n"
     "                                index = 0;\n",
     "                                (pauseCtx->equipTargetItem == ITEM_ARROW_LIGHT))) {\n"
     "                                index = 0;\n"),
])

# Better Farore's Wind (parte das restricoes): a magia de Farore tambem no Campo de Treinamento Gerudo e dentro do
# Castelo de Ganon
patch_file("src/code/z_parameter.c", [
    ("SoH: farore em mais lugares",
     "            interfaceCtx->restrictions.all = (sRestrictionFlags[i].flags3 & 0x03) >> 0;\n",
     "            interfaceCtx->restrictions.all = (sRestrictionFlags[i].flags3 & 0x03) >> 0;\n"
     "            if (" + O("better_farore") + " && /* port: SoH BetterFarore */\n"
     "                (sceneId == SCENE_GERUDO_TRAINING_GROUND || sceneId == SCENE_INSIDE_GANONS_CASTLE)) {\n"
     "                interfaceCtx->restrictions.farores = 0;\n"
     "            }\n"),
])
# Better Farore's Wind (parte das idades): crianca e adulto guardam cada um o seu ponto de Farore (trocados ao tirar
# ou recolocar a Espada Mestra). O ponto da outra idade fica so na memoria (some ao fechar o jogo).
patch_file("src/code/z_demo.c", [
    ("SoH: farore por idade",
     "            case CS_DEST_TEMPLE_OF_TIME_FROM_MASTER_SWORD:\n"
     "                gSaveContext.save.info.fw.set = 0;\n"
     "                gSaveContext.respawn[RESPAWN_MODE_TOP].data = 0;\n",
     "            case CS_DEST_TEMPLE_OF_TIME_FROM_MASTER_SWORD:\n"
     "                if (" + O("better_farore") + ") { /* port: SoH BetterFarore */\n"
     "                    static FaroresWindData sPortBackupFW;\n"
     "                    FaroresWindData portTemp = sPortBackupFW;\n"
     "                    sPortBackupFW = gSaveContext.save.info.fw;\n"
     "                    gSaveContext.save.info.fw = portTemp;\n"
     "                } else {\n"
     "                gSaveContext.save.info.fw.set = 0;\n"
     "                gSaveContext.respawn[RESPAWN_MODE_TOP].data = 0;\n"
     "                }\n"),
])

# ---------------------------------------------------------------- Fixes
# Fix Zora Hint Dialogue: o Zora para de mandar procurar a Ruth no lago depois que voce ja pegou a carta
patch_file("src/overlays/actors/ovl_En_Zo/z_en_zo.c", [
    ("SoH: dica do zora",
     "            if (GET_EVENTCHKINF(EVENTCHKINF_GAVE_LETTER_TO_KING_ZORA)) {\n                return 0x4010;\n",
     "            if (GET_EVENTCHKINF(EVENTCHKINF_GAVE_LETTER_TO_KING_ZORA) ||\n"
     "                (" + O("fix_zora_hint") + " && GET_EVENTCHKINF(EVENTCHKINF_31))) { /* port: SoH FixZoraHintDialogue */\n"
     "                return 0x4010;\n"),
])

# Fix Enemies not Spawning Near Water: o teste de agua dos inimigos do campo estava com o sinal trocado
patch_file("src/overlays/actors/ovl_En_Encount1/z_en_encount1.c", [
    ("SoH: inimigos perto da agua",
     "                    (floorY < (player->actor.world.pos.y - player->actor.depthInWater))) {\n",
     "                    (floorY < (player->actor.world.pos.y +\n"
     "                               player->actor.depthInWater * (" + O("fix_enemy_spawns_water") + " ? 1 : -1)))) { /* port: SoH EnemySpawnsOverWaterboxes */\n"),
])

# Fix Camera Drift / Fix Camera Swing / Fix Hanging Ledge Swing Rate
patch_file("src/code/z_camera.c", [
    ("SoH: camera sem deriva",
     "    return Camera_LERPCeilF(distTarget, camera->dist, 1.0f / camera->rUpdateRateInv, 0.2f);\n}\n",
     "    return Camera_LERPCeilF(distTarget, camera->dist, 1.0f / camera->rUpdateRateInv,\n"
     "                            " + O("fix_camera_drift") + " ? 0.0f : 0.2f); /* port: SoH FixCameraDrift */\n}\n", 2),
    ("SoH: camera sem balancar",
     "        if (rwData->startSwingTimer > 0) {\n"
     "            func_80046E20(camera, &eyeAdjustment, roData->distMin, roData->unk_0C, &sp98, &rwData->swing);\n"
     "        } else {\n"
     "            sp88 = *eyeNext;\n"
     "            rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;\n"
     "            if (Camera_BGCheck(camera, at, &sp88)) {\n"
     "                rwData->swingYawTarget = atEyeNextGeo.yaw;\n"
     "                rwData->startSwingTimer = -1;\n"
     "            } else {\n"
     "                *eye = *eyeNext;\n"
     "            }\n"
     "            rwData->swing.unk_18 = 0;\n"
     "        }\n",
     "        if (" + O("fix_camera_swing") + ") { /* port: SoH FixCameraSwing */\n"
     "            if (rwData->startSwingTimer <= 0) {\n"
     "                rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;\n"
     "                rwData->swing.unk_18 = 0;\n"
     "            }\n"
     "            func_80046E20(camera, &eyeAdjustment, roData->distMin, roData->unk_0C, &sp98, &rwData->swing);\n"
     "        } else\n"
     "        if (rwData->startSwingTimer > 0) {\n"
     "            func_80046E20(camera, &eyeAdjustment, roData->distMin, roData->unk_0C, &sp98, &rwData->swing);\n"
     "        } else {\n"
     "            sp88 = *eyeNext;\n"
     "            rwData->swing.swingUpdateRate = camera->yawUpdateRateInv = roData->unk_0C * 2.0f;\n"
     "            if (Camera_BGCheck(camera, at, &sp88)) {\n"
     "                rwData->swingYawTarget = atEyeNextGeo.yaw;\n"
     "                rwData->startSwingTimer = -1;\n"
     "            } else {\n"
     "                *eye = *eyeNext;\n"
     "            }\n"
     "            rwData->swing.unk_18 = 0;\n"
     "        }\n"),
    ("SoH: pendurado na beirada",
     "    sp8C.yaw = Camera_LERPFloorS(rwData->yawTarget, eyeNextAtOffset.yaw, 0.5f, 0x2710);\n",
     "    sp8C.yaw = Camera_LERPFloorS(rwData->yawTarget, eyeNextAtOffset.yaw, 0.5f,\n"
     "                                 " + O("fix_ledge_swing_rate") + " ? 0xA : 0x2710); /* port: SoH FixHangingLedgeSwingRate */\n"),
])
patch_file("src/code/z_olib.c", [
    ("SoH: seno e cosseno exatos (camera sem deriva)",
     "    sinPitch = Math_SinS(sph->pitch);\n    sinYaw = Math_SinS(sph->yaw);\n",
     "    sinPitch = Math_SinS(sph->pitch);\n    sinYaw = Math_SinS(sph->yaw);\n"
     "    if (" + O("fix_camera_drift") + ") { /* port: SoH FixCameraDrift (seno/cosseno exatos, nao da tabela) */\n"
     "        extern float sinf(float);\n        extern float cosf(float);\n"
     "        cosPitch = cosf((sph->pitch & 0xFFFC) * (3.14159265f / 32767.0f));\n"
     "        cosYaw = cosf((sph->yaw & 0xFFFC) * (3.14159265f / 32767.0f));\n"
     "        sinPitch = sinf((sph->pitch & 0xFFFC) * (3.14159265f / 32767.0f));\n"
     "        sinYaw = sinf((sph->yaw & 0xFFFC) * (3.14159265f / 32767.0f));\n"
     "    }\n"),
])

# Fix L&R Pause Menu: o botao L do menu de pausa com a mesma cor do R (o original pinta com a cor do painel)
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("SoH: cor do L no menu",
     "    gSPDisplayList(POLY_OPA_DISP++, gInfoPanelBgDL);\n\n"
     "    if ((pauseCtx->cursorSpecialPos == PAUSE_CURSOR_PAGE_LEFT)",
     "    gSPDisplayList(POLY_OPA_DISP++, gInfoPanelBgDL);\n\n"
     "    if (" + O("fix_menu_lr") + ") { /* port: SoH FixMenuLR */\n"
     "        gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 180, 210, 255, 220);\n    }\n"
     "    if ((pauseCtx->cursorSpecialPos == PAUSE_CURSOR_PAGE_LEFT)"),
])

# Fix Dungeon Entrances: o icone de entrada de masmorra sem posicao definida (aparecia no canto da tela) some
patch_file("src/code/z_map_exp.c", [
    ("SoH: icone de entrada fora do lugar",
     "                        if ((gMapData->owEntranceFlag[sEntranceIconMapIndex] == 0xFFFF) ||\n"
     "                            ((gMapData->owEntranceFlag[sEntranceIconMapIndex] != 0xFFFF) &&\n"
     "                             (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] &\n"
     "                              gBitFlags[gMapData->owEntranceFlag[mapIndex]]))) {\n",
     "                        if (!(" + O("fix_dungeon_entrance_icons") + " && /* port: SoH FixDungeonMinimapIcon */\n"
     "                              gMapData->owEntranceIconPosY[sEntranceIconMapIndex] == 0) &&\n"
     "                            ((gMapData->owEntranceFlag[sEntranceIconMapIndex] == 0xFFFF) ||\n"
     "                            ((gMapData->owEntranceFlag[sEntranceIconMapIndex] != 0xFFFF) &&\n"
     "                             (gSaveContext.save.info.infTable[INFTABLE_INDEX_1AX] &\n"
     "                              gBitFlags[gMapData->owEntranceFlag[mapIndex]])))) {\n"),
])

# Fix Gerudo Warrior's Clothing Colors: a guerreira Gerudo nao muda a cor da roupa (bug corrigido no PAL 1.1)
patch_file("src/overlays/actors/ovl_En_Ge3/z_en_ge3.c", [
    ("SoH: roupa da gerudo (inicio)",
     "            rot->x += this->headRot.y;\n#if OOT_VERSION >= PAL_1_1\n            FALLTHROUGH;\n        default:\n",
     "            rot->x += this->headRot.y;\n            FALLTHROUGH;\n        default:\n"
     "            if (" + O("fix_gerudo_warrior_clothes") + ") { /* port: SoH GerudoWarriorClothingFix (como no PAL 1.1) */\n"),
    ("SoH: roupa da gerudo (fim)",
     "            CLOSE_DISPS(play->state.gfxCtx, \"../z_en_ge3.c\", 566);\n#endif\n            break;\n",
     "            CLOSE_DISPS(play->state.gfxCtx, \"../z_en_ge3.c\", 566);\n            }\n            break;\n"),
])

# Fix Link's Eyes Open while Sleeping (cenas do comeco do jogo)
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: olhos fechados dormindo",
     "        csAction = sCueToCsActionMap[this->cueId];\n        func_80852B4C(play, this, cue, &D_80854E50[ABS(csAction)]);\n",
     "        csAction = sCueToCsActionMap[this->cueId];\n        func_80852B4C(play, this, cue, &D_80854E50[ABS(csAction)]);\n"
     "        if (" + O("fix_eyes_open_sleeping") + " && (cue->id == 28 || cue->id == 29)) { /* port: SoH FixEyesOpenWhileSleeping */\n"
     "            this->skelAnime.jointTable[22].x = 8;\n        }\n"),
])

# Fix Missing Jingle after 5 Silver Rupees: a escala continua ate a 10a rupia prateada
patch_file("src/overlays/actors/ovl_En_G_Switch/z_en_g_switch.c", [
    ("SoH: escala das rupias prateadas", "    static s8 majorScale[] = { 0, 2, 4, 5, 7 };\n",
     "    static s8 majorScale[] = { 0, 2, 4, 5, 7, 9, 11, 13, 15, 17 }; /* port: SoH SilverRupeeJingleExtend */\n"),
    ("SoH: escala ate 10", "        if (sCollectedCount < 5) {\n",
     "        if (sCollectedCount < (" + O("fix_silver_rupee_jingle") + " ? 10 : 5)) { /* port */\n"),
])

# Fix Darunia Dancing too Fast: a danca do Darunia no ritmo da musica (animacoes a ~0,78x)
patch_file("src/overlays/actors/ovl_En_Du/z_en_du.c", [
    ("SoH: danca do darunia (tabela)",
     "    { &gDaruniaDancingEndAnim, 1.0f, 0.0f, -1.0f, ANIMMODE_ONCE, -6.0f },\n};\n",
     "    { &gDaruniaDancingEndAnim, 1.0f, 0.0f, -1.0f, ANIMMODE_ONCE, -6.0f },\n};\n\n"
     "/* port: SoH FixDaruniaDanceSpeed */\n"
     "static AnimationInfo sPortAnimationInfoFix[] = {\n"
     "    { NULL }, { NULL }, { NULL }, { NULL }, { NULL }, { NULL }, { NULL },\n"
     "    { &gDaruniaDancingLoop1Anim, 0.78f, 0.0f, -1.0f, ANIMMODE_ONCE, -10.0f },\n"
     "    { &gDaruniaDancingLoop1Anim, 0.77f, 0.0f, -1.0f, ANIMMODE_ONCE, 0.0f },\n"
     "    { &gDaruniaDancingLoop2Anim, 0.78f, 0.0f, -1.0f, ANIMMODE_ONCE, 0.0f },\n"
     "    { &gDaruniaDancingLoop3Anim, 0.77f, 0.0f, -1.0f, ANIMMODE_ONCE, 0.0f },\n"
     "    { NULL }, { NULL },\n"
     "    { &gDaruniaDancingLoop4Anim, 0.78f, 0.0f, -1.0f, ANIMMODE_ONCE, 0.0f },\n"
     "    { NULL },\n"
     "};\n"),
    ("SoH: danca do darunia 1",
     "            this->unk_1E6 = 0;\n        }\n        Animation_ChangeByInfo(&this->skelAnime, sAnimationInfo, animationIndices[this->unk_1E6]);\n",
     "            this->unk_1E6 = 0;\n        }\n        Animation_ChangeByInfo(&this->skelAnime, " + O("fix_darunia_dance") + " ? sPortAnimationInfoFix : sAnimationInfo,\n"
     "                               animationIndices[this->unk_1E6]); /* port */\n"),
    ("SoH: danca do darunia 2",
     "                Animation_ChangeByInfo(&this->skelAnime, sAnimationInfo, animationIndices[this->unk_1E6]);\n",
     "                Animation_ChangeByInfo(&this->skelAnime,\n"
     "                                       (" + O("fix_darunia_dance") + " && this->unk_1E6 <= 1) ? sPortAnimationInfoFix : sAnimationInfo,\n"
     "                                       animationIndices[this->unk_1E6]); /* port */\n"),
    ("SoH: danca do darunia 3",
     "Animation_ChangeByInfo(&this->skelAnime, sAnimationInfo, ENDU_ANIM_7);\n",
     "Animation_ChangeByInfo(&this->skelAnime, " + O("fix_darunia_dance") + " ? sPortAnimationInfoFix : sAnimationInfo, ENDU_ANIM_7); /* port */\n"),
])

# Red Ganon Blood: sangue vermelho no Ganondorf/Ganon (como no N64 antes do PAL; depois ficou verde)
patch_file("src/overlays/actors/ovl_Boss_Ganon/z_boss_ganon.c", [
    ("SoH: sangue vermelho (ganondorf)",
     "                    static Color_RGBA8 bloodPrimColor = { 0, 120, 0, 255 };\n"
     "                    static Color_RGBA8 bloodEnvColor = { 0, 120, 0, 255 };\n#endif\n",
     "                    static Color_RGBA8 bloodPrimColor = { 0, 120, 0, 255 };\n"
     "                    static Color_RGBA8 bloodEnvColor = { 0, 120, 0, 255 };\n#endif\n"
     "                    if (" + O("red_ganon_blood") + ") { /* port: SoH RedGanonBlood */\n"
     "                        bloodPrimColor.r = bloodEnvColor.r = 120;\n"
     "                        bloodPrimColor.g = bloodEnvColor.g = 0;\n"
     "                    }\n"),
])
patch_file("src/overlays/actors/ovl_Boss_Ganon2/z_boss_ganon2.c", [
    ("SoH: sangue vermelho (ganon)",
     "    static Color_RGBA8 sPrimColor = { 0, 120, 0, 255 };\n    static Color_RGBA8 sEnvColor = { 0, 120, 0, 255 };\n#endif\n"
     "    Vec3f pos;\n",
     "    static Color_RGBA8 sPrimColor = { 0, 120, 0, 255 };\n    static Color_RGBA8 sEnvColor = { 0, 120, 0, 255 };\n#endif\n"
     "    Vec3f pos;\n\n"
     "    if (" + O("red_ganon_blood") + ") { /* port: SoH RedGanonBlood */\n"
     "        sPrimColor.r = sEnvColor.r = 120;\n        sPrimColor.g = sEnvColor.g = 0;\n    }\n"),
])

# Pulsate Boss Icon: o icone do chefe pulsando tambem nas salas dos chefes (mapa da pausa)
patch_file("src/overlays/misc/ovl_kaleido_scope/z_lmap_mark.c", [
    ("SoH: icone do chefe",
     "        case SCENE_ICE_CAVERN:\n            PauseMapMark_DrawForDungeon(play);\n            break;\n    }\n",
     "        case SCENE_ICE_CAVERN:\n            PauseMapMark_DrawForDungeon(play);\n            break;\n"
     "        default: /* port: SoH PulsateBossIcon */\n"
     "            if (" + O("pulsate_boss_icon") + " && play->sceneId >= SCENE_DEKU_TREE_BOSS &&\n"
     "                play->sceneId <= SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR) {\n"
     "                PauseMapMark_DrawForDungeon(play);\n"
     "            }\n"
     "            break;\n    }\n"),
])

# Fix the Gravedigging Tour Glitch: o premio do Dampe vira pedaco de coracao enquanto o pedaco nao foi pego (a
# bandeira do jogo marcava "ganho" mesmo sem pegar)
patch_file("src/overlays/actors/ovl_En_Tk/z_en_tk.c", [
    # o pedaco de coracao do premio nasce com a bandeira de coleta 0x19 (como no SoH): pego, a bandeira fica marcada e
    # o teste abaixo passa a dar a rupia roxa. Sem ela o pedaco nunca marcava nada e saia de novo a cada escavacao.
    ("SoH: pedaco de coracao com bandeira",
     "        ITEM00_RUPEE_GREEN, ITEM00_RUPEE_BLUE, ITEM00_RUPEE_RED, ITEM00_RUPEE_PURPLE, ITEM00_HEART_PIECE,\n",
     "        ITEM00_RUPEE_GREEN, ITEM00_RUPEE_BLUE, ITEM00_RUPEE_RED, ITEM00_RUPEE_PURPLE,\n"
     "        ((0x19 & 0x3F) << 8) | ITEM00_HEART_PIECE, /* port: SoH (bandeira de coleta do pedaco do Dampe) */\n"),
    ("SoH: dampe e o pedaco de coracao",
     "                if (!GET_ITEMGETINF(ITEMGETINF_1C)) {\n                    SET_ITEMGETINF(ITEMGETINF_1C);\n",
     "                if (" + O("fix_gravedigging_tour") + " ? !Flags_GetCollectible(play, 0x19) /* port: SoH GravediggingTourFix */\n"
     "                                                  : !GET_ITEMGETINF(ITEMGETINF_1C)) {\n"
     "                    SET_ITEMGETINF(ITEMGETINF_1C);\n"),
])

# Fix Raised Floor Switches: dois interruptores de chao que ficavam 1 unidade acima do chao
patch_file("src/overlays/actors/ovl_Obj_Switch/z_obj_switch.c", [
    ("SoH: interruptores de chao",
     "        this->dyna.actor.world.pos.y = this->dyna.actor.home.pos.y + 1.0f;\n    }\n\n"
     "    Actor_SetFocus(&this->dyna.actor, sFocusHeights[type]);\n",
     "        this->dyna.actor.world.pos.y = this->dyna.actor.home.pos.y + 1.0f;\n    }\n"
     "    if (" + O("fix_floor_switches") + " && (this->dyna.actor.params == 0x1200 || this->dyna.actor.params == 0x3A00)) {\n"
     "        this->dyna.actor.world.pos.y -= 1; /* port: SoH FixFloorSwitches */\n    }\n\n"
     "    Actor_SetFocus(&this->dyna.actor, sFocusHeights[type]);\n"),
])

# Fix Deku Nut Upgrade: a serra do carpinteiro marcava a bandeira do upgrade de nozes do palco da floresta (ficava
# impossivel ganhar); e quem ja perdeu pode ganhar de novo no palco
patch_file("src/code/z_parameter.c", [
    ("SoH: serra nao marca o upgrade de nozes",
     "        if (item == ITEM_POACHERS_SAW) {\n            //! @bug Setting this shared flag makes getting the Deku Nut upgrade impossible\n",
     "        if (item == ITEM_POACHERS_SAW && !" + O("fix_deku_nut_upgrade") + ") { /* port: SoH DekuNutUpgradeFix */\n"
     "            //! @bug Setting this shared flag makes getting the Deku Nut upgrade impossible\n"),
])
patch_file("src/overlays/actors/ovl_En_Dnt_Demo/z_en_dnt_demo.c", [
    ("SoH: upgrade de nozes de volta",
     "                case PLAYER_MASK_TRUTH:\n"
     "                    if (!GET_ITEMGETINF(ITEMGETINF_FOREST_STAGE_NUT_UPGRADE) &&\n",
     "                case PLAYER_MASK_TRUTH:\n"
     "                    if (" + O("fix_deku_nut_upgrade") + " && Player_GetMask(play) != PLAYER_MASK_SKULL &&\n"
     "                        (((INV_CONTENT(ITEM_DEKU_NUT) == ITEM_DEKU_NUT) ? 1 : 0) +\n"
     "                         (GET_INFTABLE(INFTABLE_HAS_DEKU_NUT_UPGRADE) ? 1 : 0) +\n"
     "                         (GET_ITEMGETINF(ITEMGETINF_FOREST_STAGE_NUT_UPGRADE) ? 1 : 0)) != CUR_UPG_VALUE(UPG_DEKU_NUTS)) {\n"
     "                        /* port: SoH DekuNutUpgradeFix (a bandeira foi marcada pela serra) */\n"
     "                        gSaveContext.save.info.itemGetInf[ITEMGETINF_INDEX(ITEMGETINF_FOREST_STAGE_NUT_UPGRADE)] &=\n"
     "                            ~ITEMGETINF_MASK(ITEMGETINF_FOREST_STAGE_NUT_UPGRADE);\n"
     "                    }\n"
     "                    if (!GET_ITEMGETINF(ITEMGETINF_FOREST_STAGE_NUT_UPGRADE) &&\n"),
])

# Fix Broken Giant's Knife Bug: com a Biggoron's Sword, a faca quebrada sai do inventario tambem se faltar a espada
# Kokiri ou a Mestra (o original so tirava com as 4 espadas)
patch_file("src/code/z_parameter.c", [
    ("SoH: faca quebrada",
     "            if (ALL_EQUIP_VALUE(EQUIP_TYPE_SWORD) ==\n"
     "                ((1 << EQUIP_INV_SWORD_KOKIRI) | (1 << EQUIP_INV_SWORD_MASTER) | (1 << EQUIP_INV_SWORD_BIGGORON) |\n",
     "            if ((ALL_EQUIP_VALUE(EQUIP_TYPE_SWORD) | /* port: SoH FixBrokenGiantsKnife */\n"
     "                 (" + O("fix_broken_giants_knife") + " ? ((1 << EQUIP_INV_SWORD_KOKIRI) | (1 << EQUIP_INV_SWORD_MASTER)) : 0)) ==\n"
     "                ((1 << EQUIP_INV_SWORD_KOKIRI) | (1 << EQUIP_INV_SWORD_MASTER) | (1 << EQUIP_INV_SWORD_BIGGORON) |\n"),
])

PORT_VINE = r'''
/* port: SoH FixVineFall - confere com um segundo raio (com folga) se a parede escalavel continua valida, para o
 * Link nao comecar a escalar e cair na hora perto da beirada */
static void Port_RevalidateClimbedWall(PlayState* play, Player* this, u32* touchedWallFlags, s16* yawDiff) {
    if ((*touchedWallFlags & WALL_FLAG_3) && ({ extern int port_opt_fix_vine_fall; port_opt_fix_vine_fall; })) {
        Vec3f checkPosA;
        Vec3f checkPosB;
        Vec3f raycastResult;
        CollisionPoly* wallPoly;
        s32 wallBgId;
        f32 yawCos = Math_CosS(this->actor.wallYaw - (*yawDiff / 2) + 0x8000);
        f32 yawSin = Math_SinS(this->actor.wallYaw - (*yawDiff / 2) + 0x8000);

        checkPosA.x = this->actor.world.pos.x + (-20.0f * yawSin);
        checkPosA.z = this->actor.world.pos.z + (-20.0f * yawCos);
        checkPosB.x = this->actor.world.pos.x + (50.0f * yawSin);
        checkPosB.z = this->actor.world.pos.z + (50.0f * yawCos);
        checkPosB.y = checkPosA.y = this->actor.world.pos.y + 26.0f;
        if (BgCheck_EntityLineTest1(&play->colCtx, &checkPosA, &checkPosB, &raycastResult, &wallPoly, true, false,
                                    false, true, &wallBgId)) {
            this->actor.wallPoly = wallPoly;
            this->actor.wallBgId = wallBgId;
            this->actor.wallYaw = Math_Atan2S(wallPoly->normal.z, wallPoly->normal.x);
            *yawDiff = this->actor.shape.rot.y - (s16)(this->actor.wallYaw + 0x8000);
            *touchedWallFlags = SurfaceType_GetWallFlags(&play->colCtx, this->actor.wallPoly, this->actor.wallBgId);
        }
    }
}

'''
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH: trepadeira (funcao)", "void Player_ProcessItemButtons(Player* this, PlayState* play) {\n",
     PORT_VINE + "void Player_ProcessItemButtons(Player* this, PlayState* play) {\n"),
    ("SoH: trepadeira sem cair",
     "        sTouchedWallFlags = SurfaceType_GetWallFlags(&play->colCtx, this->actor.wallPoly, this->actor.wallBgId);\n"
     "        sShapeYawToTouchedWall = ABS(yawDiff);\n",
     "        sTouchedWallFlags = SurfaceType_GetWallFlags(&play->colCtx, this->actor.wallPoly, this->actor.wallBgId);\n"
     "        Port_RevalidateClimbedWall(play, this, &sTouchedWallFlags, &yawDiff); /* port: SoH FixVineFall */\n"
     "        sShapeYawToTouchedWall = ABS(yawDiff);\n"),
    # Fix Two-Handed Idle Animations: com espada de duas maos (Biggoron) o Link nao faz a pose de ajeitar o escudo
    ("SoH: pose parado com espada de duas maos",
     "                    if (((commonType + FIDGET_SWORD_SWING != FIDGET_SWORD_SWING) &&\n"
     "                         (commonType + FIDGET_SWORD_SWING != FIDGET_ADJUST_SHIELD)) ||\n"
     "                        ((this->rightHandType == PLAYER_MODELTYPE_RH_SHIELD) &&\n"
     "                         ((commonType + FIDGET_SWORD_SWING == FIDGET_ADJUST_SHIELD) ||\n"
     "                          (Player_GetMeleeWeaponHeld2(this) != 0)))) {\n",
     "                    if (" + O("fix_two_handed_idle") + " ? /* port: SoH TwoHandedIdle */\n"
     "                        (((commonType + FIDGET_SWORD_SWING != FIDGET_SWORD_SWING) &&\n"
     "                          (commonType + FIDGET_SWORD_SWING != FIDGET_ADJUST_SHIELD)) ||\n"
     "                         ((this->rightHandType == PLAYER_MODELTYPE_RH_SHIELD) &&\n"
     "                          (commonType + FIDGET_SWORD_SWING == FIDGET_ADJUST_SHIELD)) ||\n"
     "                         ((commonType + FIDGET_SWORD_SWING == FIDGET_SWORD_SWING) &&\n"
     "                          (Player_GetMeleeWeaponHeld(this) != 0))) :\n"
     "                        (((commonType + FIDGET_SWORD_SWING != FIDGET_SWORD_SWING) &&\n"
     "                         (commonType + FIDGET_SWORD_SWING != FIDGET_ADJUST_SHIELD)) ||\n"
     "                        ((this->rightHandType == PLAYER_MODELTYPE_RH_SHIELD) &&\n"
     "                         ((commonType + FIDGET_SWORD_SWING == FIDGET_ADJUST_SHIELD) ||\n"
     "                          (Player_GetMeleeWeaponHeld2(this) != 0))))) {\n"),
])

# ---------------------------------------------------------------- Difficulty / Minigames
# Always Win Dampe Digging Game: toda escavacao do Dampe da o premio maior
patch_file("src/overlays/actors/ovl_En_Tk/z_en_tk.c", [
    ("SoH: dampe sempre acha",
     "        if (this->validDigHere == 1) {\n",
     "        if (" + O("cheat_dampe_always_win") + ") { /* port: SoH DampeWin */\n            this->validDigHere = 1;\n        }\n"
     "        if (this->validDigHere == 1) {\n"),
    ("SoH: dampe premio maior",
     "            this->currentReward = EnTk_ChooseReward(this);\n",
     "            this->currentReward = EnTk_ChooseReward(this);\n"
     "            if (" + O("cheat_dampe_always_win") + ") { /* port: SoH DampeWin */\n                this->currentReward = 3;\n            }\n"),
])

# Frogs' Ocarina Game > Modify Note Timer: N vezes mais tempo para cada nota no jogo dos sapos
patch_file("src/overlays/actors/ovl_En_Fr/z_en_fr.c", [
    ("SoH: sapos (1a nota)", "        this->frogSongTimer = 40;\n",
     "        this->frogSongTimer = 40 * (" + O("frogs_note_timer") + " > 1 ? " + O("frogs_note_timer") + " : 1); /* port: SoH FrogsModifyFailTime */\n"),
    ("SoH: sapos (notas)", "        this->frogSongTimer = sTimerFrogSong[index];\n",
     "        this->frogSongTimer = sTimerFrogSong[index] * (" + O("frogs_note_timer") + " > 1 ? " + O("frogs_note_timer") + " : 1); /* port */\n"),
])

# Lost Woods Ocarina Game: notas iniciais e de cada rodada do jogo da ocarina com os Skull Kids
patch_file("src/audio/game/general.c", [
    ("SoH: jogo da ocarina do bosque",
     "    sOcaMemoryGameEndPos = sOcaMemoryGameNumNotes[minigameRound];\n\n"
     "    for (i = 0; i < 3; i++) {\n",
     "    sOcaMemoryGameEndPos = sOcaMemoryGameNumNotes[minigameRound];\n"
     "    if (" + O("lost_woods_customize") + ") { /* port: SoH CustomizeOcarinaGame */\n"
     "        s32 portN = minigameRound == 0 ? " + O("lost_woods_round1_notes") + "\n"
     "                  : minigameRound == 1 ? " + O("lost_woods_round2_notes") + " : " + O("lost_woods_round3_notes") + ";\n"
     "        sOcaMemoryGameEndPos = CLAMP(portN, 1, 8);\n"
     "    }\n\n"
     "    for (i = 0; i < (" + O("lost_woods_customize") + " ? CLAMP(" + O("lost_woods_starting_notes") + ", 1, 8) : 3); i++) {\n"),
])

# Fishing > Customize Behavior: a mordida e garantida, o peixe nunca escapa e o peso minimo para o premio
PORT_FISH = r'''
/* port: SoH CustomizeFishing - comprimento minimo do premio a partir do peso em libras (formula do SoH) */
static f32 Port_FishMinLength(f32 vanilla) {
    s32 w = LINK_IS_ADULT ? ({ extern int port_opt_fishing_min_weight_adult; port_opt_fishing_min_weight_adult; })
                          : ({ extern int port_opt_fishing_min_weight_child; port_opt_fishing_min_weight_child; });
    if (w <= 0) {
        return vanilla;
    }
    return sqrtf((w - 0.5f) / 0.0036f);
}

'''
patch_file("src/overlays/actors/ovl_Fishing/z_fishing.c", [
    ("SoH: pescaria (funcao)", "void Fishing_Init(Actor* thisx, PlayState* play2) {\n",
     PORT_FISH + "void Fishing_Init(Actor* thisx, PlayState* play2) {\n"),
    ("SoH: recorde inicial (crianca)", "                sFishingRecordLength = 40.0f; // 6 lbs\n",
     "                sFishingRecordLength = " + O("fishing_min_weight_child") + " > 0 ? Port_FishMinLength(40.0f) : 40.0f; // 6 lbs (port)\n"),
    ("SoH: recorde inicial (adulto)", "                sFishingRecordLength = 45.0f; // 7 lbs\n",
     "                sFishingRecordLength = " + O("fishing_min_weight_adult") + " > 0 ? Port_FishMinLength(45.0f) : 45.0f; // 7 lbs (port)\n"),
    ("SoH: premio (crianca)",
     "                        if ((sFishingRecordLength >= 50.0f) && !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_CHILD)) {\n",
     "                        if ((sFishingRecordLength >= Port_FishMinLength(50.0f)) && !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_CHILD)) {\n"),
    ("SoH: premio (adulto)",
     "                        if ((sFishingRecordLength >= 60.0f) && !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_ADULT)) {\n",
     "                        if ((sFishingRecordLength >= Port_FishMinLength(60.0f)) && !(HIGH_SCORE(HS_FISHING) & HS_FISH_PRIZE_ADULT)) {\n"),
    ("SoH: mordida garantida",
     "            if (((this->timerArray[0] == 1) || (Rand_ZeroOne() < chance)) &&\n"
     "                (Rand_ZeroOne() < (this->perception * multiplier)))\n",
     "            if (" + O("fishing_guarantee_bite") + " || /* port: SoH GuaranteeFishingBite */\n"
     "                (((this->timerArray[0] == 1) || (Rand_ZeroOne() < chance)) &&\n"
     "                (Rand_ZeroOne() < (this->perception * multiplier))))\n"),
    ("SoH: peixe nao escapa",
     "                (((sLureTimer & 0x7F) == 0) && (Rand_ZeroOne() < 0.05f) && (sLureEquipped != FS_LURE_SINKING) &&\n",
     "                (!" + O("fishing_fish_never_escape") + " && /* port: SoH FishNeverEscape */\n"
     "                 ((sLureTimer & 0x7F) == 0) && (Rand_ZeroOne() < 0.05f) && (sLureEquipped != FS_LURE_SINKING) &&\n"),
])

# ---------------------------------------------------------------- Cheats (todos desligados por padrao)
C = lambda n: O("cheat_" + n)
PORT_CHEATS = r'''
/* port: cheats do SoH (secao Cheats do options.txt), aplicados a cada quadro da logica */
static void Port_Cheats(PlayState* this) {
    static s32 sPortFrozenTime = -1;
    Player* player = GET_PLAYER(this);

    if (C_(infinite_money)) {
        gSaveContext.save.info.playerData.rupees = CUR_CAPACITY(UPG_WALLET);
    }
    if (C_(infinite_health)) {
        gSaveContext.save.info.playerData.health = gSaveContext.save.info.playerData.healthCapacity;
    }
    if (C_(infinite_ammo)) {
        AMMO(ITEM_DEKU_STICK) = CUR_CAPACITY(UPG_DEKU_STICKS);
        AMMO(ITEM_DEKU_NUT) = CUR_CAPACITY(UPG_DEKU_NUTS);
        AMMO(ITEM_BOMB) = CUR_CAPACITY(UPG_BOMB_BAG);
        AMMO(ITEM_BOW) = CUR_CAPACITY(UPG_QUIVER);
        AMMO(ITEM_SLINGSHOT) = CUR_CAPACITY(UPG_BULLET_BAG);
        if (INV_CONTENT(ITEM_BOMBCHU) != ITEM_NONE) {
            AMMO(ITEM_BOMBCHU) = 50;
        }
    }
    if (C_(infinite_magic) && gSaveContext.save.info.playerData.magicLevel != 0) {
        gSaveContext.save.info.playerData.magic = gSaveContext.save.info.playerData.magicLevel * MAGIC_NORMAL_METER;
    }
    if (C_(infinite_nayrus_love)) {
        gSaveContext.nayrusLoveTimer = 0x44B;
    }
    if (C_(freeze_time)) {
        if (sPortFrozenTime < 0) {
            sPortFrozenTime = gSaveContext.save.dayTime;
        }
        gSaveContext.save.dayTime = sPortFrozenTime;
    } else {
        sPortFrozenTime = -1;
    }
    if (C_(no_item_restrictions)) {
        u8 portSuns = this->interfaceCtx.restrictions.sunsSong;
        memset(&this->interfaceCtx.restrictions, 0, sizeof(this->interfaceCtx.restrictions));
        this->interfaceCtx.restrictions.sunsSong = portSuns;
    }
    if (C_(disable_sandstorm) && this->sceneId == SCENE_HAUNTED_WASTELAND) {
        this->envCtx.sandstormState = SANDSTORM_OFF;
    }
    if (C_(moon_jump_on_l) && player != NULL && ({ extern int port_input_select_a; port_input_select_a; })) { /* SELECT+A */
        player->actor.velocity.y = 6.34375f;
    }
}

'''.replace("C_(", "PORT_CHEAT(")
PORT_CHEATS = "#define PORT_CHEAT(n) ({ extern int port_opt_cheat_##n; port_opt_cheat_##n; })\n" + PORT_CHEATS
patch_file("src/code/z_play.c", [
    ("cheats (funcao)", "void Play_Update(PlayState* this) {\n", PORT_CHEATS + "void Play_Update(PlayState* this) {\n"),
    ("cheats (chamada)",
     "void Play_Update(PlayState* this) {\n    Input* input = this->state.input;\n    s32 isPaused;\n    STACK_PAD(s32);\n",
     "void Play_Update(PlayState* this) {\n    Input* input = this->state.input;\n    s32 isPaused;\n    STACK_PAD(s32);\n\n"
     "    Port_Cheats(this); /* port: cheats do SoH */\n"
     "    { /* port: pulo manual - Link pode agir neste quadro (fora da pausa e sem caixa de texto) */\n"
     "        extern int port_jump_mode, port_jump_stamp, port_input_frames;\n"
     "        port_jump_mode = (this->pauseCtx.state == PAUSE_STATE_OFF) && (this->msgCtx.msgMode == MSGMODE_NONE) &&\n"
     "                         (this->csCtx.state == CS_STATE_IDLE) && !Player_InCsMode(this);\n"
     "        port_jump_stamp = port_input_frames;\n"
     "        { /* troca de cena em andamento (gfx_hd.c: so entao o desenho espera as texturas novas) */\n"
     "            extern int port_scene_transition;\n"
     "            port_scene_transition = (this->transitionMode != TRANS_MODE_OFF) || (this->transitionTrigger != TRANS_TRIGGER_OFF);\n"
     "        }\n"
     "        if (" + O("tot_medallion_colors") + ") { /* port: SoH ToTMedallionsColors */\n"
     "            Port_ToTMedallions(this);\n"
     "        }\n"
     "        if (" + O("autosave") + " > 0) { /* port: SoH Autosave */\n"
     "            Port_Autosave(this, 0);\n"
     "        }\n"
     "        { /* segurar B: caixa de texto aberta (e se e de escolha) */\n"
     "            extern int port_text_open, port_text_choice;\n"
     "            port_text_open = (this->msgCtx.msgMode != MSGMODE_NONE);\n"
     "            port_text_choice = (this->msgCtx.textboxEndType == TEXTBOX_ENDTYPE_2_CHOICE) ||\n"
     "                               (this->msgCtx.textboxEndType == TEXTBOX_ENDTYPE_3_CHOICE);\n"
     "        }\n"
     "    }\n"),
])
patch_file("src/overlays/actors/ovl_En_Bom/z_en_bom.c", [
    ("cheat: tempo das bombas", "    this->timer = 70;\n",
     "    this->timer = 70;\n"
     "    if (" + C("bomb_timer_percent") + " > 0 && " + C("bomb_timer_percent") + " != 100) { /* port: SoH BombTimerMultiplier */\n"
     "        this->timer = this->timer * " + C("bomb_timer_percent") + " / 100;\n    }\n"),
])
patch_file("src/overlays/actors/ovl_Arms_Hook/z_arms_hook.c", [
    ("cheat: alcance do gancho", "        s32 length = (player->heldItemAction == PLAYER_IA_HOOKSHOT) ? 13 : 26;\n",
     "        s32 length = ((player->heldItemAction == PLAYER_IA_HOOKSHOT) ? 13 : 26) *\n"
     "                     (" + C("hookshot_reach_percent") + " > 0 ? " + C("hookshot_reach_percent") + " : 100) / 100; /* port: SoH HookshotReachMultiplier */\n"),
])
patch_file("src/code/z_player_lib.c", [
    ("cheat: alcance do gancho (mira)",
     "                                                   (this->heldItemAction == PLAYER_IA_HOOKSHOT) ? 38600.0f : 77600.0f);\n",
     "                                                   ((this->heldItemAction == PLAYER_IA_HOOKSHOT) ? 38600.0f : 77600.0f) *\n"
     "                                                   (" + C("hookshot_reach_percent") + " > 0 ? " + C("hookshot_reach_percent") + " : 100) / 100.0f); /* port */\n"),
])
patch_file("src/overlays/actors/ovl_En_Rd/z_en_rd.c", [
    ("cheat: redead nao paralisa 1",
     "                if (!(this->rdFlags & 0x80)) {\n                    player->actor.freezeTimer = 40;\n",
     "                if (!(this->rdFlags & 0x80) && !" + C("no_redead_freeze") + ") { /* port: SoH NoRedeadFreeze */\n"
     "                    player->actor.freezeTimer = 40;\n"),
    ("cheat: redead nao paralisa 2",
     "        if (!(this->rdFlags & 0x80)) {\n            player->actor.freezeTimer = 60;\n",
     "        if (!(this->rdFlags & 0x80) && !" + C("no_redead_freeze") + ") { /* port */\n"
     "            player->actor.freezeTimer = 60;\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("cheat: escudo deku nao queima",
     "void func_8083819C(Player* this, PlayState* play) {\n    if (this->currentShield == PLAYER_SHIELD_DEKU) {\n",
     "void func_8083819C(Player* this, PlayState* play) {\n"
     "    if (this->currentShield == PLAYER_SHIELD_DEKU && !" + C("fireproof_deku_shield") + ") { /* port: SoH FireproofDekuShield */\n"),
])

# Cheats (2a parte)
PL = "src/overlays/actors/ovl_player_actor/z_player.c"
# Infinite Epona Boost: a cenoura nao e gasta (e os icones delas nao aparecem)
patch_file("src/overlays/actors/ovl_En_Horse/z_en_horse.c", [
    ("cheat: cenouras infinitas",
     "                    this->stateFlags |= ENHORSE_FLAG_8;\n                    this->numBoosts--;\n",
     "                    this->stateFlags |= ENHORSE_FLAG_8;\n"
     "                    if (!" + C("infinite_epona_boost") + ") { /* port: SoH InfiniteEponaBoost */\n"
     "                        this->numBoosts--;\n                    }\n"),
])
patch_file("src/code/z_parameter.c", [
    ("cheat: cenouras infinitas (icones)", "                if (interfaceCtx->unk_1EE == 8) {\n",
     "                if (interfaceCtx->unk_1EE == 8 && !" + C("infinite_epona_boost") + ") { /* port */\n"),
    ("cheat: super tunica (calor)",
     "        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_GORON) {\n            sEnvHazard = PLAYER_ENV_HAZARD_NONE;\n",
     "        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_GORON || " + C("super_tunic") + ") { /* port: SoH SuperTunic */\n"
     "            sEnvHazard = PLAYER_ENV_HAZARD_NONE;\n"),
    ("cheat: super tunica (agua)",
     "        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_ZORA) {\n            sEnvHazard = PLAYER_ENV_HAZARD_NONE;\n",
     "        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_ZORA || " + C("super_tunic") + ") { /* port */\n"
     "            sEnvHazard = PLAYER_ENV_HAZARD_NONE;\n"),
])
patch_file("src/code/z_player_lib.c", [
    ("cheat: super tunica (avisos)",
     "            (((envHazard == (PLAYER_ENV_HAZARD_HOTROOM - 1)) && (this->currentTunic != PLAYER_TUNIC_GORON)) ||\n",
     "            !" + C("super_tunic") + " && /* port: SoH SuperTunic */\n"
     "            (((envHazard == (PLAYER_ENV_HAZARD_HOTROOM - 1)) && (this->currentTunic != PLAYER_TUNIC_GORON)) ||\n"),
    ("cheat: escudo com arma de duas maos (modelo)",
     "        if (!Player_HoldsTwoHandedWeapon(this) && !Player_IsChildWithHylianShield(this)) {\n"
     "            this->rightHandType = PLAYER_MODELTYPE_RH_SHIELD;\n",
     "        if ((!Player_HoldsTwoHandedWeapon(this) || /* port: SoH ShieldTwoHanded */\n"
     "             (" + C("shield_two_handed") + " && this->heldItemAction != PLAYER_IA_DEKU_STICK)) &&\n"
     "            !Player_IsChildWithHylianShield(this)) {\n"
     "            this->rightHandType = PLAYER_MODELTYPE_RH_SHIELD;\n"),
])
TWO = "(Player_HoldsTwoHandedWeapon(this) && !(" + C("shield_two_handed") + " && this->heldItemAction != PLAYER_IA_DEKU_STICK))"
patch_file(PL, [
    ("cheat: super tunica (chao quente 1)",
     "                     ((this->currentTunic != PLAYER_TUNIC_GORON) || (this->floorTypeTimer >= D_808544F4[sp48])))) {\n",
     "                     ((this->currentTunic != PLAYER_TUNIC_GORON && !" + C("super_tunic") + ") || /* port */\n"
     "                      (this->floorTypeTimer >= D_808544F4[sp48])))) {\n"),
    ("cheat: super tunica (chao quente 2)",
     "void Player_Action_80843CEC(Player* this, PlayState* play) {\n    if (this->currentTunic != PLAYER_TUNIC_GORON) {\n",
     "void Player_Action_80843CEC(Player* this, PlayState* play) {\n"
     "    if (this->currentTunic != PLAYER_TUNIC_GORON && !" + C("super_tunic") + ") { /* port */\n"),
    ("cheat: super tunica (chao quente 3)",
     "    if (this->currentTunic == PLAYER_TUNIC_GORON) {\n        sp54 = 20;\n",
     "    if (this->currentTunic == PLAYER_TUNIC_GORON || " + C("super_tunic") + ") { /* port */\n        sp54 = 20;\n"),
    ("cheat: escudo com arma de duas maos 1", "        return D_808543A4[Player_HoldsTwoHandedWeapon(this)];\n",
     "        return D_808543A4[" + TWO + "]; /* port */\n"),
    ("cheat: escudo com arma de duas maos 2", "        return D_808543AC[Player_HoldsTwoHandedWeapon(this)];\n",
     "        return D_808543AC[" + TWO + "]; /* port */\n"),
    ("cheat: escudo com arma de duas maos 3", "anim = D_808543BC[Player_HoldsTwoHandedWeapon(this)];\n",
     "anim = D_808543BC[" + TWO + "]; /* port */\n"),
    ("cheat: escudo com arma de duas maos 4", "anim = D_808543B4[Player_HoldsTwoHandedWeapon(this)];\n",
     "anim = D_808543B4[" + TWO + "]; /* port */\n"),
    ("cheat: escudo com arma de duas maos 5", "Player_AnimPlayOnce(play, this, D_808543C4[Player_HoldsTwoHandedWeapon(this)]);\n",
     "Player_AnimPlayOnce(play, this, D_808543C4[" + TWO + "]); /* port */\n"),
    # Deku Stick: 1 = nao quebra; 2 = nao quebra e fica sempre em chamas (SoH DekuStick)
    ("cheat: graveto nao quebra", "        if (AMMO(ITEM_DEKU_STICK) != 0) {\n            EffectSsStick_Spawn(",
     "        if (AMMO(ITEM_DEKU_STICK) != 0 && " + C("deku_stick") + " == 0) { /* port: SoH DekuStick */\n            EffectSsStick_Spawn("),
    ("cheat: graveto nao apaga 1", "    if (this->unk_85C == 0.0f) {\n        Player_UseItem(play, this, ITEM_NONE);\n",
     "    if (this->unk_85C == 0.0f && " + C("deku_stick") + " == 0) { /* port */\n        Player_UseItem(play, this, ITEM_NONE);\n"),
    ("cheat: graveto nao apaga 2", "    if (DECR(this->unk_860) == 0) {\n        Inventory_ChangeAmmo(ITEM_DEKU_STICK, -1);\n",
     "    if (DECR(this->unk_860) == 0 && " + C("deku_stick") + " == 0) { /* port: nao queima ate o fim */\n"
     "        Inventory_ChangeAmmo(ITEM_DEKU_STICK, -1);\n"),
    ("cheat: graveto nao encolhe", "    } else if (this->unk_860 < 20) {\n        temp = this->unk_860 / 20.0f;\n",
     "    } else if (this->unk_860 < 20 && " + C("deku_stick") + " == 0) { /* port */\n        temp = this->unk_860 / 20.0f;\n"),
    ("cheat: graveto sempre em chamas",
     "    if ((this->heldItemAction == PLAYER_IA_DEKU_STICK) && (this->unk_860 != 0)) {\n        Player_UpdateBurningDekuStick(play, this);\n",
     "    if ((this->heldItemAction == PLAYER_IA_DEKU_STICK) && " + C("deku_stick") + " == 2) { /* port */\n"
     "        this->unk_860 = 200;\n        this->unk_85C = 1.0f;\n    }\n"
     "    if ((this->heldItemAction == PLAYER_IA_DEKU_STICK) && (this->unk_860 != 0)) {\n        Player_UpdateBurningDekuStick(play, this);\n"),
])
# Hookshot Everything: toda superficie aceita o gancho
patch_file("src/code/z_bgcheck.c", [
    ("cheat: gancho em tudo",
     "u32 SurfaceType_CanHookshot(CollisionContext* colCtx, CollisionPoly* poly, s32 bgId) {\n"
     "    return SurfaceType_GetData(colCtx, poly, bgId, 1) >> 17 & 1;\n",
     "u32 SurfaceType_CanHookshot(CollisionContext* colCtx, CollisionPoly* poly, s32 bgId) {\n"
     "    if (" + C("hookshot_everything") + ") { /* port: SoH HookshotEverything */\n        return 1;\n    }\n"
     "    return SurfaceType_GetData(colCtx, poly, bgId, 1) >> 17 & 1;\n"),
])
# Drops Don't Despawn / No Fish Despawn / No Bugs Despawn
patch_file("src/code/z_en_item00.c", [
    ("cheat: itens nao somem", "    if (this->despawnTimer > 0) {\n        this->despawnTimer--;\n",
     "    if (this->despawnTimer > 0 && !(" + C("drops_dont_despawn") + " && this->unk_154 <= 0)) { /* port: SoH DropsDontDie */\n"
     "        this->despawnTimer--;\n"),
])
patch_file("src/overlays/actors/ovl_En_Fish/z_en_fish.c", [
    ("cheat: peixes nao somem",
     "void EnFish_UpdateImpl(EnFish* this, PlayState* play) {\n    if (this->timer > 0) {\n",
     "void EnFish_UpdateImpl(EnFish* this, PlayState* play) {\n"
     "    if (this->timer > 0 && !" + C("no_fish_despawn") + ") { /* port: SoH NoFishDespawn */\n"),
])
patch_file("src/overlays/actors/ovl_En_Insect/z_en_insect.c", [
    ("cheat: insetos nao somem", "        EnInsect_SetupDropped(this);\n\n        sDroppedCount++;\n",
     "        EnInsect_SetupDropped(this);\n\n        sDroppedCount++;\n"
     "        if (" + C("no_bugs_despawn") + " && this->soilActor == NULL) { /* port: SoH NoBugsDespawn */\n"
     "            this->insectFlags &= ~INSECT_FLAG_IS_SHORT_LIVED;\n            sDroppedCount--;\n        }\n"),
])
# No Keese/Guay Target: morcegos e corvos nao atacam e voam embora
patch_file("src/overlays/actors/ovl_En_Firefly/z_en_firefly.c", [
    ("cheat: keese nao ataca",
     "    if ((this->timer == 0) && (this->actor.xzDistToPlayer < 200.0f) && (Player_GetMask(play) != PLAYER_MASK_SKULL)) {\n",
     "    if ((this->timer == 0) && (this->actor.xzDistToPlayer < 200.0f) && (Player_GetMask(play) != PLAYER_MASK_SKULL) &&\n"
     "        !" + C("no_keese_guay_target") + ") { /* port: SoH NoKeeseGuayTarget */\n"),
    ("cheat: keese voa embora", "    if ((this->timer == 0) || (Player_GetMask(play) == PLAYER_MASK_SKULL)) {\n",
     "    if ((this->timer == 0) || (Player_GetMask(play) == PLAYER_MASK_SKULL) || " + C("no_keese_guay_target") + ") { /* port */\n"),
])
patch_file("src/overlays/actors/ovl_En_Crow/z_en_crow.c", [
    ("cheat: guay nao ataca",
     "        (this->actor.depthInWater < -40.0f) && (Player_GetMask(play) != PLAYER_MASK_SKULL)) {\n",
     "        (this->actor.depthInWater < -40.0f) && (Player_GetMask(play) != PLAYER_MASK_SKULL) &&\n"
     "        !" + C("no_keese_guay_target") + ") { /* port */\n"),
    ("cheat: guay voa embora",
     "        (player->stateFlags1 & PLAYER_STATE1_23) || (this->actor.depthInWater > -40.0f)) {\n",
     "        (player->stateFlags1 & PLAYER_STATE1_23) || (this->actor.depthInWater > -40.0f) ||\n"
     "        " + C("no_keese_guay_target") + ") { /* port */\n"),
])
# GS Targetable: Skulltulas douradas visiveis podem ser miradas com Z (cursor verde, categoria Misc)
patch_file("src/code/z_play.c", [
    ("cheat: skulltula dourada miravel",
     "    Port_Cheats(this); /* port: cheats do SoH */\n",
     "    Port_Cheats(this); /* port: cheats do SoH */\n"
     "    if (" + C("gs_targetable") + ") { /* port: SoH GSTargetable */\n"
     "        Actor* portA = this->actorCtx.actorLists[ACTORCAT_NPC].head;\n"
     "        while (portA != NULL) {\n"
     "            Actor* portNext = portA->next;\n"
     "            if (portA->id == ACTOR_EN_SW && portA->naviEnemyId == 0x20) {\n"
     "                Actor_ChangeCategory(this, &this->actorCtx, portA, ACTORCAT_MISC);\n"
     "            }\n"
     "            portA = portNext;\n"
     "        }\n"
     "        for (portA = this->actorCtx.actorLists[ACTORCAT_MISC].head; portA != NULL; portA = portA->next) {\n"
     "            if (portA->id == ACTOR_EN_SW && portA->naviEnemyId == 0x20) {\n"
     "                if (portA->colChkInfo.health > 0 && portA->scale.x >= 0.0139999995f) {\n"
     "                    portA->flags |= ACTOR_FLAG_ATTENTION_ENABLED;\n"
     "                } else {\n"
     "                    portA->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;\n"
     "                }\n"
     "            }\n"
     "        }\n"
     "    }\n"),
])

# port: texturas novas so nos fundos pre-renderizados (opcao hd_textures 2, gfx_hd.c): marcas 0x54585441/42 em volta
# do desenho da "caixa do ceu", que nas casas, lojas e becos e o proprio ambiente pintado (vr_*VR_static)
patch_file("src/code/z_vr_box_draw.c", [
    ("fundos pre-renderizados: marca do comeco",
     "    Port_FiOpenChild(NULL, Port_FiCameraEpoch()); /* port */\n",
     "    Port_FiOpenChild(NULL, Port_FiCameraEpoch()); /* port */\n"
     "    POLY_OPA_DISP->words.w0 = 0; /* port: marca para gfx_hd.c */\n"
     "    POLY_OPA_DISP->words.w1 = 0x54585441;\n    POLY_OPA_DISP++;\n"),
    ("fundos pre-renderizados: marca do fim",
     "    Port_FiCloseChild(); /* port */\n",
     "    POLY_OPA_DISP->words.w0 = 0; /* port */\n"
     "    POLY_OPA_DISP->words.w1 = 0x54585442;\n    POLY_OPA_DISP++;\n"
     "    Port_FiCloseChild(); /* port */\n"),
])

# port: hd_textures 2 na logo inteira da abertura (o fogo atras do escudo tambem: gfx_hd.c), marcas 0x54585443/44
patch_file("src/overlays/actors/ovl_En_Mag/z_en_mag.c", [
    ("logo inteira: marcas para as texturas novas",
     "    EnMag_DrawInner(thisx, play, &gfx);\n",
     "    gfx->words.w0 = 0; /* port: marca para gfx_hd.c */\n    gfx->words.w1 = 0x54585443;\n    gfx++;\n"
     "    EnMag_DrawInner(thisx, play, &gfx);\n"
     "    gfx->words.w0 = 0; /* port */\n    gfx->words.w1 = 0x54585444;\n    gfx++;\n"),
])

# Better Bombchu Shopping (SoH): bombchus nas lojas nunca esgotam (os pacotes nao viram "esgotado" depois de comprados)
# e o pacote de 10 custa 99 rupias (da para comprar com a carteira inicial). O texto da loja muda junto (Port_MsgInject).
def _girla():
    reps = [("SoH: bombchus custam 99",
             "        this->basePrice = itemEntry->price;\n",
             "        this->basePrice = (" + O("better_bombchu_shopping") + " && this->getItemId == GI_BOMBCHUS_10) ? 99 : itemEntry->price; /* port: SoH */\n")]
    for f in ("03", "04", "05", "06", "07", "08", "09", "0A"):
        reps.append(("SoH: bombchus nao esgotam (" + f + ")",
                     "            if (GET_ITEMGETINF(ITEMGETINF_" + f + ")) {\n",
                     "            if (!" + O("better_bombchu_shopping") + " && GET_ITEMGETINF(ITEMGETINF_" + f + ")) {\n"))
        reps.append(("SoH: bombchus nao marcam esgotado (" + f + ")",
                     "            SET_ITEMGETINF(ITEMGETINF_" + f + ");\n",
                     "            if (!" + O("better_bombchu_shopping") + ") {\n                SET_ITEMGETINF(ITEMGETINF_" + f + ");\n            }\n"))
    patch_file("src/overlays/actors/ovl_En_GirlA/z_en_girla.c", reps)
_girla()

# Temple of Time Medallion Colors (SoH ToTMedallionsColors): no chao do Templo do Tempo, os medalhoes que voce ja tem
# ficam coloridos. O SoH liga o modo cinza da GPU (gsSPGrayscale) nesses desenhos; aqui o mesmo resultado sai de graca
# no desenho: as 12 texturas (RGBA16 64x32, duas metades por medalhao, no arquivo tokinoma_room_0) sao pintadas na
# memoria da sala (media dos canais x cor do medalhao, a mesma conta do SoH); a cache das texturas ve o conteudo novo.
patch_file("src/code/z_play.c", [
    ("SoH: medalhoes coloridos no Templo do Tempo (funcao)",
     "void Play_Update(PlayState* this) {\n",
     """static void Port_ToTMedallions(PlayState* this) { /* port */
    static const u8 kCol[6][3] = { /* agua, espirito, luz, floresta, fogo, sombra (cores do SoH) */
        { 0, 161, 255 }, { 255, 135, 0 }, { 255, 255, 0 }, { 0, 255, 0 }, { 255, 0, 0 }, { 212, 0, 255 },
    };
    static const u8 kQuest[6] = { QUEST_MEDALLION_WATER, QUEST_MEDALLION_SPIRIT, QUEST_MEDALLION_LIGHT,
                                  QUEST_MEDALLION_FOREST, QUEST_MEDALLION_FIRE, QUEST_MEDALLION_SHADOW };
    static u16 sOrig[12][64 * 32];
    static u8* sSeg;
    static u16 sDone; /* bit 15 = copia feita; bits 0-5 = medalhoes pintados */
    static u16 sMark; /* pixel 0 da primeira textura depois de pintar (sala recarregada = volta o original) */
    u8* seg;
    u16 want = 0x8000;
    s32 m, k, t, i;

    if (this->sceneId != SCENE_TEMPLE_OF_TIME || this->roomCtx.curRoom.num != 0 || this->roomCtx.curRoom.segment == NULL) {
        return;
    }
    seg = (u8*)this->roomCtx.curRoom.segment;
    /* confere a lista de desenho dos medalhoes (0x7A70; palavras ja na ordem do ARM): pipesync, geometria e a 1a
     * textura em 0x030131D8 (a imagem fica nos bytes do N64) */
    if (*(u32*)(seg + 0x7A70) != 0xE7000000 || *(u32*)(seg + 0x7A78) != 0xD9FCFFFF || *(u32*)(seg + 0x7AB0) != 0xFD100000 ||
        *(u32*)(seg + 0x7AB4) != 0x030131D8 || *(u32*)(seg + 0x8014) != 0x0300E1D8) {
        return;
    }
    for (m = 0; m < 6; m++) {
        if (CHECK_QUEST_ITEM(kQuest[m])) {
            want |= 1 << m;
        }
    }
    if (seg != sSeg || !(sDone & 0x8000) || *(u16*)(seg + 0x131D8) != sMark) {
        for (t = 0; t < 12; t++) { /* sala nova (ou recarregada): guarda o original */
            u16* src = (u16*)(seg + 0x131D8 - 0x1000 * t);
            for (i = 0; i < 64 * 32; i++) {
                sOrig[t][i] = src[i];
            }
        }
        sSeg = seg;
        sDone = 0x8000;
        sMark = *(u16*)(seg + 0x131D8);
    }
    if (want == sDone) {
        return;
    }
    for (m = 0; m < 6; m++) {
        for (k = 0; k < 2; k++) {
            t = m * 2 + k;
            u16* dst = (u16*)(seg + 0x131D8 - 0x1000 * t);
            for (i = 0; i < 64 * 32; i++) {
                u16 p = sOrig[t][i];
                if (want & (1 << m)) {
                    /* textura em big-endian na memoria: RRRRRGGGGGBBBBBA */
                    u16 q = (u16)((p >> 8) | (p << 8));
                    s32 v = (((q >> 11) & 31) + ((q >> 6) & 31) + ((q >> 1) & 31)) / 3;
                    s32 r = v * kCol[m][0] / 255, g = v * kCol[m][1] / 255, b = v * kCol[m][2] / 255;
                    q = (u16)((r << 11) | (g << 6) | (b << 1) | (q & 1));
                    p = (u16)((q >> 8) | (q << 8));
                }
                dst[i] = p;
            }
        }
    }
    sDone = want;
    sMark = *(u16*)(seg + 0x131D8);
}

void Play_Update(PlayState* this) {
"""),
])

# ---------------------------------------------------------------- port: estado rapido (quick save / quick load)
# MENU+R2 salva, MENU+L2 carrega (port/savestate.c copia a RAM e os dados do jogo). A copia e feita na thread
# grafica entre dois quadros: as variaveis locais do Graph_ThreadEntry viram globais (entram na copia; o laco
# continua no mesmo ponto com o estado que voltou), o desenho pendente termina antes (Port_GfxIdle), a mensagem
# "tarefa do desenho acabou" e tirada da fila e devolvida depois, e nada e feito com leitura da ROM em andamento
# (Port_DmaIdle) ou, ao salvar, durante troca de cena/sala. Ao carregar: tabela de mensagens do idioma (memoria do
# port) e o som (Port_SsAudioRestore) sao acertados.
patch_file("src/code/graph.c", [
    ("estado rapido: CfbInfo do escalonador fora da funcao (ficam como estao ao carregar)",
     "        static CfbInfo sGraphCfbInfos[3];\n        static s32 sGraphCfbInfoIdx = 0;\n", ""),
    ("estado rapido: CfbInfo do escalonador fora da funcao (declaracao)",
     "void Graph_TaskSet00(GraphicsContext* gfxCtx) {\n",
     "/* port: eram static dentro de Graph_TaskSet00; o escalonador guarda ponteiros para eles (troca de framebuffer), entao\n"
     " * o estado rapido nao os troca (Port_SsFrame) */\n"
     "static CfbInfo sGraphCfbInfos[3];\nstatic s32 sGraphCfbInfoIdx = 0;\n\n"
     "void Graph_TaskSet00(GraphicsContext* gfxCtx) {\n"),
    ("estado rapido: Graph_ThreadEntry com variaveis globais e o ponto da copia",
     "void Graph_ThreadEntry(UNUSED void* arg) {\n    GraphicsContext gfxCtx;\n    GameState* gameState;\n    u32 size;\n"
     "    GameStateOverlay* nextOvl = &gGameStateOverlayTable[GAMESTATE_SETUP];\n    GameStateOverlay* ovl;\n",
     """/* port: estado rapido (port/savestate.c) */
GraphicsContext gPortGfxCtx;
GameState* gPortGameState;
u32 gPortStateSize;
GameStateOverlay* gPortNextOvl;
GameStateOverlay* gPortOvl;
int port_ss_want(int isPlay, int isSetup);
void port_ss_defer(int op);
int port_ss_save(u16 seqMain, u16 seqSub);
int port_ss_load(int which, u16* seqMain, u16* seqSub);
void port_ss_exit(u16 seqMain, u16 seqSub);
void Port_GfxIdle(void);
s32 Port_DmaIdle(void);
void Port_LangRelink(void);
void Port_SsAudioIds(u16* seqMain, u16* seqSub);
void Port_SsAudioRestore(u16 seqMain, u16 seqSub, u8 natureAmbienceId);
s32 Port_AudioPark(void);
void Port_AudioUnpark(void);
void Port_FiDontInterpolateFrame(void);

/* menu de opcoes no jogo (port/miyoo/menu_miyoo.c, MENU+START): o jogo para enquanto ele esta aberto (o desenho
 * pendente termina antes; a thread dos controles, o som e a tela continuam). Um pedido de fechar (desligar...) fecha o
 * menu e o jogo segue para o Port_SsFrame. */
int miyoo_menu_active(void) __attribute__((weak));
void miyoo_menu_close_now(void) __attribute__((weak));
int miyoo_menu_glyphs_wanted(void) __attribute__((weak));
void miyoo_menu_load_glyphs(void (*load)(u16 sjis, void* out)) __attribute__((weak));
int port_ss_exit_pending(void);
int port_ss_menu_op(void);
void port_ss_slot_not_play(void);
int port_ss_save_slot(u16 seqMain, u16 seqSub);

static s32 Port_SsRun(s32 op);

static void Port_MenuFreeze(void) {
    OSMesgQueue queue;
    OSMesg msgBuf;
    OSTimer timer;
    s32 op;

    if (miyoo_menu_active == NULL || !miyoo_menu_active() || gPortOvl == &gGameStateOverlayTable[GAMESTATE_SETUP]) {
        return;
    }
    Port_GfxIdle();
    osCreateMesgQueue(&queue, &msgBuf, 1);
    while (miyoo_menu_active()) {
        if (miyoo_menu_glyphs_wanted != NULL && miyoo_menu_glyphs_wanted()) {
            void Port_KanjiGlyph(u16 sjis, void* out); /* z_kanfont.c */

            miyoo_menu_load_glyphs(Port_KanjiGlyph); /* menu em japones: as letras vem da fonte do jogo */
        }
        if (port_ss_exit_pending()) {
            miyoo_menu_close_now();
            break;
        }
        op = port_ss_menu_op(); /* salvar/carregar um dos 10 estados (aba Estados do menu) */
        if (op != 0 && Port_SsRun(op)) {
            continue; /* (nao deu ainda: espera um passo e tenta de novo) */
        }
        osSetTimer(&timer, OS_USEC_TO_CYCLES(33333), 0, &queue, NULL);
        osRecvMesg(&queue, NULL, OS_MESG_BLOCK);
    }
    Port_FiDontInterpolateFrame(); /* (o primeiro quadro depois do menu nao mistura com o de antes) */
}

/* port/savestate.c: 1 salvar o estado rapido, 2 carregar, 3 fechar (salvando o estado), 4 continuar de onde parou,
 * 5 salvar num dos 10 estados do menu, 6 carregar um deles */
static void Port_SsFrame(void) {
    s32 op;

    if (miyoo_menu_glyphs_wanted != NULL && gPortOvl != &gGameStateOverlayTable[GAMESTATE_SETUP] &&
        miyoo_menu_glyphs_wanted()) {
        void Port_KanjiGlyph(u16 sjis, void* out); /* z_kanfont.c */

        miyoo_menu_load_glyphs(Port_KanjiGlyph); /* avisos em japones: as letras vem da fonte do jogo */
    }
    Port_MenuFreeze();
    op = port_ss_want(gPortOvl == &gGameStateOverlayTable[GAMESTATE_PLAY],
                      gPortOvl == &gGameStateOverlayTable[GAMESTATE_SETUP]);
    if (op != 0) {
        Port_SsRun(op);
    }
}

/* faz a operacao no ponto seguro; 0 = ainda nao deu (tenta de novo no proximo quadro / passo do menu) */
static s32 Port_SsRun(s32 op) {
    s32 isPlay = gPortOvl == &gGameStateOverlayTable[GAMESTATE_PLAY];
    s32 save = (op == 1) || (op == 3) || (op == 5);
    s32 ready;
    OSMesg msg;
    u16 seqMain;
    u16 seqSub;

    if (op == 5 && !isPlay) {
        port_ss_slot_not_play();
        return 1;
    }
    Port_GfxIdle(); /* o desenho pendente termina (le a RAM) */
    osRecvMesg(&gPortGfxCtx.queue, &msg, OS_MESG_BLOCK); /* "tarefa acabou" do ultimo quadro */
    ready = Port_DmaIdle();
    if (ready && save) {
        PlayState* play = (PlayState*)gPortGameState;

        if ((play->transitionTrigger != TRANS_TRIGGER_OFF) || (play->transitionMode != TRANS_MODE_OFF) ||
            (play->roomCtx.status != 0)) {
            ready = false;
        }
    }
    if (!ready) {
        osSendMesg(&gPortGfxCtx.queue, msg, OS_MESG_NOBLOCK);
        port_ss_defer(op);
        return 0;
    }
    if (save) {
        s32 portParked = Port_AudioPark(); /* port: o som para num ponto conhecido e entra no estado */

        Port_SsAudioIds(&seqMain, &seqSub);
        if (op == 3) {
            extern int port_opt_autosave;
            void Port_Autosave(PlayState* play, s32 now);

            if (port_opt_autosave > 0) {
                Port_Autosave((PlayState*)gPortGameState, true); /* port: SoH Autosave (ao fechar) */
            }
            port_ss_exit(seqMain, seqSub); /* (nao volta) */
        }
        if (op == 5) {
            port_ss_save_slot(seqMain, seqSub);
        } else {
            port_ss_save(seqMain, seqSub);
        }
        if (portParked) {
            Port_AudioUnpark();
        }
        osSendMesg(&gPortGfxCtx.queue, msg, OS_MESG_NOBLOCK);
    } else {
        /* o que o escalonador (fora do estado) acompanha fica como esta: qual framebuffer esta na tela / esperando a
         * troca (sGraphCfbInfos, fbIdx) e qual pool de comandos esta livre. Voltar isso para o salvo fazia o proximo
         * quadro desenhar no framebuffer que espera a troca: o escalonador segurava a tarefa e o jogo parava. */
        CfbInfo cfbKeep[ARRAY_COUNT(sGraphCfbInfos)];
        s32 cfbIdxKeep = sGraphCfbInfoIdx;
        s32 fbIdxKeep = gPortGfxCtx.fbIdx;
        u32 poolKeep = gPortGfxCtx.gfxPoolIdx;
        s32 loaded;

        s32 portParked;

        __builtin_memcpy(cfbKeep, sGraphCfbInfos, sizeof(cfbKeep));
        portParked = Port_AudioPark(); /* port: o som do estado entra com o som parado */
        loaded = port_ss_load(op == 4 ? 1 : (op == 6 ? 2 : 0), &seqMain, &seqSub) == 0;
        if (portParked) {
            Port_AudioUnpark(); /* (a musica continua de onde estava no estado; se nao carregou, de onde estava) */
        }
        __builtin_memcpy(sGraphCfbInfos, cfbKeep, sizeof(cfbKeep));
        sGraphCfbInfoIdx = cfbIdxKeep;
        gPortGfxCtx.fbIdx = fbIdxKeep;
        gPortGfxCtx.gfxPoolIdx = poolKeep;
        if (!loaded) {
            osSendMesg(&gPortGfxCtx.queue, msg, OS_MESG_NOBLOCK);
            return 1;
        }
        osCreateMesgQueue(&gPortGfxCtx.queue, gPortGfxCtx.msgBuff, ARRAY_COUNT(gPortGfxCtx.msgBuff));
        osSendMesg(&gPortGfxCtx.queue, NULL, OS_MESG_NOBLOCK);
        Port_LangRelink();
        { /* o idioma desta execucao (o estado guarda o de quando foi salvo) */
            extern int port_lang_game;

            gSaveContext.language = (port_lang_game == 2) ? LANGUAGE_JPN : LANGUAGE_ENG;
        }
        if (!portParked) {
            Port_SsAudioRestore(seqMain, seqSub, gSaveContext.natureAmbienceId); /* (o som nao parou: o jeito antigo) */
        }
        Port_FiDontInterpolateFrame();
    }
    return 1;
}

#define gfxCtx gPortGfxCtx
#define gameState gPortGameState
#define size gPortStateSize
#define nextOvl gPortNextOvl
#define ovl gPortOvl
void Graph_ThreadEntry(UNUSED void* arg) {
    nextOvl = &gGameStateOverlayTable[GAMESTATE_SETUP];
"""),
    ("estado rapido: ponto da copia no laco",
     "        while (GameState_IsRunning(gameState)) {\n            Graph_Update(&gfxCtx, gameState);\n        }\n",
     "        while (GameState_IsRunning(gameState)) {\n            Port_SsFrame(); /* port: estado rapido */\n"
     "            if (!GameState_IsRunning(gameState)) {\n                break;\n            }\n"
     "            Graph_Update(&gfxCtx, gameState);\n        }\n"),
    ("estado rapido: fim das macros",
     "    Graph_Destroy(&gfxCtx);\n    PRINTF(T(\"グラフィックスレッド実行終了\\n\", \"End of graphic thread execution\\n\"));\n}\n",
     "    Graph_Destroy(&gfxCtx);\n    PRINTF(T(\"グラフィックスレッド実行終了\\n\", \"End of graphic thread execution\\n\"));\n}\n"
     "#undef gfxCtx\n#undef gameState\n#undef size\n#undef nextOvl\n#undef ovl\n"),
])
patch_file("src/boot/z_std_dma.c", [
    ("estado rapido: DMA em andamento",
     "void DmaMgr_ThreadEntry(UNUSED void* arg) {\n",
     "static volatile s32 sPortDmaBusy; /* port: pedido sendo atendido (a leitura da ROM anda em outra thread) */\n\n"
     "s32 Port_DmaIdle(void) { /* port: estado rapido (graph.c) */\n"
     "    return MQ_IS_EMPTY(&sDmaMgrMsgQueue) && !sPortDmaBusy;\n}\n\n"
     "void DmaMgr_ThreadEntry(UNUSED void* arg) {\n"),
    ("estado rapido: marca o pedido",
     "        // Process the DMA request\n        DmaMgr_ProcessRequest(req);\n",
     "        // Process the DMA request\n        sPortDmaBusy = true;\n        DmaMgr_ProcessRequest(req);\n"),
    ("estado rapido: pedido atendido",
     "            if (0) {\n                PRINTF(\"osSendMesg: dmap=%08x, mq=%08x, m=%08x \\n\", req, req->notifyQueue, req->notifyMsg);\n"
     "            }\n        }\n",
     "            if (0) {\n                PRINTF(\"osSendMesg: dmap=%08x, mq=%08x, m=%08x \\n\", req, req->notifyQueue, req->notifyMsg);\n"
     "            }\n        }\n        sPortDmaBusy = false;\n"),
])
patch_file("src/audio/game/sequence.c", [
    ("estado rapido: musica",
     "u16 Audio_GetActiveSeqId(u8 seqPlayerIndex) {\n",
     """void Audio_StopSfxByBank(u8 bankId);
void Audio_PlayNatureAmbienceSequence(u8 natureAmbienceId);
void AudioOcarina_SetInstrument(u8 ocarinaInstrumentId);

/* port: estado rapido (graph.c). O som do jogo nao entra na copia: ao carregar, para os efeitos e poe para tocar
 * a musica que tocava quando o estado foi salvo. */
void Port_SsAudioIds(u16* seqMain, u16* seqSub) {
    *seqMain = gAudioCtx.seqPlayers[SEQ_PLAYER_BGM_MAIN].enabled ? gActiveSeqs[SEQ_PLAYER_BGM_MAIN].seqId : NA_BGM_DISABLED;
    *seqSub = gAudioCtx.seqPlayers[SEQ_PLAYER_BGM_SUB].enabled ? gActiveSeqs[SEQ_PLAYER_BGM_SUB].seqId : NA_BGM_DISABLED;
}

static void Port_SsSeq(u8 player, u16 seqId, u8 natureAmbienceId) {
    u16 cur = gAudioCtx.seqPlayers[player].enabled ? gActiveSeqs[player].seqId : NA_BGM_DISABLED;

    if (cur == seqId) {
        return;
    }
    if (seqId == NA_BGM_DISABLED) {
        SEQCMD_STOP_SEQUENCE(player, 0);
    } else if (player == SEQ_PLAYER_BGM_MAIN && (seqId & 0xFF) == NA_BGM_NATURE_AMBIENCE &&
               natureAmbienceId != NATURE_ID_DISABLED) {
        Audio_PlayNatureAmbienceSequence(natureAmbienceId);
    } else {
        SEQCMD_PLAY_SEQUENCE(player, 0, 0, seqId);
    }
}

void Port_SsAudioRestore(u16 seqMain, u16 seqSub, u8 natureAmbienceId) {
    u8 bankId;

    AudioOcarina_SetInstrument(0); /* OCARINA_INSTRUMENT_OFF */
    for (bankId = 0; bankId < 7; bankId++) {
        Audio_StopSfxByBank(bankId);
    }
    Port_SsSeq(SEQ_PLAYER_BGM_MAIN, seqMain, natureAmbienceId);
    Port_SsSeq(SEQ_PLAYER_BGM_SUB, seqSub, natureAmbienceId);
}

u16 Audio_GetActiveSeqId(u8 seqPlayerIndex) {
"""),
])
patch_file("src/code/z_message.c", [
    ("estado rapido: tabela do idioma",
     "void Message_FindMessageNES(PlayState* play, u16 textId) {\n",
     "void Port_LangRelink(void) { /* port: estado rapido (o ponteiro da tabela entra na copia; a tabela e do port) */\n"
     "    extern void* Port_LangNesTable(void);\n\n"
     "    if (Port_LangNesTable() != NULL) {\n"
     "        sNesMessageEntryTablePtr = (MessageTableEntry*)Port_LangNesTable();\n"
     "    }\n}\n\n"
     "void Message_FindMessageNES(PlayState* play, u16 textId) {\n"),
])

# Autosave (SoH): o jogo salva sozinho (como "Salvar" no menu de pausa) a cada N minutos de jogo (opcao autosave,
# 0 = desligado) e ao fechar o jogo (MENU, desligar, bateria: graph.c, estado rapido). So quando o SoH salvaria:
# arquivo carregado, 3 s depois de entrar na cena, fora da Camara dos Sabios e do mapa das cenas, fora do trecho entre a
# Ocarina do Tempo e a Cancao do Tempo; e (no intervalo) sem pausa, texto, cena de cinema, troca de cena ou Link morto.
# Um aviso curto aparece na tela.
patch_file("src/code/z_play.c", [
    ("SoH: salvamento automatico (funcao)",
     "void Play_Update(PlayState* this) {\n",
     """void Port_Autosave(PlayState* play, s32 now) { /* port: SoH Autosave_CanSave / Play_PerformSave */
    static u32 sFrames;
    extern int port_opt_autosave;
    void port_ss_autosave_toast(void) __attribute__((weak));
    u8 prevB;
    u8 prevStatus;

    if (!now) {
        if (++sFrames < (u32)port_opt_autosave * 60 * 20) { /* (o jogo roda a 20 quadros por segundo) */
            return;
        }
        if (IS_PAUSED(&play->pauseCtx) || (play->msgCtx.msgMode != MSGMODE_NONE) ||
            (play->csCtx.state != CS_STATE_IDLE) || Player_InCsMode(play) ||
            (play->transitionTrigger != TRANS_TRIGGER_OFF) || (play->transitionMode != TRANS_MODE_OFF) ||
            (gSaveContext.save.info.playerData.health <= 0)) {
            return; /* (tenta de novo no proximo quadro) */
        }
    }
    if ((gSaveContext.fileNum == 0xFF) || (gSaveContext.gameMode != GAMEMODE_NORMAL) ||
        (play->gameplayFrames < 60) || (play->sceneId == SCENE_CHAMBER_OF_THE_SAGES) ||
        (play->sceneId == SCENE_CUTSCENE_MAP) ||
        (!CHECK_QUEST_ITEM(QUEST_SONG_TIME) && (INV_CONTENT(ITEM_OCARINA_OF_TIME) == ITEM_OCARINA_OF_TIME))) {
        return;
    }
    sFrames = 0;
    Play_SaveSceneFlags(play);
    gSaveContext.save.info.playerData.savedSceneId = play->sceneId;
    /* o B das minigames/da Epona volta ao normal no save, como o menu de pausa faz */
    prevB = gSaveContext.save.info.equips.buttonItems[0];
    prevStatus = gSaveContext.buttonStatus[0];
    if ((prevB == ITEM_SLINGSHOT) || (prevB == ITEM_BOW) || (prevB == ITEM_BOMBCHU) || (prevB == ITEM_FISHING_POLE) ||
        ((prevB == ITEM_NONE) && (gSaveContext.save.info.infTable[INFTABLE_INDEX_1DX] == 0))) {
        gSaveContext.save.info.equips.buttonItems[0] = gSaveContext.buttonStatus[0];
    }
    Sram_WriteSave(&play->sramCtx);
    gSaveContext.save.info.equips.buttonItems[0] = prevB;
    gSaveContext.buttonStatus[0] = prevStatus;
    if (!now && port_ss_autosave_toast != NULL) {
        port_ss_autosave_toast();
    }
}

void Play_Update(PlayState* this) {
"""),
])

# Text Speed (SoH TextSpeed; o Redux usa 2x): o texto aparece N letras por quadro em vez de 1 (opcao text_speed,
# 1 = original). Para nao pular nenhum comando do texto (troca de caixa, escolha, ocarina, som...), o avanco para logo
# depois do primeiro comando (byte < 0x20): ele e sempre o ultimo da vez, como no jogo original. Trechos que o proprio
# texto deixa lentos (MESSAGE_TEXT_SPEED) ficam como estao.
patch_file("src/code/z_message.c", [
    ("SoH: texto mais rapido",
     "        }\n    }\n    if (msgCtx->textDelayTimer == 0) {\n        msgCtx->textDrawPos = i + 1;\n        msgCtx->textDelayTimer = msgCtx->textDelay;\n"
     "    } else {\n        msgCtx->textDelayTimer--;\n    }\n    *gfxP = gfx;\n}\n",
     "        }\n    }\n    if (msgCtx->textDelayTimer == 0) {\n        msgCtx->textDrawPos = i + 1;\n        msgCtx->textDelayTimer = msgCtx->textDelay;\n"
     "        { /* port: SoH TextSpeed */\n"
     "            extern int port_opt_text_speed;\n"
     "            s32 n = port_opt_text_speed;\n\n"
     "            if (n > 1 && msgCtx->textDelay == 0 && msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) {\n"
     "                while (--n > 0 && MSG_BUF_DECODED[msgCtx->textDrawPos - 1] >= 0x20 &&\n"
     "                       msgCtx->textDrawPos < msgCtx->decodedTextLen) {\n"
     "                    msgCtx->textDrawPos++;\n"
     "                }\n"
     "            }\n"
     "        }\n"
     "    } else {\n        msgCtx->textDelayTimer--;\n    }\n    *gfxP = gfx;\n}\n"),
])

# port: com o menu de opcoes aberto (MENU+START) o som tambem para: o gerenciador de som pula os retraces (nem
# atualiza as sequencias nem manda tarefa de audio); a saida fica em silencio e a musica continua de onde parou.
patch_file("src/code/audio_thread_manager.c", [
    ("menu de opcoes: som parado",
     "void AudioMgr_HandleRetrace(AudioMgr* audioMgr) {\n    AudioTask* rspTask;\n",
     "void AudioMgr_HandleRetrace(AudioMgr* audioMgr) {\n    AudioTask* rspTask;\n\n"
     "    {\n        int miyoo_menu_active(void) __attribute__((weak)); /* port/miyoo/menu_miyoo.c */\n\n"
     "        if (miyoo_menu_active != NULL && miyoo_menu_active()) {\n            return;\n        }\n    }\n"),
])

# port: idioma japones (opcao language = 2). O cartucho americano (NTSC 1.1) tambem traz o texto, a fonte kanji e os
# icones em japones; o jogo escolhe pela regiao (SaveContext_Init). Aqui o idioma vem da opcao, lida ao abrir o jogo
# (port_lang_game, lang.c), e vale de novo depois de carregar um estado (graph.c). O texto japones e de 16 bits em
# big-endian na ROM: depois de ler a mensagem, troca os bytes (como a mensagem 0xFFFC do teclado de nomes).
patch_file("src/code/z_common_data.c", [
    ("idioma da opcao",
     "#elif OOT_VERSION == GC_US || OOT_VERSION == GC_US_MQ\n    gSaveContext.language = LANGUAGE_ENG;\n",
     "    { /* port: idioma da opcao (0 ingles, 1 portugues = texto ingles trocado pelo pacote, 2 japones) */\n"
     "        extern int port_lang_game;\n\n"
     "        gSaveContext.language = (port_lang_game == 2) ? LANGUAGE_JPN : LANGUAGE_ENG;\n"
     "    }\n"
     "#elif OOT_VERSION == GC_US || OOT_VERSION == GC_US_MQ\n    gSaveContext.language = LANGUAGE_ENG;\n"),
])
patch_file("src/code/z_message.c", [
    ("japones: texto em big-endian",
     "            } else {\n                DMA_REQUEST_SYNC(MSG_BUF, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset,\n"
     "                                 font->msgLength, \"../z_message_PAL.c\", UNK_LINE);\n            }\n",
     "            } else {\n                DMA_REQUEST_SYNC(MSG_BUF, (uintptr_t)_jpn_message_data_staticSegmentRomStart + font->msgOffset,\n"
     "                                 font->msgLength, \"../z_message_PAL.c\", UNK_LINE);\n            }\n"
     "            { /* port: o texto japones e de 16 bits big-endian: para a ordem do processador */\n"
     "                u16* w_ = (u16*)MSG_BUF;\n                s32 k_;\n\n"
     "                for (k_ = 0; k_ < (s32)(font->msgLength / 2); k_++) {\n"
     "                    w_[k_] = (u16)((w_[k_] >> 8) | (w_[k_] << 8));\n                }\n            }\n"),
])
patch_file("src/code/z_kanfont.c", [
    ("menu de opcoes em japones: letras da fonte kanji",
     "void Font_LoadMessageBoxIcon(Font* font, u16 icon) {\n",
     "/* port: uma letra da fonte kanji (16x16, 4 bits) para o menu de opcoes em japones (port/miyoo/menu_miyoo.c) */\n"
     "void Port_KanjiGlyph(u16 sjis, void* out) {\n"
     "    DMA_REQUEST_SYNC(out, (uintptr_t)_kanjiSegmentRomStart + KanjiTex_CodepointToOffset(sjis), FONT_CHAR_TEX_SIZE,\n"
     "                     \"../z_kanfont.c\", UNK_LINE);\n}\n\n"
     "void Font_LoadMessageBoxIcon(Font* font, u16 icon) {\n"),
])

# Quit Fishing At Door (SoH): saindo pela porta com a vara, o dono pergunta se voce quer parar de pescar (Sim: a vara fica
# com ele e voce sai; Nao: volta para dentro, como antes). A pergunta entra na mensagem 0x409E (Port_MsgInject).
patch_file("src/overlays/actors/ovl_Fishing/z_fishing.c", [
    ("SoH: parar de pescar na porta (funcao)",
     "void Fishing_UpdateOwner(Actor* thisx, PlayState* play2) {\n",
     "static void Port_FishingQuitAtDoor(PlayState* play) { /* port: SoH Fishing_QuitAtDoor */\n"
     "    if ((Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE) && Message_ShouldAdvance(play)) {\n"
     "        Message_CloseTextbox(play);\n"
     "        if (play->msgCtx.choiceIndex == 0) {\n"
     "            if (sFishesCaught == 0) {\n"
     "                Message_ContinueTextbox(play, 0x4085);\n"
     "            } else if (sLinkAge == LINK_AGE_CHILD) {\n"
     "                Message_ContinueTextbox(play, 0x4092);\n"
     "            }\n"
     "            if (sIsOwnersHatHooked) {\n"
     "                sOwnerHair = FS_OWNER_CAPPED;\n"
     "                sIsOwnersHatHooked = false;\n"
     "            }\n"
     "            sFishingPlayingState = 0;\n"
     "            play->interfaceCtx.unk_260 = 0;\n"
     "        } else {\n"
     "            Rumble_Override(0.0f, 150, 10, 10);\n"
     "        }\n"
     "    }\n"
     "}\n\n"
     "void Fishing_UpdateOwner(Actor* thisx, PlayState* play2) {\n"),
    ("SoH: parar de pescar na porta (chamada)",
     "        case 11:\n            player->actor.world.pos.z = 1360.0f;\n            player->actor.speed = 0.0f;\n",
     "        case 11:\n            player->actor.world.pos.z = 1360.0f;\n            player->actor.speed = 0.0f;\n"
     "            if (" + O("quit_fishing_at_door") + ") { /* port: SoH QuitFishingAtDoor */\n"
     "                Port_FishingQuitAtDoor(play);\n            }\n"),
])

# ---------------------------------------------------------------- port: transicoes a 30 fps
# o escurecer/clarear da tela (fade) e o preenchimento de tela do ambiente mudam de transparencia a cada quadro
# intermediario tambem (port/frame_interp.c mistura a cor/alfa entre os dois quadros do jogo)
patch_file("src/code/z_fbdemo_fade.c", [
    ("fade a 30 fps",
     "        gDPSetPrimColor(gfx++, 0, 0, color->r, color->g, color->b, color->a);\n",
     "        gDPSetPrimColor(gfx++, 0, 0, color->r, color->g, color->b, color->a);\n"
     "        { extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(gfx - 1, 0); } /* port */\n"),
])
patch_file("src/code/z_kankyo.c", [
    ("preenchimento de tela a 30 fps (opaco)",
     "            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, red, green, blue, alpha);\n",
     "            gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, red, green, blue, alpha);\n"
     "            { extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(POLY_OPA_DISP - 1, 0); } /* port */\n"),
    ("preenchimento de tela a 30 fps (transparente)",
     "            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, red, green, blue, alpha);\n",
     "            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, red, green, blue, alpha);\n"
     "            { extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(POLY_XLU_DISP - 1, 0); } /* port */\n"),
])

# port: camera dando um pulinho quando o Link volta a superficie depois de mergulhar. Com a camera na agua o jogo
# aplica um tremor leve (Quake tipo 5, a ondulacao da agua); quando a camera sai da agua o tremor e removido de uma
# vez e a vista pula. Aqui o ultimo deslocamento de tremor some aos poucos (~6 quadros) em vez de sumir na hora.
patch_file("src/code/z_camera.c", [
    ("tremor some aos poucos (guarda)",
     "        viewEye.z = camera->eye.z + camShake.eyeOffset.z;\n\n        eyeAtAngle = OLib_Vec3fDiffToVecGeo(&viewEye, &viewAt);\n",
     "        viewEye.z = camera->eye.z + camShake.eyeOffset.z;\n"
     "        sPortShakeAt = camShake.atOffset; /* port */\n        sPortShakeEye = camShake.eyeOffset;\n\n"
     "        eyeAtAngle = OLib_Vec3fDiffToVecGeo(&viewEye, &viewAt);\n"),
    ("tremor some aos poucos",
     "    } else {\n        viewAt = camera->at;\n        viewEye = camera->eye;\n"
     "        eyeAtAngle = OLib_Vec3fDiffToVecGeo(&viewEye, &viewAt);\n",
     "    } else {\n        viewAt = camera->at;\n        viewEye = camera->eye;\n"
     "        if (" + O("fix_camera_surface_hop") + ") { /* port */\n"
     "            sPortShakeAt.x *= 0.6f; sPortShakeAt.y *= 0.6f; sPortShakeAt.z *= 0.6f;\n"
     "            sPortShakeEye.x *= 0.6f; sPortShakeEye.y *= 0.6f; sPortShakeEye.z *= 0.6f;\n"
     "            viewAt.x += sPortShakeAt.x; viewAt.y += sPortShakeAt.y; viewAt.z += sPortShakeAt.z;\n"
     "            viewEye.x += sPortShakeEye.x; viewEye.y += sPortShakeEye.y; viewEye.z += sPortShakeEye.z;\n"
     "        } else {\n"
     "            sPortShakeAt.x = sPortShakeAt.y = sPortShakeAt.z = 0.0f;\n"
     "            sPortShakeEye = sPortShakeAt;\n"
     "        }\n"
     "        eyeAtAngle = OLib_Vec3fDiffToVecGeo(&viewEye, &viewAt);\n"),
    ("tremor some aos poucos (variaveis)", "Vec3s Camera_Update(Camera* camera) {\n",
     "static Vec3f sPortShakeAt, sPortShakeEye; /* port: tremor que some aos poucos */\n\nVec3s Camera_Update(Camera* camera) {\n"),
])

# port: pulinho da camera ao emergir (2a tentativa). Quando o Link volta a superficie, a camera troca do modo de
# mergulho (CAM_SET_PIVOT_WATER_SURFACE) para o normal de uma vez. Por ~0,5 s depois da troca a vista (so o que e
# desenhado; a logica da camera nao muda) vai do ponto antigo ao novo suavemente.
patch_file("src/code/z_camera.c", [
    ("emergir suave (marca)",
     "        } else if (camera->stateFlags & CAM_STATE_PLAYER_DIVING) {\n"
     "            Camera_RequestSettingImpl(camera, *waterCamSetting,\n",
     "        } else if (camera->stateFlags & CAM_STATE_PLAYER_DIVING) {\n"
     "            sPortSurfaceBlend = 10; /* port: vista suave ao emergir */\n"
     "            Camera_RequestSettingImpl(camera, *waterCamSetting,\n"),
    ("emergir suave (variavel)", "s32 Camera_UpdateWater(Camera* camera) {\n",
     "static s32 sPortSurfaceBlend; /* port */\n\ns32 Camera_UpdateWater(Camera* camera) {\n"),
    ("emergir suave (vista)",
     "    camera->play->view.fovy = viewFov;\n    View_LookAt(&camera->play->view, &viewEye, &viewAt, &viewUp);\n",
     "    { /* port: vista suave ao emergir (fix_camera_surface_hop) */\n"
     "        static Vec3f sPortLastEye, sPortLastAt;\n"
     "        if (sPortSurfaceBlend > 0 && " + O("fix_camera_surface_hop") + ") {\n"
     "            f32 portK = sPortSurfaceBlend / 12.0f;\n"
     "            viewEye.x += (sPortLastEye.x - viewEye.x) * portK;\n"
     "            viewEye.y += (sPortLastEye.y - viewEye.y) * portK;\n"
     "            viewEye.z += (sPortLastEye.z - viewEye.z) * portK;\n"
     "            viewAt.x += (sPortLastAt.x - viewAt.x) * portK;\n"
     "            viewAt.y += (sPortLastAt.y - viewAt.y) * portK;\n"
     "            viewAt.z += (sPortLastAt.z - viewAt.z) * portK;\n"
     "            sPortSurfaceBlend--;\n"
     "        } else {\n"
     "            sPortSurfaceBlend = 0;\n"
     "        }\n"
     "        sPortLastEye = viewEye;\n"
     "        sPortLastAt = viewAt;\n"
     "    }\n"
     "    camera->play->view.fovy = viewFov;\n    View_LookAt(&camera->play->view, &viewEye, &viewAt, &viewUp);\n"),
])

# port: opcao auto_jump 0 (com o pulo manual): Link nao pula sozinho ao correr para fora de uma beirada; cai, e o pulo
# fica so no botao
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("pulo automatico opcional",
     "                    (sYDistToFloor > 20.0f) && (this->meleeWeaponState == 0) && (ABS(sp5C) < 0x2000) &&\n"
     "                    (this->speedXZ > 3.0f)) {\n",
     "                    (sYDistToFloor > 20.0f) && (this->meleeWeaponState == 0) && (ABS(sp5C) < 0x2000) &&\n"
     "                    (this->speedXZ > 3.0f) &&\n"
     "                    ({ extern int port_opt_auto_jump, port_opt_manual_jump; port_opt_auto_jump || !port_opt_manual_jump; })) { /* port */\n"),
])

# port: opcao ledge_grab 0: ao sair andando de uma beirada (sem o pulo automatico), Link cai em vez de se agarrar
# sozinho na borda. As trepadeiras e escadas (agarrar para descer) continuam; agarrar uma borda no ar, caindo de
# frente para uma parede, tambem.
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("agarrar a borda opcional",
     "            func_8083A5C4(play, this, sp84, sp54,\n"
     "                          sp50 ? &gPlayerAnim_link_normal_Fclimb_startB : &gPlayerAnim_link_normal_fall);\n",
     "            if (!sp50 && !({ extern int port_opt_ledge_grab; port_opt_ledge_grab; })) {\n"
     "                return 0; /* port: opcao ledge_grab 0 (cai em vez de se agarrar) */\n"
     "            }\n"
     "            func_8083A5C4(play, this, sp84, sp54,\n"
     "                          sp50 ? &gPlayerAnim_link_normal_Fclimb_startB : &gPlayerAnim_link_normal_fall);\n"),
])

# ---------------------------------------------------------------- port: pulo manual (opcao manual_jump, botao X)
# Com Link no chao, parado/andando/correndo (inclusive mirando com Z), o botao de pulo usa o mesmo pulo do jogo
# para beiradas (func_8083A4A8: correndo = pulo longo; parado = pulo curto). Nao pula nadando, escalando,
# pendurado, carregando algo, com o escudo levantado, falando ou em cena.
PORT_JUMP = r'''
/* port: pulo manual (port/miyoo/input_miyoo.c avisa o botao em port_input_jump) */
static void Port_ManualJump(Player* this, PlayState* play) {
    static s32 sPortJumpPrev;
    s32 held = ({ extern int port_input_jump; port_input_jump; });
    s32 pressed = held && !sPortJumpPrev;
    PlayerActionFunc f = this->actionFunc;

    sPortJumpPrev = held;
    if (!pressed || !({ extern int port_opt_manual_jump; port_opt_manual_jump; })) {
        return;
    }
    if (!(this->actor.bgCheckFlags & BGCHECKFLAG_GROUND) || Player_InCsMode(play) || (this->csAction != 0) ||
        (this->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD | PLAYER_STATE1_CARRYING_ACTOR |
                              PLAYER_STATE1_21 | PLAYER_STATE1_SHIELDING | PLAYER_STATE1_27 | PLAYER_STATE1_29)) ||
        (this->stateFlags2 & PLAYER_STATE2_CRAWLING) || (this->actor.depthInWater > 10.0f)) {
        return;
    }
    if (f == Player_Action_80840450 || f == Player_Action_808407CC || f == Player_Action_Idle ||
        f == Player_Action_80840DE4 || f == Player_Action_808414F8 || f == Player_Action_8084170C ||
        f == Player_Action_808417FC || f == Player_Action_8084193C || f == Player_Action_TurnInPlace ||
        f == Player_Action_80842180 || f == Player_Action_8084227C ||
        f == Player_Action_808423EC || f == Player_Action_8084251C) {
        /* como o pulo de beirada do jogo (func_8083A4A8), mas com forca minima (opcao jump_strength): parado, o pulo
         * de beirada quase nao sai do chao */
        s16 yawDiff = this->yaw - this->actor.shape.rot.y;
        LinkAnimationHeader* anim = ((ABS(yawDiff) < 0x1000) && (this->speedXZ > 4.0f)) ? &gPlayerAnim_link_normal_run_jump
                                                                                          : &gPlayerAnim_link_normal_jump;
        f32 vel = (this->speedXZ > (IREG(66) / 100.0f)) ? (IREG(67) / 100.0f)
                                                         : (IREG(68) / 100.0f) + ((IREG(69) * this->speedXZ) / 1000.0f);
        f32 minVel = ({ extern int port_opt_jump_strength; port_opt_jump_strength; }) / 10.0f;
        if (vel < minVel) {
            vel = minVel;
        }
        func_80838940(this, anim, vel, play, NA_SE_VO_LI_AUTO_JUMP);
        this->av2.actionVar2 = 1;
    }
}

'''
# port: corrida (opcao sprint_speed): A faz o Link rolar; segurando A depois da rolada ele corre mais rapido (a
# animacao de correr acelera junto, pois acompanha a velocidade). Solta o A ou para de andar: volta ao normal.
PORT_SPRINT = r'''
/* port: corrida segurando A depois da rolada */
s32 Port_Sprint;
static void Port_SprintUpdate(Player* this, PlayState* play) {
    static s32 sArmed;
    s32 aHeld = CHECK_BTN_ALL(play->state.input[0].cur.button, BTN_A);
    s32 pct = ({ extern int port_opt_sprint_speed; port_opt_sprint_speed; });

    if (pct <= 100 || !aHeld || Player_InCsMode(play) ||
        (this->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD | PLAYER_STATE1_CARRYING_ACTOR |
                              PLAYER_STATE1_21 | PLAYER_STATE1_SHIELDING | PLAYER_STATE1_27 | PLAYER_STATE1_29)) ||
        (this->stateFlags2 & PLAYER_STATE2_CRAWLING) || (this->focusActor != NULL)) {
        sArmed = Port_Sprint = false;
        return;
    }
    if (this->actionFunc == Player_Action_Roll) {
        sArmed = true; /* rolou com o A apertado */
        Port_Sprint = false;
        return;
    }
    if (sArmed && (this->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        Port_Sprint = (this->speedXZ > 1.0f) || (sControlStickMagnitude > 20.0f);
        if (!Port_Sprint) {
            sArmed = false; /* parou: so volta a correr depois de outra rolada */
        }
    }
}

'''
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("corrida (velocidade)",
     "            *outSpeedTarget = (*outSpeedTarget * 0.14f) - (8.0f * floorPitchInfluence * floorPitchInfluence);\n"
     "            *outSpeedTarget = CLAMP(*outSpeedTarget, 0.0f, speedCap);\n",
     "            *outSpeedTarget = (*outSpeedTarget * 0.14f) - (8.0f * floorPitchInfluence * floorPitchInfluence);\n"
     "            *outSpeedTarget = CLAMP(*outSpeedTarget, 0.0f, speedCap);\n"
     "            { /* port: corrida (Port_SprintUpdate) */\n"
     "                extern s32 Port_Sprint;\n"
     "                if (Port_Sprint) {\n"
     "                    *outSpeedTarget *= ({ extern int port_opt_sprint_speed; port_opt_sprint_speed; }) / 100.0f;\n"
     "                }\n"
     "            }\n"),
    ("corrida (funcao)", "void Player_Update(Actor* thisx, PlayState* play) {\n",
     PORT_SPRINT + "void Player_Update(Actor* thisx, PlayState* play) {\n"),
    ("corrida (chamada)", "    Port_EquipShortcuts(this, play, &input); /* port */\n",
     "    Port_EquipShortcuts(this, play, &input); /* port */\n    Port_SprintUpdate(this, play); /* port: corrida */\n"),
])
# port: corrida sem a paradinha no fim da rolada. Com a corrida ligada, o A ainda apertado e o direcional para algum
# lado, a rolada passa direto para a corrida no quadro 15 (onde o jogo ja deixa a rolada virar ataque), em vez de
# terminar a animacao de levantar (quadros 15-20) e passar um quadro parado antes de correr.
# Opcao better_roll (Enhancements): A de novo a partir do quadro 12 encadeia outra rolada (para onde o direcional
# aponta) e, durante a rolada, o direcional vira o Link aos poucos.
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("corrida sem paradinha (funcao)",
     "void Player_Action_Roll(Player* this, PlayState* play) {\n",
     """static s32 Port_SprintFromRoll(Player* this, PlayState* play) { /* port */
    f32 speedTarget;
    s16 yawTarget;

    if (({ extern int port_opt_sprint_speed; port_opt_sprint_speed; }) <= 100 ||
        !CHECK_BTN_ALL(play->state.input[0].cur.button, BTN_A) || (this->focusActor != NULL) ||
        Player_IsZTargeting(this)) {
        return false;
    }
    Player_GetMovementSpeedAndYaw(this, &speedTarget, &yawTarget, SPEED_MODE_CURVED, play);
    if (speedTarget == 0.0f) {
        return false;
    }
    func_8083C8DC(this, play, yawTarget);
    return true;
}

/* port: opcao better_roll - outra rolada encadeada (A de novo no fim da rolada, para onde o direcional aponta) */
static s32 Port_RollChain(Player* this, PlayState* play) {
    f32 speedTarget;
    s16 yawTarget;

    if (!({ extern int port_opt_better_roll; port_opt_better_roll; }) || (this->skelAnime.curFrame < 12.0f) ||
        !CHECK_BTN_ALL(sControlInput->press.button, BTN_A) || (sFloorType == FLOOR_TYPE_7) ||
        (this->focusActor != NULL) || Player_IsZTargeting(this)) {
        return false;
    }
    Player_GetMovementSpeedAndYaw(this, &speedTarget, &yawTarget, SPEED_MODE_CURVED, play);
    if (speedTarget == 0.0f) {
        return false;
    }
    this->actor.shape.rot.y = this->yaw = yawTarget;
    Player_SetupRoll(this, play);
    return true;
}

void Player_Action_Roll(Player* this, PlayState* play) {
"""),
    ("corrida sem paradinha (chamada)",
     "            if ((this->skelAnime.curFrame < 15.0f) || !Player_ActionHandler_7(this, play)) {\n"
     "                if (this->skelAnime.curFrame >= 20.0f) {\n",
     "            if (Port_RollChain(this, play)) {\n"
     "                return; /* port: better_roll */\n"
     "            }\n"
     "            if ((this->skelAnime.curFrame < 15.0f) || !Player_ActionHandler_7(this, play)) {\n"
     "                if ((this->skelAnime.curFrame >= 15.0f) && Port_SprintFromRoll(this, play)) {\n"
     "                    return; /* port: direto para a corrida */\n"
     "                }\n"
     "                if (this->skelAnime.curFrame >= 20.0f) {\n"),
    ("better_roll: direcao durante a rolada",
     "                Player_GetMovementSpeedAndYaw(this, &speedTarget, &yawTarget, SPEED_MODE_CURVED, play);\n\n"
     "                // `speedTarget` at this point is the speed that would be used for regular walking.\n",
     "                Player_GetMovementSpeedAndYaw(this, &speedTarget, &yawTarget, SPEED_MODE_CURVED, play);\n"
     "                if (({ extern int port_opt_better_roll; port_opt_better_roll; }) && (speedTarget != 0.0f)) {\n"
     "                    /* port: better_roll - a rolada vira aos poucos para onde o direcional aponta (~4 graus por quadro) */\n"
     "                    Math_ScaledStepToS(&this->actor.shape.rot.y, yawTarget, 0x300);\n"
     "                    this->yaw = this->actor.shape.rot.y;\n"
     "                }\n\n"
     "                // `speedTarget` at this point is the speed that would be used for regular walking.\n"),
])
# port: opcao air_steering (Controls): no ar (pulo ou queda), o direcional vira o Link aos poucos (~4 graus por quadro,
# como a rolada com better_roll), mas no maximo 45 graus para cada lado da direcao em que ele saiu do chao: so um
# direcionamento, nao uma curva no ar. Sem mira (Z) e sem o pulo de ataque (ficam como no jogo).
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("direcao no ar (funcao)",
     "void Player_Action_8084411C(Player* this, PlayState* play) {\n",
     """static void Port_AirSteer(Player* this, PlayState* play, f32 speedTarget, s16 yawTarget) { /* port */
    static s16 sBaseYaw;
    static u32 sLastFrame;
    s16 d;

    if (play->gameplayFrames != sLastFrame + 1) {
        sBaseYaw = this->actor.shape.rot.y; /* saiu do chao agora: a direcao de partida */
    }
    sLastFrame = play->gameplayFrames;
    if (!({ extern int port_opt_air_steering; port_opt_air_steering; }) || (speedTarget == 0.0f) ||
        (this->focusActor != NULL) || Player_IsZTargeting(this)) {
        return;
    }
    d = yawTarget - sBaseYaw;
    d = CLAMP(d, -0x2000, 0x2000); /* no maximo 45 graus da direcao de partida */
    Math_ScaledStepToS(&this->actor.shape.rot.y, sBaseYaw + d, 0x300);
    this->yaw = this->actor.shape.rot.y;
}

void Player_Action_8084411C(Player* this, PlayState* play) {\n"""),
    ("direcao no ar (chamada)",
     "        if (!(this->stateFlags2 & PLAYER_STATE2_19)) {\n            func_8083DFE0(this, &speedTarget, &yawTarget);\n        }\n",
     "        if (!(this->stateFlags2 & PLAYER_STATE2_19)) {\n            func_8083DFE0(this, &speedTarget, &yawTarget);\n"
     "            Port_AirSteer(this, play, speedTarget, yawTarget); /* port: opcao air_steering */\n        }\n"),
])
# port: corrida sem a paradinha ao aterrissar. Correndo (A segurado depois da rolada) e pulando, ao tocar o chao o
# jogo poe a animacao de aterrissar (run_jump_end / short_landing) com o Link parado ate ela acabar. Com a corrida
# ligada, o A ainda apertado e o direcional para algum lado, ele volta direto a correr (como no fim da rolada,
# Port_SprintFromRoll). So sem dano de queda, sem o pulo de ataque e sem mirar/carregar algo.
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("corrida sem paradinha ao aterrissar (declaracao)",
     "void Player_Action_8084411C(Player* this, PlayState* play) {\n",
     "static s32 Port_SprintFromRoll(Player* this, PlayState* play); /* port */\n"
     "void Player_Action_8084411C(Player* this, PlayState* play) {\n"),
    ("corrida sem paradinha ao aterrissar",
     "        sp3C = func_80843E64(play, this);\n\n",
     "        sp3C = func_80843E64(play, this);\n\n"
     "        if ((sp3C == 0) && !(this->stateFlags2 & PLAYER_STATE2_19) && !Player_CheckHostileLockOn(this) &&\n"
     "            !(this->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) && Port_SprintFromRoll(this, play)) {\n"
     "            return; /* port: direto para a corrida */\n"
     "        }\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("pulo manual (funcao)", "void Player_Update(Actor* thisx, PlayState* play) {\n",
     PORT_JUMP + "void Player_Update(Actor* thisx, PlayState* play) {\n"),
    ("pulo manual (chamada)",
     "    Port_EquipShortcuts(this, play, &input); /* port */\n",
     "    Port_EquipShortcuts(this, play, &input); /* port */\n    Port_ManualJump(this, play); /* port: pulo manual */\n"),
])

# port: segurar B passa as falas ate o fim. No fim de cada mensagem (MSGMODE_TEXT_DONE) o jogo usa
# Message_ShouldAdvanceSilent, que so aceitava um toque novo: segurando B a fala parava ali ate soltar e apertar.
patch_file("src/code/z_message.c", [
    ("segurar B tambem no fim da mensagem",
     "u8 Message_ShouldAdvanceSilent(PlayState* play) {\n    Input* input = &play->state.input[0];\n\n"
     "    return CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_B) ||\n",
     "u8 Message_ShouldAdvanceSilent(PlayState* play) {\n    Input* input = &play->state.input[0];\n\n"
     "    return CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_B) ||\n"
     "           (Port_SegurarB(play) && play->msgCtx.textboxEndType != TEXTBOX_ENDTYPE_2_CHOICE &&\n"
     "            play->msgCtx.textboxEndType != TEXTBOX_ENDTYPE_3_CHOICE) || /* port: segurar B (nunca escolhe opcao) */\n"),
])

# port: pular cena com START deixava a fala em andamento aberta (ex.: a narracao da Arvore Deku com fundo preto no
# comeco do jogo novo continuava passando depois do pulo). Ao pular, a caixa de texto aberta e fechada.
patch_file("src/code/z_demo.c", [
    ("pular cena fecha a fala",
     "        csCtx->state = CS_STATE_RUN_UNSTOPPABLE;\n        Audio_SetCutsceneFlag(0);\n"
     "        gSaveContext.cutsceneTransitionControl = 1;\n",
     "        csCtx->state = CS_STATE_RUN_UNSTOPPABLE;\n        Audio_SetCutsceneFlag(0);\n"
     "        gSaveContext.cutsceneTransitionControl = 1;\n"
     "        if ((csCtx->curFrame != cmd->startFrame) && (play->msgCtx.msgMode != MSGMODE_NONE)) {\n"
     "            Message_CloseTextbox(play); /* port: cena pulada, a fala tambem */\n"
     "        }\n"),
])

# port: marca de acerto leve (opcao light_hit_marks). A marca branca/amarela/vermelha que aparece quando a espada
# acerta um inimigo ou uma pedra de levantar (EffectSsHitMark) e um quadrado fixo de 24x24 texels virado para a
# camera, com uma imagem por quadro (8 quadros); perto da camera ele cobre boa parte da tela e o tranco vinha dai.
# A maior parte do quadrado e vazia: aqui ele e recortado para a parte da imagem que tem algo (com 1 texel de folga
# para o filtro; mesma posicao e as mesmas coordenadas de textura, a imagem nao muda), e os quadros quase vazios
# (menos de 30 texels acesos, pontinhos no fim da animacao; o ultimo e totalmente vazio) nao sao desenhados.
# Tabela: recorte em texels (x0, y0, x1, y1; x0 = -1 = nao desenha) para cada entrada de sTextures, calculado das
# imagens do jogo (gEffHitMark1..24Tex, i4 32x24; o quadro usa s e t de 0 a 24).
PORT_HITMARK = (
    "static const s8 sPortHitCrop[32][4] = { /* port: marca de acerto leve */\n"
    "    { 2, 4, 21, 22 }, { 2, 2, 20, 21 }, { 0, 1, 23, 23 }, { 0, 0, 23, 24 }, { 0, 0, 24, 24 }, { -1, 0, 0, 0 },\n"
    "    { -1, 0, 0, 0 }, { -1, 0, 0, 0 }, { 2, 2, 23, 23 }, { 2, 2, 24, 24 }, { 0, 1, 24, 24 }, { 0, 0, 24, 24 },\n"
    "    { 0, 0, 24, 24 }, { 0, 0, 24, 24 }, { 0, 0, 24, 23 }, { -1, 0, 0, 0 }, { 4, 0, 21, 18 }, { 4, 0, 22, 20 },\n"
    "    { 3, 0, 22, 21 }, { 2, 0, 23, 23 }, { 2, 0, 24, 24 }, { 1, 0, 24, 24 }, { 0, 1, 24, 24 }, { -1, 0, 0, 0 },\n"
    "    { 2, 4, 21, 22 }, { 2, 2, 20, 21 }, { 0, 1, 23, 23 }, { 0, 0, 23, 24 }, { 0, 0, 24, 24 }, { -1, 0, 0, 0 },\n"
    "    { -1, 0, 0, 0 }, { -1, 0, 0, 0 },\n"
    "};\n"
    "/* vertice do quadrado recortado: x e y 24 vezes maiores que no gEffHitMarkVtx (a escala da matriz e /24) */\n"
    "static void PortHitVtx(Vtx* v, s32 s, s32 t) {\n"
    "    v->v.ob[0] = -264 + s * 23;\n"
    "    v->v.ob[1] = 288 - t * 23;\n"
    "    v->v.ob[2] = 0;\n"
    "    v->v.flag = 0;\n"
    "    v->v.tc[0] = s << 5;\n"
    "    v->v.tc[1] = t << 5;\n"
    "    v->v.cn[0] = v->v.cn[1] = v->v.cn[2] = v->v.cn[3] = 0xFF;\n"
    "}\n\n")
patch_file("src/overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.c", [
    ("marca de acerto leve: tabela",
     "void EffectSsHitMark_Draw(PlayState* play, u32 index, EffectSs* this) {\n",
     PORT_HITMARK + "void EffectSsHitMark_Draw(PlayState* play, u32 index, EffectSs* this) {\n"),
    ("marca de acerto leve: quadro vazio e escala",
     "    SkinMatrix_SetTranslate(&mfTrans, this->pos.x, this->pos.y, this->pos.z);\n"
     "    scale = this->rScale / 100.0f;\n",
     "    const s8* portCrop = sPortHitCrop[((this->rType * 8) + (this->rTexIndex)) & 31];\n"
     "    s32 portLeve = " + O("light_hit_marks") + ";\n"
     "    if (portLeve && portCrop[0] < 0) {\n"
     "        return; /* port: quadro quase vazio */\n"
     "    }\n"
     "    SkinMatrix_SetTranslate(&mfTrans, this->pos.x, this->pos.y, this->pos.z);\n"
     "    scale = this->rScale / 100.0f;\n"
     "    if (portLeve) {\n"
     "        scale /= 24.0f; /* port: vertices 24x maiores */\n"
     "    }\n"),
    ("marca de acerto leve: quadrado recortado",
     "        gSPDisplayList(POLY_XLU_DISP++, this->gfx);\n",
     "        if (portLeve) { /* port: so a parte da imagem que tem algo */\n"
     "            Vtx* portV = GRAPH_ALLOC(gfxCtx, 4 * sizeof(Vtx));\n"
     "            PortHitVtx(&portV[0], portCrop[2], portCrop[1]);\n"
     "            PortHitVtx(&portV[1], portCrop[0], portCrop[1]);\n"
     "            PortHitVtx(&portV[2], portCrop[0], portCrop[3]);\n"
     "            PortHitVtx(&portV[3], portCrop[2], portCrop[3]);\n"
     "            gSPTexture(POLY_XLU_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);\n"
     "            gDPLoadTextureBlock_4b(POLY_XLU_DISP++, 0x08000000, G_IM_FMT_I, 32, 24, 0, G_TX_NOMIRROR | G_TX_WRAP,\n"
     "                                   G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);\n"
     "            gSPVertex(POLY_XLU_DISP++, portV, 4, 0);\n"
     "            gSP2Triangles(POLY_XLU_DISP++, 0, 1, 2, 0, 0, 2, 3, 0);\n"
     "        } else {\n"
     "            gSPDisplayList(POLY_XLU_DISP++, this->gfx);\n"
     "        }\n"),
])

# ================================================================ lote 127: melhorias de outros ports (1o bloco)
# Faster Pause Menu (SoH): animacoes do menu de pausa no dobro da velocidade (abrir, fechar, virar pagina)
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("SoH FasterPauseMenu: registros",
     "    s16 stepA;\n\n    if ((R_PAUSE_BG_PRERENDER_STATE >= PAUSE_BG_PRERENDER_READY) &&\n",
     "    s16 stepA;\n\n"
     "    if (" + O("faster_pause_menu") + ") { /* port: SoH FasterPauseMenu (o dobro do padrao 1 e metade do padrao 8) */\n"
     "        ZREG(46) = 2;\n"
     "        R_PAUSE_UI_ANIMS_DURATION = 4;\n"
     "    }\n"
     "    if ((R_PAUSE_BG_PRERENDER_STATE >= PAUSE_BG_PRERENDER_READY) &&\n"),
    ("SoH FasterPauseMenu: virar pagina 2x",
     "                case PAUSE_MAIN_STATE_SWITCHING_PAGE:\n"
     "                    KaleidoScope_UpdatePageSwitch(play, &play->state.input[0]);\n"
     "                    break;\n",
     "                case PAUSE_MAIN_STATE_SWITCHING_PAGE:\n"
     "                    KaleidoScope_UpdatePageSwitch(play, &play->state.input[0]);\n"
     "                    if (" + O("faster_pause_menu") + " && pauseCtx->mainState == PAUSE_MAIN_STATE_SWITCHING_PAGE) {\n"
     "                        KaleidoScope_UpdatePageSwitch(play, &play->state.input[0]); /* port: SoH FasterPauseMenu */\n"
     "                    }\n"
     "                    break;\n"),
    ("SoH FasterPauseMenu: volta ao padrao ao fechar",
     "        case PAUSE_STATE_RESUME_GAMEPLAY:\n            pauseCtx->state = PAUSE_STATE_OFF;\n",
     "        case PAUSE_STATE_RESUME_GAMEPLAY:\n            pauseCtx->state = PAUSE_STATE_OFF;\n"
     "            if (" + O("faster_pause_menu") + ") { /* port: SoH FasterPauseMenu */\n"
     "                ZREG(46) = 1;\n"
     "                R_PAUSE_UI_ANIMS_DURATION = 8;\n"
     "            }\n"),
])

# Faster Bottle Emptying (SoH): esvaziar a garrafa 3x mais rapido no comeco da animacao
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH FasterBottleEmpty",
     "void Player_Action_8084EFC0(Player* this, PlayState* play) {\n    Player_DecelerateToZero(this);\n",
     "void Player_Action_8084EFC0(Player* this, PlayState* play) {\n    Player_DecelerateToZero(this);\n"
     "    if (" + O("faster_bottle_empty") + ") { /* port: SoH FasterBottleEmpty */\n"
     "        this->skelAnime.playSpeed = this->skelAnime.curFrame <= 60.0f ? 3.0f : 1.0f;\n"
     "    }\n"),
])

# Faster Rupee Accumulator (SoH): o contador de rupias anda de 10 em 10 (alem do passo normal)
patch_file("src/code/z_parameter.c", [
    ("SoH FasterRupeeAccumulator",
     "    if (gSaveContext.rupeeAccumulator != 0) {\n        if (gSaveContext.rupeeAccumulator > 0) {\n",
     "    if (" + O("faster_rupee_counter") + " && gSaveContext.rupeeAccumulator != 0) { /* port: SoH FasterRupeeAccumulator */\n"
     "        if (gSaveContext.rupeeAccumulator >= 10 &&\n"
     "            gSaveContext.save.info.playerData.rupees + 10 < CUR_CAPACITY(UPG_WALLET)) {\n"
     "            gSaveContext.rupeeAccumulator -= 10;\n"
     "            gSaveContext.save.info.playerData.rupees += 10;\n"
     "        } else if (gSaveContext.rupeeAccumulator <= -10 && gSaveContext.save.info.playerData.rupees > 10) {\n"
     "            gSaveContext.rupeeAccumulator += 10;\n"
     "            gSaveContext.save.info.playerData.rupees -= 10;\n"
     "        }\n"
     "    }\n"
     "    if (gSaveContext.rupeeAccumulator != 0) {\n        if (gSaveContext.rupeeAccumulator > 0) {\n"),
])

# Faster Shadow Ship (SoH): o barco do Templo da Sombra anda 25 (em vez de 4) quando nao ha Stalfos perto e o Link
# esta a bordo
patch_file("src/overlays/actors/ovl_Bg_Haka_Ship/z_bg_haka_ship.c", [
    ("SoH FasterShadowShip",
     "    } else {\n        Math_StepToF(&this->dyna.actor.speed, 4.0f, 0.2f);\n    }\n",
     "    } else if (" + O("faster_shadow_ship") + " && !Play_InCsMode(play) && /* port: SoH FasterShadowShip */\n"
     "               Actor_FindNearby(play, &this->dyna.actor, ACTOR_EN_TEST, ACTORCAT_ENEMY, 800.0f) == NULL &&\n"
     "               Actor_FindNearby(play, &this->dyna.actor, ACTOR_PLAYER, ACTORCAT_PLAYER, 450.0f) != NULL) {\n"
     "        this->dyna.actor.speed = 25.0f;\n"
     "    } else if (" + O("faster_shadow_ship") + " && !Play_InCsMode(play) && this->dyna.actor.speed > 4.0f) {\n"
     "        this->dyna.actor.speed = 4.0f;\n"
     "    } else {\n        Math_StepToF(&this->dyna.actor.speed, 4.0f, 0.2f);\n    }\n"),
])

# No HUD Heart Animation (SoH): o coracao que esta enchendo nao pulsa
patch_file("src/code/z_lifemeter.c", [
    ("SoH NoHUDHeartAnimation",
     "                Matrix_SetTranslateScaleMtx2(\n"
     "                    matrix, 1.0f - (0.32f * beatingHeartPulsingSize), 1.0f - (0.32f * beatingHeartPulsingSize),\n"
     "                    1.0f - (0.32f * beatingHeartPulsingSize), -130.0f + offsetX, 94.5f - offsetY, 0.0f);\n",
     "                if (" + O("no_heart_animation") + ") { /* port: SoH NoHUDHeartAnimation */\n"
     "                    Matrix_SetTranslateScaleMtx2(matrix, 1.0f, 1.0f, 1.0f, -130.0f + offsetX, 94.5f - offsetY, 0.0f);\n"
     "                } else\n"
     "                Matrix_SetTranslateScaleMtx2(\n"
     "                    matrix, 1.0f - (0.32f * beatingHeartPulsingSize), 1.0f - (0.32f * beatingHeartPulsingSize),\n"
     "                    1.0f - (0.32f * beatingHeartPulsingSize), -130.0f + offsetX, 94.5f - offsetY, 0.0f);\n"),
])

# Disable Title Card (SoH): sem o nome da area ao entrar
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH DisableTitleCard",
     "        if ((titleFileSize != 0) && gSaveContext.showTitleCard) {\n",
     "        if ((titleFileSize != 0) && gSaveContext.showTitleCard && !" + O("disable_title_card") + ") { /* port: SoH */\n"),
])

# Dampe All Night (SoH) e Open All Hours (SoH): portas ajustadas antes de montar (EnDoor_Init le os parametros).
#  - Dampe: a porta da cabana do coveiro (774) vira a aberta (447) quando o Link crianca esta no cemiterio.
#  - Lojas: portas de lojas/jogos que so abrem de dia ou de noite viram portas comuns de troca de cena.
patch_file("src/overlays/actors/ovl_En_Door/z_en_door.c", [
    ("SoH DampeAllNight / OpenAllHours: portas",
     "    objectInfo = &sDoorInfo[0];\n    Actor_ProcessInitChain(&this->actor, sInitChain);\n",
     "    if (" + O("dampe_all_night") + " && this->actor.params == 774 && !LINK_IS_ADULT && play->sceneId == SCENE_GRAVEYARD) {\n"
     "        this->actor.params = 447; /* port: SoH DampeAllNight (porta da cabana aberta) */\n"
     "    }\n"
     "    if (" + O("shops_open_all_hours") + ") { /* port: SoH OpenAllHours */\n"
     "        switch (this->actor.params) {\n"
     "            case 653: case 2689: case 1678: case 2703: case 3728: case 4753: case 4751: case 6801: case 7822: case 8846:\n"
     "                this->actor.params = (this->actor.params & 0xFC00) | (DOOR_SCENEEXIT << 7) | 0x3F;\n"
     "                break;\n"
     "            default:\n"
     "                break;\n"
     "        }\n"
     "    }\n"
     "    objectInfo = &sDoorInfo[0];\n    Actor_ProcessInitChain(&this->actor, sInitChain);\n"),
])
patch_file("src/overlays/actors/ovl_En_Tk/z_en_tk.c", [
    ("SoH DampeAllNight: o coveiro fica a noite toda",
     "    if (gSaveContext.save.dayTime <= CLOCK_TIME(18, 0) || gSaveContext.save.dayTime >= CLOCK_TIME(21, 0) ||\n"
     "        LINK_IS_ADULT || play->sceneId != SCENE_GRAVEYARD) {\n",
     "    if (" + O("dampe_all_night") + " ? (LINK_IS_ADULT || play->sceneId != SCENE_GRAVEYARD) /* port: SoH DampeAllNight */\n"
     "        : (gSaveContext.save.dayTime <= CLOCK_TIME(18, 0) || gSaveContext.save.dayTime >= CLOCK_TIME(21, 0) ||\n"
     "        LINK_IS_ADULT || play->sceneId != SCENE_GRAVEYARD)) {\n"),
])

# Night GS Always Spawn (SoH DaytimeGS): as Skulltulas douradas que so aparecem a noite aparecem tambem de dia
# (as de dia sao criadas a mais, com os mesmos dados da cena da noite; a da arvore de Kakariko cai ao bater nela)
PORT_DAYTIME_GS = (
    "static void Port_DaytimeGS(PlayState* play) { /* port: SoH NightGSAlwaysSpawn */\n"
    "    static const struct { s16 scene, room, child, n; s16 e[4][7]; } k[] = {\n"
    "        { SCENE_GRAVEYARD, 1, 1, 1, { { 156, 315, 795, 16384, -32768, 0, -20096 } } },\n"
    "        { SCENE_ZORAS_FOUNTAIN, 0, 1, 1, { { -1891, 187, 1911, 16384, 18022, 0, -19964 } } },\n"
    "        { SCENE_GERUDOS_FORTRESS, 0, 0, 1, { { 1598, 999, -2008, 16384, -16384, 0, -19198 } } },\n"
    "        { SCENE_GERUDOS_FORTRESS, 1, 0, 1, { { 3377, 1734, -4935, 16384, 0, 0, -19199 } } },\n"
    "        { SCENE_KAKARIKO_VILLAGE, 0, 0, 1, { { -18, 540, 1800, 0, -32768, 0, -20160 } } },\n"
    "        { SCENE_KAKARIKO_VILLAGE, 0, 1, 4, { { -465, 377, -888, 0, 28217, 0, -20222 }, { 5, 686, -171, 0, -32768, 0, -20220 },\n"
    "                                           { 324, 270, 905, 16384, 0, 0, -20216 }, { -602, 120, 1120, 16384, 0, 0, -20208 } } },\n"
    "        { SCENE_LON_LON_RANCH, 0, 1, 3, { { -2344, 180, 672, 16384, 22938, 0, -29695 }, { 808, 48, 326, 16384, 0, 0, -29694 },\n"
    "                                         { 997, 286, -2698, 16384, -16384, 0, -29692 } } },\n"
    "    };\n"
    "    s32 i, j;\n"
    "    if (!" + O("daytime_gold_skulltulas") + " || !IS_DAY) {\n"
    "        return;\n"
    "    }\n"
    "    for (i = 0; i < (s32)ARRAY_COUNT(k); i++) {\n"
    "        if (k[i].child == (LINK_IS_CHILD ? 1 : 0) && k[i].scene == play->sceneId && k[i].room == play->roomCtx.curRoom.num) {\n"
    "            for (j = 0; j < k[i].n; j++) {\n"
    "                Actor_Spawn(&play->actorCtx, play, ACTOR_EN_SW, k[i].e[j][0], k[i].e[j][1], k[i].e[j][2], k[i].e[j][3],\n"
    "                            k[i].e[j][4], k[i].e[j][5], k[i].e[j][6]);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n\n")
patch_file("src/code/z_actor.c", [
    ("SoH NightGSAlwaysSpawn: funcao",
     "void Actor_UpdateAll(PlayState* play, ActorContext* actorCtx) {\n",
     PORT_DAYTIME_GS + "void Actor_UpdateAll(PlayState* play, ActorContext* actorCtx) {\n"),
    ("SoH NightGSAlwaysSpawn: depois dos atores da cena",
     "            Actor_SpawnEntry(&play->actorCtx, actorEntry++, play);\n        }\n        play->numActorEntries = 0;\n    }\n",
     "            Actor_SpawnEntry(&play->actorCtx, actorEntry++, play);\n        }\n        play->numActorEntries = 0;\n"
     "        Port_DaytimeGS(play); /* port */\n    }\n"),
])
patch_file("src/overlays/actors/ovl_En_Sw/z_en_sw.c", [
    ("SoH NightGSAlwaysSpawn: Skulltula visivel de dia",
     "        Math_ApproachF(&this->actor.scale.x, !IS_DAY ? 0.02f : 0.0f, 0.2f, 0.01f);\n",
     "        Math_ApproachF(&this->actor.scale.x, (!IS_DAY || " + O("daytime_gold_skulltulas") + ") ? 0.02f : 0.0f, 0.2f, 0.01f); /* port */\n"),
])
patch_file("src/overlays/actors/ovl_En_Wood02/z_en_wood02.c", [
    ("SoH NightGSAlwaysSpawn: cabecalho", '#include "z_en_wood02.h"\n', '#include "z_en_wood02.h"\n#include "save.h" /* port */\n'),
    ("SoH NightGSAlwaysSpawn: arvore de Kakariko",
     "    spawnType = WOOD_SPAWN_NORMAL;\n    actorScale = 1.0f;\n",
     "    if (" + O("daytime_gold_skulltulas") + " && (this->actor.params & 0xFF) == WOOD_TREE_CONICAL_MEDIUM && IS_DAY &&\n"
     "        play->sceneId == SCENE_KAKARIKO_VILLAGE) { /* port: SoH (a arvore de dia tem os dados da de noite) */\n"
     "        this->actor.params = 0x2001;\n"
     "        this->actor.home.rot.z = 0x71;\n"
     "    }\n"
     "    spawnType = WOOD_SPAWN_NORMAL;\n    actorScale = 1.0f;\n"),
])

# Rebottle Blue Fire (SoH): o fogo azul solto da garrafa pode ser pego de novo
patch_file("src/overlays/actors/ovl_En_Ice_Hono/z_en_ice_hono.c", [
    ("SoH RebottleBlueFire",
     "    if (this->actionFunc != NULL) {\n        this->actionFunc(this, play);\n    }\n}\n",
     "    if (this->actionFunc != NULL) {\n        this->actionFunc(this, play);\n    }\n"
     "    if (" + O("rebottle_blue_fire") + " && this->actionFunc != EnIceHono_CapturableFlame &&\n"
     "        EnIceHono_InBottleRange(this, play)) { /* port: SoH RebottleBlueFire (GI_MAX: pega na garrafa) */\n"
     "        Actor_OfferGetItem(&this->actor, play, GI_MAX, 60.0f, 100.0f);\n"
     "    }\n}\n"),
])

# ---------------------------------------------------------------- lote 128
# Fast Drops / Skip Pickup Messages (SoH FastDrops): municao e consumiveis caidos de moitas, pedras e inimigos
# (e as nozes/gravetos das Deku Babas) nao mostram a cena de "voce pegou" na primeira vez. Pedaco de coracao e
# chave pequena continuam com a cena (como no SoH).
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH FastDrops",
     "                if ((Item_CheckObtainability(giEntry->itemId) == ITEM_NONE) ||\n"
     "                    (play->sceneId == SCENE_BOMBCHU_BOWLING_ALLEY)) {\n",
     "                if (((Item_CheckObtainability(giEntry->itemId) == ITEM_NONE) ||\n"
     "                     (play->sceneId == SCENE_BOMBCHU_BOWLING_ALLEY)) &&\n"
     "                    !(" + O("fast_drops") + " && /* port: SoH FastDrops */\n"
     "                      ((interactedActor->id == ACTOR_EN_ITEM00 && interactedActor->params != ITEM00_HEART_PIECE &&\n"
     "                        interactedActor->params != ITEM00_SMALL_KEY) ||\n"
     "                       interactedActor->id == ACTOR_EN_KAREBABA || interactedActor->id == ACTOR_EN_DEKUBABA))) {\n"),
])

# Skip Forced Dialog (SoH TimeSavers.SkipForcedDialog), separado em duas opcoes: Navi e outros personagens.
# Navi: as falas forcadas (Navi chamando ao entrar numa area) somem; a marca de "ja falou" e gravada como se
# tivesse falado. As que so tocam com C-cima (bit 0x80) ficam.
_NAVI_SKIP = (
    "        if (" + O("skip_navi_forced_talk") + " && !(((s32)this->actor.params >> 8) & 0x80)) { /* port: SoH */\n"
    "            if ((((s32)this->actor.params >> 8) & 0x3F) != 0x3F) {\n"
    "                Flags_SetSwitch(play, ((s32)this->actor.params >> 8) & 0x3F);\n"
    "            }\n"
    "            Actor_Kill(&this->actor);\n"
    "            return;\n"
    "        }\n")
patch_file("src/overlays/actors/ovl_Elf_Msg/z_elf_msg.c", [
    ("SoH SkipForcedDialog Navi (caixa)",
     "        (fabsf(player->actor.world.pos.z - this->actor.world.pos.z) < (100.0f * this->actor.scale.z))) {\n"
     "        player->naviTextId = ElfMsg_GetMessageId(this);\n",
     "        (fabsf(player->actor.world.pos.z - this->actor.world.pos.z) < (100.0f * this->actor.scale.z))) {\n"
     + _NAVI_SKIP +
     "        player->naviTextId = ElfMsg_GetMessageId(this);\n"),
    ("SoH SkipForcedDialog Navi (cilindro)",
     "        ((player->actor.world.pos.y - this->actor.world.pos.y) < (100.0f * this->actor.scale.y))) {\n"
     "        player->naviTextId = ElfMsg_GetMessageId(this);\n",
     "        ((player->actor.world.pos.y - this->actor.world.pos.y) < (100.0f * this->actor.scale.y))) {\n"
     + _NAVI_SKIP +
     "        player->naviTextId = ElfMsg_GetMessageId(this);\n"),
])

# Outros personagens: conversas que travam o Link ao chegar perto (mensagens "invisiveis" das areas, a Gerudo
# que entrega o cartao, o Goron Link rolando).
patch_file("src/overlays/actors/ovl_En_Wonder_Talk2/z_en_wonder_talk2.c", [
    ("SoH SkipForcedDialog NPC (fim)",
     "            this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_UPDATE_CULLING_DISABLED);\n"
     "            Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_7);\n",
     "            this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_UPDATE_CULLING_DISABLED);\n"
     "            if (!" + O("skip_npc_forced_talk") + ") {\n"
     "                Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_7);\n"
     "            } else if (GET_PLAYER(play)->csAction == PLAYER_CSACTION_8) { /* port: SoH (opcao mudada no meio) */\n"
     "                Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_7);\n"
     "            }\n"),
    ("SoH SkipForcedDialog NPC (inicio)",
     "                Message_StartTextbox(play, this->actor.textId, NULL);\n"
     "                Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_8);\n",
     "                if (!" + O("skip_npc_forced_talk") + ") { /* port: SoH SkipForcedDialog */\n"
     "                    Message_StartTextbox(play, this->actor.textId, NULL);\n"
     "                    Player_SetCsActionWithHaltedActors(play, NULL, PLAYER_CSACTION_8);\n"
     "                }\n"),
])
patch_file("src/overlays/actors/ovl_En_Ge3/z_en_ge3.c", [
    ("SoH SkipForcedDialog NPC (cartao Gerudo)",
     "    if ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play)) {\n"
     "        Message_CloseTextbox(play);\n"
     "        this->actor.flags &= ~ACTOR_FLAG_TALK_OFFER_AUTO_ACCEPTED;\n",
     "    if (" + O("skip_npc_forced_talk") + " || /* port: SoH */\n"
     "        ((Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) && Message_ShouldAdvance(play))) {\n"
     "        Message_CloseTextbox(play);\n"
     "        this->actor.flags &= ~ACTOR_FLAG_TALK_OFFER_AUTO_ACCEPTED;\n"),
])
patch_file("src/overlays/actors/ovl_En_Go2/z_en_go2.c", [
    ("SoH SkipForcedDialog NPC (Goron Link)",
     "                case GORON_CITY_LINK:\n"
     "                    this->goronState = 0;\n"
     "                    this->actionFunc = EnGo2_GoronLinkStopRolling;\n",
     "                case GORON_CITY_LINK:\n"
     "                    if (" + O("skip_npc_forced_talk") + ") { /* port: SoH (para de rolar sem a conversa forcada) */\n"
     "                        this->trackingMode = NPC_TRACKING_NONE;\n"
     "                        this->unk_211 = false;\n"
     "                        this->isAwake = false;\n"
     "                        this->actionFunc = EnGo2_CurledUp;\n"
     "                        SET_INFTABLE(INFTABLE_10C);\n"
     "                        break;\n"
     "                    }\n"
     "                    this->goronState = 0;\n"
     "                    this->actionFunc = EnGo2_GoronLinkStopRolling;\n"),
])

# Skip Warp Cutscenes (SoH WarpSongSkipAnimation): as musicas de teleporte vao direto para o destino, sem a cena
# de partida (luzes subindo) nem a de chegada. Como no SoH, no nascimento do Demo_Kankyo da partida (parametro 0x0F):
# ele nao nasce (Actor_Spawn devolve NULL e o Link chama Environment_WarpSongLeave, como o jogo ja faz quando o ator
# nao cabe) e a chegada vira o modo parado (13) em vez do modo de musica de teleporte (5).
patch_file("src/code/z_actor.c", [
    ("SoH WarpSongSkipAnimation",
     "    overlayEntry = &gActorOverlayTable[actorId];\n",
     "    if (actorId == ACTOR_DEMO_KANKYO && params == 0x0F && " + O("skip_warp_cutscenes") + ") { /* port: SoH */\n"
     "        s32 portPp = gSaveContext.respawn[1].playerParams; /* 1 = RESPAWN_MODE_RETURN */\n"
     "        if (((portPp & 0xF00) >> 8) == 5) {\n"
     "            gSaveContext.respawn[1].playerParams = (portPp & ~0xF00) | (13 << 8);\n"
     "        }\n"
     "        return NULL;\n"
     "    }\n"
     "    overlayEntry = &gActorOverlayTable[actorId];\n"),
])

# Skip Tower Escape e Skip Child Stealth (SoH TimeSavers): antes de montar a cena (Cutscene_HandleConditionalTriggers)
#  - fuga da torre de Ganon: vai direto para a luta final (com a marca da cena das Gerudo vista);
#  - furtiva do castelo (Link crianca): o buraco do castelo leva direto ao patio da Zelda, sem os guardas.
patch_file("src/code/z_demo.c", [
    ("SoH SkipTowerEscape / SkipChildStealth",
     "    PRINTF(\"\\ngame_info.mode=[%d] restart_flag\", ((void)0, gSaveContext.respawnFlag));\n",
     "    PRINTF(\"\\ngame_info.mode=[%d] restart_flag\", ((void)0, gSaveContext.respawnFlag));\n"
     "    if (" + O("skip_tower_escape") + " && /* port: SoH SkipTowerEscape */\n"
     "        gSaveContext.save.entranceIndex == ENTR_GANONS_TOWER_COLLAPSE_EXTERIOR_0) {\n"
     "        Flags_SetEventChkInf(EVENTCHKINF_GERUDO_CAUGHT_TOWER_FALL);\n"
     "        gSaveContext.save.entranceIndex = ENTR_GANON_BOSS_0;\n"
     "        return;\n"
     "    }\n"
     "    if (" + O("skip_child_stealth") + " && /* port: SoH SkipChildStealth */\n"
     "        gSaveContext.save.entranceIndex == ENTR_CASTLE_COURTYARD_GUARDS_DAY_0) {\n"
     "        gSaveContext.save.entranceIndex = ENTR_CASTLE_COURTYARD_ZELDA_0;\n"
     "        return;\n"
     "    }\n"),
])

# Skip Feeding Jabu-Jabu (SoH TimeSavers.SkipJabuJabuFish): o Jabu-Jabu abre a boca quando o Link chega perto, sem
# precisar do peixe nem da cena dele engolindo.
_jj = read(os.path.join(oot, "src/overlays/actors/ovl_En_Jj/z_en_jj.c"))
_jjWait, _jjOpen = ("EnJj_WaitForFish", "EnJj_WaitToOpenMouth") if "EnJj_WaitForFish" in _jj else \
                   ("EnJj_WaitFishDrop", "EnJj_WaitPlayerNear")  # (os nomes mudam conforme a versao do decomp)
patch_file("src/overlays/actors/ovl_En_Jj/z_en_jj.c", [
    ("SoH SkipJabuJabuFish",
     "    } else {\n        this->actionFunc(this, play);\n",
     "    } else {\n"
     "        if (" + O("skip_jabu_fish") + " && this->actionFunc == " + _jjWait + ") { /* port: SoH */\n"
     "            EnJj_SetupAction(this, " + _jjOpen + ");\n"
     "        }\n"
     "        this->actionFunc(this, play);\n"),
])

# Market Sneak (SoH MarketSneak): a noite, o guarda da entrada do mercado (Link crianca, sem mascara) pergunta se
# o Link quer dar uma volta; "sim" leva ao campo de Hyrule (perto da ponte, como no SoH). A fala nova vem do
# Port_MsgMarketSneak (z_message.c) em ingles, portugues (sem acentos: a fonte da traducao e propria) e japones.
patch_file("src/overlays/actors/ovl_En_Heishi4/z_en_heishi4.c", [
    ("SoH MarketSneak (resposta)",
     "void func_80A56B40(EnHeishi4* this, PlayState* play) {\n",
     "static void Port_HeishiMarketSneak(EnHeishi4* this, PlayState* play) { /* port: SoH EnHeishi4_MarketSneak */\n"
     "    SkelAnime_Update(&this->skelAnime);\n"
     "    if (Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE && Message_ShouldAdvance(play)) {\n"
     "        Message_CloseTextbox(play);\n"
     "        if (play->msgCtx.choiceIndex == 0) {\n"
     "            play->nextEntranceIndex = 0x0CD; /* campo de Hyrule, depois da ponte (nao cai na agua) */\n"
     "            play->transitionTrigger = TRANS_TRIGGER_START;\n"
     "            play->transitionType = TRANS_TYPE_FADE_WHITE;\n"
     "            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;\n"
     "        }\n"
     "        this->actionFunc = func_80A56614;\n"
     "    }\n"
     "}\n\n"
     "void func_80A56B40(EnHeishi4* this, PlayState* play) {\n"),
    ("SoH MarketSneak (conversa)",
     "        if (this->type == HEISHI4_AT_MARKET_NIGHT) {\n"
     "            this->actionFunc = func_80A56614;\n"
     "            return;\n"
     "        }\n"
     "    }\n"
     "    Actor_OfferTalkNearColChkInfoCylinder(&this->actor, play);\n",
     "        if (this->type == HEISHI4_AT_MARKET_NIGHT) {\n"
     "            if (" + O("market_sneak") + " && play->sceneId == SCENE_MARKET_ENTRANCE_NIGHT &&\n"
     "                GET_PLAYER(play)->currentMask == PLAYER_MASK_NONE && this->actor.textId == 0x7003) { /* port: SoH */\n"
     "                this->actionFunc = Port_HeishiMarketSneak;\n"
     "                return;\n"
     "            }\n"
     "            this->actionFunc = func_80A56614;\n"
     "            return;\n"
     "        }\n"
     "    }\n"
     "    Actor_OfferTalkNearColChkInfoCylinder(&this->actor, play);\n"),
])

# ---------------------------------------------------------------- lote 131
# Miras (SoH BowReticle e HookshotableReticle) e ciclo de flechas (SoH BowArrowCycle).
# A mira do gancho e um triangulo com a textura de 64x64 do object_link_boy (o Link adulto). O Link crianca nao tem
# esse objeto carregado; por isso a textura vai copiada aqui (do PNG extraido do cartucho 1.1, igual byte a byte) e
# a mira e montada na hora com a cor escolhida, sem depender do segmento 6.
_RET_TEX = bytes.fromhex("000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100010001020102040404080b0c0c0f14191b1a1c222121242a282c2e302e272a2d2b29292025201d221f1312100b090607050504030201000101000000000000020003020305050908090810161a1c1a1d24242a3a4b5f6f78848c8d8b8c857767624d3b292a26221d1a1a211412140a090705070303030202010000000100010101010403090407070c0f1312191f21293f5e728a92939896999c9a989c969599968f8d785e452527282420171713120d080c0606050102020101000000010201010103070609110d131822212123506f8b90979a9fa0a1a6a5a6a5a7a7a3a3a2a3a19896998d72502c2a22221e1a18140a0d0b09040301040303000001010302030704050a0d11191420222445758e909a9ba2a5a5adaaafb4b7b3b4b5b4b1b3adaaaba7a29c98927b502b2d25211f17150f110c0b07050101010000000000020603080a0c1111141e2533698d96999ea6a9aab1b7bbbbbec0c1c0c0c2c2c2bfbdb8b7b2aeaca49d98956f472c252117180f100b0c0a080402010000010303010807090e0c17161f204f7f909b9da6abafb5b8bfc1c5c8cbcbcdcecdcdcdccccc9c5c2beb9b6b0a7a59f968459372b27211a1b11090906040403000002030301060b0e12161b1c2c5d8e949ea4abb0b8bcc2c6cbcdd2d3d6d7d9d8d9dad9d7d6d6d3cfccc7c2b8b8b1aba499916e3b2c261d19120c0f080403020000010301090c1113151a2239729598a0a5aeb5bdc5c6cbd3d6d8dbdddfe0e2e4e4e3e2e2e1dfddd8d6d3d0cac2bdb8ada79d9779442e272719170e0d0a0502000006060808060e0f1a1836718f98a2a7b2bbc3c6ced2d7dae0e1e3e5e7e9eaebebebeaeaeae8e6e4dfdcd8d4cdc8c1bab0a7a0957b422b251c17180f0a080600000304080b0e16181b36788f99a0acb1b8c1ccd2d7dce0e3e6eaecedeeeff0f1f1f1f1f0eeeeedeae7e7e2ded7d0c9c3bcb4aaa2967c3e2d261b17140f090800000707080d0b162131718f9b9fb0b5c0c5ccd1d8dce2e8eaeceef1f2f4f6f6f7f6f6f5f5f4f4f2f1eeebe8e6e1dad5cdc8bfb6a7a095773a2a1d1d130b0b050000030a0f0f14161e638e97a2afb6bec7d0d6dbe0e5eaedeff3f3f5f7f7f8f8f8f8f8f9f7f7f7f6f5f3f2efebe7e3ded8cfc7bfb5ab9b96682a232118130e09000008080d14161f518a949da9b2bec7d1d5dee2e8eceef2f4f7f7f8f9f9fafafafbfbfbfafaf9f9f7f6f5f4f1eeebe4ded7d2c7bdb2a5a4925c29271a180d090000080a1514183a80959fa8b5bfc9ced6dde3e9edf1f4f6f8f9f9fafcfcfcfdfdfdfdfdfcfcfbfbfbf9f7f6f5f3efeae7dfd9d0cabbb2a79b8242292119100e0000070c12111d6a8d9ba6afbbc3d1d5dce5eaeef1f5f6f9fafbfcfdfdfdfefefefefefefdfefdfcfcfbfaf9f6f6f2f1ebe6dfd7d0c3bab1a2966e2e262317100000090f14204c8e98a4aeb5c0cbd5dee4e9eef1f5f8fafafcfdfdfefefefefefefefefefefefefdfdfdfbfbf9f8f6f3efece5ded4ccc0b7ac9f95522826161600000b121a21779298a9b5c1cbd2dbe0e7edf0f4f8fafbfdfdfefefefefefefefefefefefefefefefefefdfcfbfaf7f6f3efeae3dcd2cabeb4a79a7930232315000011161a47909ca5b2b8c8cfd6e0e6ecf1f4f8fafafdfdfefefefefefefefefefefefefefefefefefefefdfcfbfaf7f6f2eee8e0d8cfc4bba8a19152271f1a00000d112070939caab5bfc9d4dee4eaeef3f7f9fbfdfdfefefefefefefefefefffffefefefefefefefefefefdfcfbfaf8f4f2ece6ddd4cbc0afa798732d1f1b0000111b388b96a0adbac7d0dae1e6eef1f6f9fafdfdfefefefefefefefffffffffffffffffffefefefefefefdfdfcfbfaf6f3efeae1d9cdc5b7aba08d4b29200000111852909da6b5bfccd4dee4ebeff4f8fafcfdfefefefefefefffffffffffffffffffffffffefefefefefefdfdfcfbf9f5f2ece6dbd6c8beafa5975f28200000151e73929fafb8c4cdd9e2e7edf2f6f9fbfdfdfefefefefefffffffffffffffffffffffffffffffefefefefefdfdfcfaf6f3f0e9e1d8cfc2b7a9997a2725000014338995a5adbfc8d3dce3ebf0f5f8fafbfdfefefefefefffffffffffffffffffffffffffffffffffefefefefefdfcfaf9f5f1ece5dbd1c2b9aca08e3b24000015498e92a3b2bdc8d3dee6edf1f7f9fbfcfdfefefefffffffffffffffffffffffffffffffffffffffffefefefefdfdfbf9f7f2ede7ded4c8bcb2a2924e2400001c51909ea8b6c3cdd6e0e8eef4f8fafbfdfefefefefffffffffffffffffffffffffffffffffffffffffefefefefefdfcfaf8f4efe9e1d6c8c0b0a698612b0000186d92a0acb7c3d0dae1eaeff6f8fafcfdfefefefefffffffffffffffffffffffffffffffffffffffffffefefefefdfdfbf8f4f1ebe2d9cec2b6a59774290000167796a1acbcc6d1dae4eaf1f6f8fbfdfefefefefffffffffffffffffffffffffffffffffffffffffffffefefefefdfdfbfaf6f2ece4dad0c6b5a69b802b00001f8398a2adbbc8d3dbe6edf1f7f9fbfdfefefefefffffffffffffffffffffffffffffffffffffffffffffffefefefdfdfbfaf6f2ece4ddd2c3baa99b842c00001e8995a3b0bccad5dde6ecf1f7f9fbfdfefefefefffffffffffffffffffffffffffffffffffffffffffffffefefefefdfbfaf6f3ede5dbd1c7bbab9e8d2a0000208e95a2b2bcc9d3dce6ecf2f7fafbfdfefefefffffffffffffffffffffffffffffffffffffffffffffffffefefefefdfbfaf6f2eee6dcd3c7b9aa9e902c00001f899aa3b0bec9d4dee7ecf2f7f9fbfdfefefefffffffffffffffffffffffffffffffffffffffffffffffffefefefefdfcfaf7f3eee6dcd2c8baaba08f2c0000208794a5b2bdc6d3dce4ecf1f7f9fbfdfefefefffffffffffffffffffffffffffffffffffffffffffffffffefefefefdfbf9f7f3ede6dcd4c7baab9f892a0000227f98a7b1bdc6d1dae5edf1f6f9fbfdfefefefefffffffffffffffffffffffffffffffffffffffffffffffefefefefdfbfaf7f2ece5dcd1c8baab9e832500001778939eacbdc6d2dce5ebf1f6f9fbfdfefefefefffffffffffffffffffffffffffffffffffffffffffffefefefefdfcfbf9f6f1ece3d9d1c3b7a89a812500001a688d9caebbc1cfd9e3eaeef4f7fafcfdfefefefffffffffffffffffffffffffffffffffffffffffffffffefefefdfcfbf8f5f1ebe3daccc2b7a79c7625000013538d9aa8b7c3cdd8e2e8eff3f7fafbfdfefefefefffffffffffffffffffffffffffffffffffffffffffefefefefdfbfaf7f3efe8e1d6cac1b2a2985d2d0000163d9098a7b7bcc9d4dfe6ecf1f7f8fbfdfdfefefefffffffffffffffffffffffffffffffffffffffffffefefefefdfbfaf6f2eee6ded4c8bdb0a195492900001a318996a0b3bdc8d0dce4eaf0f5f7fafcfdfefefefefffffffffffffffffffffffffffffffffffffffefefefefdfcfbf8f5f1ece5dad2c7bbaca18f39230000171d73979facb9c4ced9e1e9edf2f7f9fbfdfdfefefefefffffffffffffffffffffffffffffffffffefefefefefdfcfaf7f3efebe2d7cebfb4a7997c281a00001516588d9daab5c0cbd5dde4ebf0f6f8fafdfdfefefefefefffffffffffffffffffffffffffffffefefefefefdfcfaf8f5f1ece7ddd4c9bcb0a2985c271d0000171b368a9ca5afbac6cfdae3e9eef2f6f8fbfdfdfdfefefefefefffffffffffffffffffffffffefefefefefefcfbf9f6f3eee8e2d8d0c4b6a9a08d441f1800001113166c939eabb5c1ced5dde5ebf0f4f7fafbfdfdfefefefefefefefffffffffffffffffefefefefefefdfdfbf9f7f4f1ebe5dfd5c9beb0a49a77251f1b00000e141b488f98a1b3bbc5d1d9e2e7ecf1f6f8f9fbfdfdfdfefefefefefefefefefefefefefefefefefefdfdfcfaf7f4f2ece7e0d8cfc5b8ae9f9653251e1900000f0c191e6f94a2aab7c0cad1dce3eaeef2f6f8f9fbfcfdfdfdfefefefefefefefefefefefefefefdfdfdfcfaf8f5f2efeae3dcd2c8beb3a49d7b2e22170f00000b0b1117458c97a6b1b8c3cdd5dde5eaeff3f5f8f9fbfcfdfdfdfdfefefefefefefefefefefefdfdfcfbf9f8f5f3f0ebe4ded6ccc4b6a99d9456231d1b0e00000c070f1820689399a7b5bec4cfd7dfe4ebf0f3f6f8f8f9fcfdfdfdfdfdfdfdfdfefefefdfdfdfcfcfafaf7f5f2efece6e1d8d1c7b9b0a09e772a201a121400000a0d121018397e94a1aab9c0cad1d8e0e5eaeef2f5f7f7f9fbfbfcfcfdfdfdfdfdfdfdfdfcfcfbf9f8f7f4f1efebe6e0d9d3c9c0b5a69a8441231916130e0000030b0d11111c528d99a7b0bcc1c8d1d9dee5e9edf2f3f5f7f8f8fafbfcfcfcfcfcfcfcfcfaf9f8f7f5f2f0ede9e6e0d8d3c9c1b5ac9e955c251f1411110b0000060a091114162065939ba7a9b8c2c9d1d8dee2e8eaeff2f3f5f6f7f8f8f8f8f9faf9f9f7f7f6f5f4f1efece8e4dfd9d1c6c5b7ad9e9a6c271f1417120b0600000709081111101a2e6c92a0a4aebabfc8d0d6dbe2e5e9eceff1f3f4f5f5f6f6f5f6f6f6f5f3f2f1efede9e6e2ded9d2cac0b8ada197782f251c13110f0d0800000402040c0a1518183477949da7afb8c1c8d0d5dae0e2e6e9ebeeeff0f2f2f3f2f2f1f1f1f0edeceae7e3e0d9d7d1c4c2bab0a59a7d3d261b1911100b080900000304090a0c0d111719377c949ba8abb5c0c5cbd0d7dbe0e0e4e7eaebecededededececebebe8e5e2e0ddd7d1cdc4beb4aba49a7e46271e1b140b11090701000004020409050b0e0f161b366e949da2a8b1bcc1c8cdd2d6dadedfe1e3e6e5e6e7e8e6e6e3e2e1dddad5d1cdc6c1bcb3aca29c7c4425201d140f0d0a0a03050000010202030a0b090d0f161d2c6d8e99a1a7b0b8bec3c8ccd0d3d7d8dadddededfdddeddd9dad8d5cecec8c3bdb4ada79e956d33231a120f0f080b07060303000000020303050509140d14121b1957819699a6adafbbbec1c4c7cdced2d5d3d4d4d5d4d4d0d0ccc7c5c3bcb7b1a8a29c8d5b24201c1c180e0e0d0605030301000001000103000505080c0e161912203d6995989da6aaadb5bcbfc0c3c7c9cccacbcacac9c4c6c3bebcbaafaba79f97764223241f1b1013090d070505030202000000000002000504050a0a0c13171b1f204f7a9799a1a7abaaafb4babbb9bac0bfbebebcbbb5b4b0aca5a49e9a7b5223221c1a13160d0b0708060406020100000000000000000305020a070a0e0e1b141d2521457593999c9fa5aaabadadb1b1b1afb1b2aba7a4a3999997735a2a26181d1213120d0c0a07060104020200000000000000000003010205060611110f1012151b1d213f577d8f969e9c9fa5a2a0a2a1a1a29d9e98957f5e402626211e181b150b0e0b0909020303010000000000000000000000000000020406090a0b090e131015181c1b252e506377828a939291938c857d624d3825211d221a1b14100e08100b0306030700010101000000000000000000000000020303020506090d0a08100d131617191c1d1f1b2223242721262623231d201c1e201a18110000000000000505040303020100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")
_RET_C = ", ".join("0x%02X" % b for b in _RET_TEX)
_lib = read(os.path.join(oot, "src/code/z_player_lib.c"))
_m = re.search(r"BgCheck_AnyLineTest3\(&play->colCtx, &\w+, &\w+, &\w+, &(\w+), [^,]+, [^,]+, [^,]+, [^,]+, &(\w+)\)", _lib)
_POLY, _BG = _m.group(1), _m.group(2)
_inc = read(os.path.join(oot, "include/bgcheck.h"))
_CANHOOK = "SurfaceType_CanHookshot" if "SurfaceType_CanHookshot" in _inc else "SurfaceType_IsHookshotSurface"
PORT_RETICLE = r"""
/* port: mira com cor (SoH BowReticle / HookshotableReticle; tools/oot-patch-soh.py) */
static u8 sPortReticleTex[64 * 64] __attribute__((aligned(8))) = { """ + _RET_C + r""" };
static Vtx sPortReticleVtx[3] = {
    VTX(-50, -30, 0, -0x2AB, 0x800, 0xFF, 0x00, 0x00, 0xFF),
    VTX(50, -30, 0, 0xAAB, 0x800, 0xFF, 0x00, 0x00, 0xFF),
    VTX(0, 70, 0, 0x400, -0x555, 0xFF, 0x00, 0x00, 0xFF),
};

/* desenha a mira com a cor certa; 0 = deixa o jogo desenhar a original (opcoes desligadas) */
static s32 Port_ReticleDraw(PlayState* play, Player* this, CollisionPoly* poly, s32 bgId) {
    s32 hook = ({ extern int port_opt_hookshot_reticle_color; port_opt_hookshot_reticle_color; }) &&
               (this->heldItemAction == PLAYER_IA_HOOKSHOT || this->heldItemAction == PLAYER_IA_LONGSHOT);
    s32 green;

    if (!hook) {
        return 0;
    }
    green = hook && """ + _CANHOOK + r"""(&play->colCtx, poly, bgId);
    OPEN_DISPS(play->state.gfxCtx, "../z_player_lib.c", 0);
    gSPMatrix(OVERLAY_DISP++, 0x01000000, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gSPTexture(OVERLAY_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(OVERLAY_DISP++, sPortReticleTex, G_IM_FMT_I, G_IM_SIZ_8b, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
    if (green) {
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 0, 255, 0, 255);
    } else {
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 0, 0, 255);
    }
    gSPVertex(OVERLAY_DISP++, sPortReticleVtx, 3, 0);
    gSP1Triangle(OVERLAY_DISP++, 0, 1, 2, 0);
    CLOSE_DISPS(play->state.gfxCtx, "../z_player_lib.c", 0);
    return 1;
}

/* mira do arco/estilingue (SoH BowReticle). Em 1a pessoa o tiro vai para o centro da tela: a mira e um ponto 2D fixo no
 * centro (sem atraso com os quadros intermediarios, sem sumir mirando o ceu e sempre do mesmo tamanho) */
static void Port_BowReticle(PlayState* play, Player* this) {
    s32 px, x0, y0, x1, y1, dsdx;
    f32 size = 5.0f;

    px = (s32)(size + 0.5f);
    x0 = 160 - px / 2;
    y0 = 120 - px / 2;
    x1 = x0 + px;
    y1 = y0 + px;
    dsdx = (64 << 10) / px;
    OPEN_DISPS(play->state.gfxCtx, "../z_player_lib.c", 0);
    gDPPipeSync(OVERLAY_DISP++);
    gDPSetCycleType(OVERLAY_DISP++, G_CYC_1CYCLE);
    gDPSetRenderMode(OVERLAY_DISP++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE,
                      0);
    gDPSetTexturePersp(OVERLAY_DISP++, G_TP_NONE);
    gDPSetTextureFilter(OVERLAY_DISP++, G_TF_BILERP);
    gDPSetTextureLUT(OVERLAY_DISP++, G_TT_NONE);
    gDPSetAlphaCompare(OVERLAY_DISP++, G_AC_NONE);
    gDPLoadTextureBlock(OVERLAY_DISP++, sPortReticleTex, G_IM_FMT_I, G_IM_SIZ_8b, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 0, 0, 255);
    gSPTextureRectangle(OVERLAY_DISP++, x0 << 2, y0 << 2, x1 << 2, y1 << 2, G_TX_RENDERTILE, 0, 0, dsdx, dsdx);
    gDPPipeSync(OVERLAY_DISP++);
    gDPSetTexturePersp(OVERLAY_DISP++, G_TP_PERSP);
    CLOSE_DISPS(play->state.gfxCtx, "../z_player_lib.c", 0);
}

"""
patch_file("src/code/z_player_lib.c", [
    ("SoH reticulos: funcoes",
     "void Player_DrawHookshotReticle(PlayState* play, Player* this, f32 arg2) {\n",
     PORT_RETICLE + "void Player_DrawHookshotReticle(PlayState* play, Player* this, f32 arg2) {\n"),
    ("SoH reticulos: cor",
     "        gSPDisplayList(OVERLAY_DISP++, gLinkAdultHookshotReticleDL);\n",
     "        if (!Port_ReticleDraw(play, this, " + _POLY + ", " + _BG + ")) { /* port: SoH */\n"
     "            gSPDisplayList(OVERLAY_DISP++, gLinkAdultHookshotReticleDL);\n"
     "        }\n"),
    ("SoH BowReticle: arco e estilingue",
     "            if ((this->heldItemAction == PLAYER_IA_HOOKSHOT) || (this->heldItemAction == PLAYER_IA_LONGSHOT)) {\n"
     "                static Vec3f D_80126184",
     "            if (({ extern int port_opt_bow_reticle; port_opt_bow_reticle; }) && /* port: SoH BowReticle */\n"
     "                (((this->heldItemAction >= PLAYER_IA_BOW) && (this->heldItemAction <= PLAYER_IA_BOW_LIGHT)) ||\n"
     "                 (this->heldItemAction == PLAYER_IA_SLINGSHOT))) {\n"
     "                s32 portAim = (this->unk_6AD == 2) && (this->stateFlags1 & PLAYER_STATE1_3);\n"
     "                if (portAim) {\n"
     "                    Port_BowReticle(play, this);\n"
     "                }\n"
     "            }\n"
     "            if ((this->heldItemAction == PLAYER_IA_HOOKSHOT) || (this->heldItemAction == PLAYER_IA_LONGSHOT)) {\n"
     "                static Vec3f D_80126184"),
])

# Ciclo de flechas (SoH BowArrowCycle): mirando com o arco (adulto), o R troca a flecha (normal, fogo, gelo, luz) entre
# as que o Link tem e com magia suficiente. A magia so e gasta quando a flecha sai (nao ao puxar).
PORT_ARROWCYCLE = r"""
/* port: SoH BowArrowCycle (soh/Enhancements/Items/ArrowCycle.cpp) */
static s32 Port_ArrowHas(s32 ia) {
    switch (ia) {
        case PLAYER_IA_BOW:
            return true;
        case PLAYER_IA_BOW_FIRE:
            return INV_CONTENT(ITEM_ARROW_FIRE) == ITEM_ARROW_FIRE && gSaveContext.save.info.playerData.magic >= sMagicArrowCosts[0];
        case PLAYER_IA_BOW_ICE:
            return INV_CONTENT(ITEM_ARROW_ICE) == ITEM_ARROW_ICE && gSaveContext.save.info.playerData.magic >= sMagicArrowCosts[1];
        case PLAYER_IA_BOW_LIGHT:
            return INV_CONTENT(ITEM_ARROW_LIGHT) == ITEM_ARROW_LIGHT && gSaveContext.save.info.playerData.magic >= sMagicArrowCosts[2];
    }
    return false;
}

static void Port_ArrowCycle(Player* this, PlayState* play, Input* input) {
    static const s8 order[] = { PLAYER_IA_BOW, PLAYER_IA_BOW_FIRE, PLAYER_IA_BOW_ICE, PLAYER_IA_BOW_LIGHT };
    static const u8 bowItem[] = { ITEM_BOW, ITEM_BOW_FIRE, ITEM_BOW_ICE, ITEM_BOW_LIGHT };
    static const u8 arrowParams[] = { ARROW_NORMAL, ARROW_FIRE, ARROW_ICE, ARROW_LIGHT };
    Actor* arrow = this->heldActor;
    s32 cur = 0, next = 0, i, k;

    if (!CHECK_BTN_ANY(input->press.button, BTN_R) || !LINK_IS_ADULT || gSaveContext.minigameState != 0 ||
        play->sceneId == SCENE_SHOOTING_GALLERY || (this->stateFlags1 & PLAYER_STATE1_23) || this->rideActor != NULL ||
        INV_CONTENT(ITEM_BOW) != ITEM_BOW ||
        (INV_CONTENT(ITEM_ARROW_FIRE) != ITEM_ARROW_FIRE && INV_CONTENT(ITEM_ARROW_ICE) != ITEM_ARROW_ICE &&
         INV_CONTENT(ITEM_ARROW_LIGHT) != ITEM_ARROW_LIGHT) ||
        this->heldItemAction < PLAYER_IA_BOW || this->heldItemAction > PLAYER_IA_BOW_LIGHT ||
        !((this->unk_6AD == 2) || (this->upperActionFunc == func_808351D4)) ||
        arrow == NULL || arrow->id != ACTOR_EN_ARROW) {
        return;
    }
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    for (i = 0; i < 4; i++) {
        if (order[i] == this->heldItemAction) {
            cur = i;
        }
    }
    for (k = 1; k <= 4; k++) {
        if (Port_ArrowHas(order[(cur + k) % 4])) {
            next = (cur + k) % 4;
            break;
        }
    }
    this->heldItemAction = order[next];
    this->itemAction = order[next];
    if (arrow->child != NULL) {
        Actor_Kill(arrow->child);
        arrow->child = NULL;
    }
    arrow->params = arrowParams[next];
    EnArrow_Init(arrow, play);
    for (i = 1; i <= 3; i++) {
        u8 it = gSaveContext.save.info.equips.buttonItems[i];
        if (it == ITEM_BOW || (it >= ITEM_BOW_FIRE && it <= ITEM_BOW_LIGHT)) {
            gSaveContext.save.info.equips.buttonItems[i] = bowItem[next];
            gSaveContext.save.info.equips.cButtonSlots[i - 1] = SLOT_BOW;
            Interface_LoadItemIcon1(play, i);
            gSaveContext.buttonStatus[i] = BTN_ENABLED;
        }
    }
    input->cur.button &= ~BTN_R; /* (o R nao levanta o escudo) */
    input->press.button &= ~BTN_R;
}

"""
_pl = "src/overlays/actors/ovl_player_actor/z_player.c"
patch_file(_pl, [
    ("SoH BowArrowCycle: funcoes",
     "void Player_UpdateCommon(Player* this, PlayState* play, Input* input) {\n",
     PORT_ARROWCYCLE + "void Player_UpdateCommon(Player* this, PlayState* play, Input* input) {\n"),
    ("SoH BowArrowCycle: R",
     "        if (!(this->stateFlags3 & PLAYER_STATE3_2)) {\n            this->actionFunc(this, play);\n",
     "        if (" + O("arrow_cycle") + " && input != NULL) { /* port: SoH BowArrowCycle */\n"
     "            Port_ArrowCycle(this, play, input);\n"
     "        }\n"
     "        if (!(this->stateFlags3 & PLAYER_STATE3_2)) {\n            this->actionFunc(this, play);\n"),
    ("SoH BowArrowCycle: magia so ao atirar",
     "                    if ((magicArrowType >= 0) && (magicArrowType <= 2) &&\n"
     "                        !Magic_RequestChange(play, sMagicArrowCosts[magicArrowType], MAGIC_CONSUME_NOW)) {\n"
     "                        arrowType = ARROW_NORMAL;\n"
     "                    }\n",
     "                    if ((magicArrowType >= 0) && (magicArrowType <= 2) && " + O("arrow_cycle") + ") { /* port: SoH */\n"
     "                        if (gSaveContext.save.info.playerData.magic < sMagicArrowCosts[magicArrowType]) {\n"
     "                            arrowType = ARROW_NORMAL;\n"
     "                        }\n"
     "                    } else if ((magicArrowType >= 0) && (magicArrowType <= 2) &&\n"
     "                               !Magic_RequestChange(play, sMagicArrowCosts[magicArrowType], MAGIC_CONSUME_NOW)) {\n"
     "                        arrowType = ARROW_NORMAL;\n"
     "                    }\n"),
])
patch_file("src/overlays/actors/ovl_En_Arrow/z_en_arrow.c", [
    ("SoH BowArrowCycle: magia ao atirar",
     "            case ARROW_FIRE:\n            case ARROW_ICE:\n            case ARROW_LIGHT:\n"
     "                Player_PlaySfx(player, NA_SE_IT_MAGIC_ARROW_SHOT);\n",
     "            case ARROW_FIRE:\n            case ARROW_ICE:\n            case ARROW_LIGHT:\n"
     "                if (" + O("arrow_cycle") + ") { /* port: SoH (custos 4, 4 e 8, como no z_player.c) */\n"
     "                    Magic_RequestChange(play, (this->actor.params == ARROW_LIGHT) ? 8 : 4, 0 /* MAGIC_CONSUME_NOW */);\n"
     "                }\n"
     "                Player_PlaySfx(player, NA_SE_IT_MAGIC_ARROW_SHOT);\n"),
])

# ---------------------------------------------------------------- lote 136
# Viagem no tempo pela Cancao do Tempo (SoH TimeTravel, soh/Enhancements/QoL/OcarinaTimeTravel.cpp e SwitchAge.cpp):
# com a Ocarina do Tempo, tocar a Cancao do Tempo longe dos lugares em que ela ja faz algo (blocos do tempo, porta do
# tempo, pontos de tocar ocarina, sapos, pedras fofoqueiras) troca a idade do Link, que volta no mesmo lugar.
PORT_TT = r"""
/* port: SoH TimeTravel / SwitchAge */
static void Port_OcarinaTimeTravel(PlayState* play) {
    extern int port_tt_novoid;
    Player* player = GET_PLAYER(play);
    Actor* pa = &player->actor;

    if (!({ extern int port_opt_time_travel; port_opt_time_travel; }) || play->msgCtx.lastPlayedSong != OCARINA_SONG_TIME ||
        INV_CONTENT(ITEM_OCARINA_OF_TIME) != ITEM_OCARINA_OF_TIME ||
        Actor_FindNearby(play, pa, ACTOR_OBJ_WARP2BLOCK, ACTORCAT_ITEMACTION, 300.0f) != NULL ||
        Actor_FindNearby(play, pa, ACTOR_OBJ_TIMEBLOCK, ACTORCAT_ITEMACTION, 300.0f) != NULL ||
        Actor_FindNearby(play, pa, ACTOR_EN_OKARINA_TAG, ACTORCAT_PROP, 120.0f) != NULL ||
        Actor_FindNearby(play, pa, ACTOR_DOOR_TOKI, ACTORCAT_BG, 500.0f) != NULL ||
        Actor_FindNearby(play, pa, ACTOR_EN_FR, ACTORCAT_NPC, 300.0f) != NULL ||
        Actor_FindNearby(play, pa, ACTOR_EN_GS, ACTORCAT_NPC, 300.0f) != NULL) {
        return;
    }
    if (play->sceneId == SCENE_HYRULE_CASTLE || play->sceneId == SCENE_OUTSIDE_GANONS_CASTLE) {
        /* (ali o Link quase sempre cai pelo chao: vai para a saida sul do castelo, como no SoH) */
        play->nextEntranceIndex = ENTR_CASTLE_GROUNDS_SOUTH_EXIT;
    } else {
        gSaveContext.respawnFlag = 1;
        play->nextEntranceIndex = gSaveContext.save.entranceIndex;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = play->nextEntranceIndex;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = play->roomCtx.curRoom.num;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = pa->world.pos;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = pa->shape.rot.y;
        gSaveContext.entranceSpeed = 0.0f;
        if (play->roomCtx.curRoom.environmentType < 4) {
            gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0DFF;
        } else {
            gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0D00 | GET_ACTIVE_CAM(play)->bgCamIndex;
        }
        port_tt_novoid = 1; /* (a volta no mesmo lugar nao tira coracao, como uma queda no vazio tiraria) */
    }
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    play->linkAgeOnLoad ^= 1;
}

"""
_entr = read(os.path.join(oot, "include/tables/entrance_table.h"))
_SOUTH = "ENTR_CASTLE_GROUNDS_SOUTH_EXIT" if "ENTR_CASTLE_GROUNDS_SOUTH_EXIT" in _entr else "ENTR_HYRULE_CASTLE_1"
PORT_TT = PORT_TT.replace("ENTR_CASTLE_GROUNDS_SOUTH_EXIT", _SOUTH)
patch_file("src/code/z_message.c", [
    ("SoH TimeTravel: funcao",
     "void Message_OpenText(PlayState* play, u16 textId) {\n",
     PORT_TT + "void Message_OpenText(PlayState* play, u16 textId) {\n"),
    ("SoH TimeTravel: depois de tocar",
     "                        PRINTF(\"→  OCARINA_MODE=%d\\n\", play->msgCtx.ocarinaMode);\n                    }\n",
     "                        PRINTF(\"→  OCARINA_MODE=%d\\n\", play->msgCtx.ocarinaMode);\n                    }\n"
     "                    Port_OcarinaTimeTravel(play); /* port: SoH TimeTravel */\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH TimeTravel: sem dano de queda",
     "            if ((respawnFlag == 1) || (respawnFlag == -1)) {\n                this->unk_A86 = -2;\n            }\n",
     "            if (((respawnFlag == 1) || (respawnFlag == -1)) &&\n"
     "                !({ extern int port_tt_novoid; s32 portNv = port_tt_novoid; port_tt_novoid = 0; portNv; })) { /* port */\n"
     "                this->unk_A86 = -2;\n            }\n"),
])

# Teleporte pelo menu de pausa (SoH PauseWarp, soh/Enhancements/QoL/PauseWarp.cpp): na pagina de missao, com a ocarina,
# o A numa musica de teleporte aprendida fecha o menu, toca a musica e pergunta "teleportar para ...?" (a mensagem do
# jogo); o "sim" leva ao destino.
PORT_PW = r"""
/* port: SoH PauseWarp */
s32 port_pausewarp_active;

void Port_PauseWarpSelect(PlayState* play) {
    static const u16 bgm[] = { NA_BGM_OCA_MINUET, NA_BGM_OCA_BOLERO, NA_BGM_OCA_SERENADE,
                               NA_BGM_OCA_REQUIEM, NA_BGM_OCA_NOCTURNE, NA_BGM_OCA_LIGHT };
    PauseContext* pauseCtx = &play->pauseCtx;
    s32 song = pauseCtx->cursorPoint[PAUSE_QUEST];
    s32 idx;

    if (!({ extern int port_opt_pause_warp; port_opt_pause_warp; }) ||
        gSaveContext.save.info.inventory.items[SLOT_OCARINA] == ITEM_NONE ||
        !CHECK_BTN_ALL(play->state.input[0].press.button, BTN_A) || pauseCtx->pageIndex != PAUSE_QUEST ||
        pauseCtx->state != PAUSE_STATE_MAIN || song < QUEST_SONG_MINUET || song > QUEST_SONG_PRELUDE ||
        !CHECK_QUEST_ITEM(song)) {
        return;
    }
    idx = song - QUEST_SONG_MINUET;
    AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_OFF);
    Interface_SetDoAction(play, DO_ACTION_NONE);
    pauseCtx->state = PAUSE_STATE_CLOSING;
    WREG(2) = -6240;
    func_800F64E0(0);
    pauseCtx->mainState = PAUSE_MAIN_STATE_IDLE;
    play->msgCtx.lastPlayedSong = OCARINA_SONG_MINUET + idx;
    Audio_SetSfxBanksMute(0x20);
    Audio_PlayFanfare(bgm[idx]);
    Message_StartTextbox(play, 0x088D + idx, NULL); /* "teleportar para ...?" (sim/nao) */
    GET_PLAYER(play)->stateFlags1 |= PLAYER_STATE1_29;
    port_pausewarp_active = 1;
}

"""
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("SoH PauseWarp: funcao", "void KaleidoScope_Update(PlayState* play) {\n",
     PORT_PW + "void KaleidoScope_Update(PlayState* play) {\n"),
    ("SoH PauseWarp: A na musica",
     "    if ((R_PAUSE_BG_PRERENDER_STATE >= PAUSE_BG_PRERENDER_READY) &&\n"
     "        (((pauseCtx->state >= PAUSE_STATE_OPENING_1) && (pauseCtx->state <= PAUSE_STATE_SAVE_PROMPT)) ||\n",
     "    Port_PauseWarpSelect(play); /* port: SoH PauseWarp */\n"
     "    if ((R_PAUSE_BG_PRERENDER_STATE >= PAUSE_BG_PRERENDER_READY) &&\n"
     "        (((pauseCtx->state >= PAUSE_STATE_OPENING_1) && (pauseCtx->state <= PAUSE_STATE_SAVE_PROMPT)) ||\n"),
])
_pw_exec = r"""
/* port: SoH PauseWarp (a resposta da pergunta, quando a caixa de texto fecha) */
static void Port_PauseWarpExecute(PlayState* play) {
    extern s32 port_pausewarp_active;
    static const s16 entr[] = { ENTR_SACRED_FOREST_MEADOW_2, ENTR_DEATH_MOUNTAIN_CRATER_4, ENTR_LAKE_HYLIA_8,
                                ENTR_DESERT_COLOSSUS_5, ENTR_GRAVEYARD_7, ENTR_TEMPLE_OF_TIME_7 };
    s32 i;

    if (!port_pausewarp_active || play->msgCtx.msgMode != MSGMODE_NONE) {
        return;
    }
    port_pausewarp_active = 0;
    GET_PLAYER(play)->stateFlags1 &= ~PLAYER_STATE1_29;
    if (play->msgCtx.choiceIndex != 0) {
        return;
    }
    i = play->msgCtx.lastPlayedSong - OCARINA_SONG_MINUET;
    if (i < 0 || i > 5) {
        return;
    }
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_FADE_WHITE_FAST;
    play->nextEntranceIndex = entr[i];
    Interface_SetSubTimerToFinalSecond(play);
}

"""
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH PauseWarp: funcao", "void Player_UpdateCommon(Player* this, PlayState* play, Input* input) {\n",
     _pw_exec + "void Player_UpdateCommon(Player* this, PlayState* play, Input* input) {\n"),
    ("SoH PauseWarp: a cada quadro",
     "        if (" + O("arrow_cycle") + " && input != NULL) { /* port: SoH BowArrowCycle */\n",
     "        Port_PauseWarpExecute(play); /* port: SoH PauseWarp */\n"
     "        if (" + O("arrow_cycle") + " && input != NULL) { /* port: SoH BowArrowCycle */\n"),
])

# Escolher a mascara (SoH MaskSelect): depois de pagar todas as mascaras da loja (o SoH pede a quitacao do Bunny Hood,
# a carta da Zelda e o portao de Kakariko aberto), no menu de itens o A em cima da mascara troca para a proxima
# (Keaton, Caveira, Assustadora, Coelho, Goron, Zora, Gerudo, Verdade). O SoH mostra as vizinhas e anda com o analogico;
# aqui e o A, que nessa tela nao fazia nada. O botao C com a mascara troca junto.
PORT_MASK = r"""
/* port: SoH MaskSelect */
void Port_MaskSelect(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    s32 cur = INV_CONTENT(ITEM_TRADE_CHILD), next, i;

    if (!({ extern int port_opt_mask_select; port_opt_mask_select; }) || pauseCtx->state != PAUSE_STATE_MAIN ||
        pauseCtx->pageIndex != PAUSE_ITEM || pauseCtx->mainState != PAUSE_MAIN_STATE_IDLE ||
        pauseCtx->cursorSlot[PAUSE_ITEM] != SLOT_TRADE_CHILD ||
        !CHECK_BTN_ALL(play->state.input[0].press.button, BTN_A) || cur < ITEM_MASK_KEATON || cur > ITEM_MASK_TRUTH ||
        !GET_EVENTCHKINF(EVENTCHKINF_PAID_BACK_BUNNY_HOOD) || !GET_EVENTCHKINF(EVENTCHKINF_OBTAINED_ZELDAS_LETTER) ||
        !GET_INFTABLE(INFTABLE_76)) {
        return;
    }
    next = (cur >= ITEM_MASK_TRUTH) ? ITEM_MASK_KEATON : cur + 1;
    Sfx_PlaySfxCentered(NA_SE_SY_CURSOR);
    for (i = 1; i < 4; i++) {
        if (gSaveContext.save.info.equips.buttonItems[i] == cur) {
            gSaveContext.save.info.equips.buttonItems[i] = next;
            Interface_LoadItemIcon1(play, i);
        }
    }
    gSaveContext.save.info.inventory.items[SLOT_TRADE_CHILD] = next;
}

"""
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("SoH MaskSelect: funcao", "void KaleidoScope_Update(PlayState* play) {\n",
     PORT_MASK + "void KaleidoScope_Update(PlayState* play) {\n"),
    ("SoH MaskSelect: A na mascara",
     "    Port_PauseWarpSelect(play); /* port: SoH PauseWarp */\n",
     "    Port_PauseWarpSelect(play); /* port: SoH PauseWarp */\n"
     "    Port_MaskSelect(play); /* port: SoH MaskSelect */\n"),
])

# Bunny Hood do Majora's Mask (SoH MMBunnyHood, modo "corre mais rapido"): com a mascara do coelho o Link corre 1,5x
# mais rapido, e os personagens nao reagem a ela (a mascara nao atrapalha conversas).
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH MMBunnyHood: corrida",
     "        if (!func_8083C484(this, &speedTarget, &yawTarget)) {\n            func_8083DF68(this, speedTarget, yawTarget);\n",
     "        if (!func_8083C484(this, &speedTarget, &yawTarget)) {\n"
     "            if (" + O("mm_bunny_hood") + " && this->currentMask == PLAYER_MASK_BUNNY) { /* port: SoH MMBunnyHood */\n"
     "                speedTarget *= 1.5f;\n"
     "            }\n"
     "            func_8083DF68(this, speedTarget, yawTarget);\n"),
])
patch_file("src/code/z_face_reaction.c", [
    ("SoH MMBunnyHood: sem reacao",
     "    u8 currentMask = Player_GetMask(play);\n\n    return sMaskReactionSetTextIds[maskReactionSet][currentMask];\n",
     "    u8 currentMask = Player_GetMask(play);\n\n"
     "    if (" + O("mm_bunny_hood") + " && currentMask == PLAYER_MASK_BUNNY) { /* port: SoH MMBunnyHood */\n"
     "        return 0;\n"
     "    }\n"
     "    return sMaskReactionSetTextIds[maskReactionSet][currentMask];\n"),
])

# Perguntar para equipar (SoH AskToEquip): ao ganhar espada, escudo ou tunica que a idade atual pode usar, a mensagem
# ganha uma caixa a mais: "Quer equipar agora? Sim / Nao". O "sim" equipa na hora.
PORT_ASKEQ = r"""
/* port: SoH AskToEquip - a pergunta no fim da mensagem do item (ingles, portugues sem acentos, japones) */
static void Port_MsgAskEquip(PlayState* play, u16 textId, u8* buf, s32 cap) {
    static const u8 en[] = { @@EN@@ };
    static const u8 pt[] = { @@PT@@ };
    static const u8 de[] = { @@DE@@ };
    static const u8 fr[] = { @@FR@@ };
    static const u8 it[] = { @@IT@@ };
    static const u8 es[] = { @@ES@@ };
    static const u16 jp[] = { @@JP@@ };
    MessageContext* msgCtx = &play->msgCtx;
    s32 adult = LINK_IS_ADULT, i, end, lines, n;

    if (!({ extern int port_opt_ask_to_equip; port_opt_ask_to_equip; }) ||
        !((adult && (textId == 0x0C || textId == 0x4B || textId == 0x4E || textId == 0x50 || textId == 0x51)) ||
          (!adult && (textId == 0x4C || textId == 0xA4)) || textId == 0x4D)) {
        return;
    }
    if (gSaveContext.language == LANGUAGE_JPN) {
        u16* w = (u16*)buf;
        s32 nw = msgCtx->msgLength / 2;
        for (end = 0; end < nw && w[end] != 0x8170; end++) {
        }
        if (end >= nw || (nw + (s32)ARRAY_COUNT(jp)) * 2 > cap) {
            return;
        }
        for (i = nw - 1; i >= end; i--) {
            w[i + ARRAY_COUNT(jp)] = w[i];
        }
        for (i = 0; i < (s32)ARRAY_COUNT(jp); i++) {
            w[end + i] = jp[i];
        }
        msgCtx->msgLength += sizeof(jp);
    } else {
        extern int port_opt_idioma;
        const u8* s = port_opt_idioma == 1 ? pt : port_opt_idioma == 3 ? de : port_opt_idioma == 4 ? fr
                    : port_opt_idioma == 5 ? it : port_opt_idioma == 6 ? es : en;
        n = port_opt_idioma == 1 ? (s32)sizeof(pt) : port_opt_idioma == 3 ? (s32)sizeof(de)
          : port_opt_idioma == 4 ? (s32)sizeof(fr) : port_opt_idioma == 5 ? (s32)sizeof(it)
          : port_opt_idioma == 6 ? (s32)sizeof(es) : (s32)sizeof(en);
        end = Port_MsgEnd(buf, msgCtx->msgLength, &lines);
        if (end < 0 || msgCtx->msgLength + n > cap) {
            return;
        }
        for (i = msgCtx->msgLength - 1; i >= end; i--) {
            buf[i + n] = buf[i];
        }
        for (i = 0; i < n; i++) {
            buf[end + i] = s[i];
        }
        msgCtx->msgLength += n;
    }
    play->msgCtx.font.msgLength = msgCtx->msgLength;
}

"""
def _jpw(s):
    e = s.encode('shift_jis')
    return [(e[i] << 8) | e[i + 1] for i in range(0, len(e), 2)]
def _nesb(l1, y, n):
    b = [0x04] + _pal(l1) + [0x01, 0x01, 0x1B, 0x05, 0x42] + _pal(y) + [0x01] + _pal(n) + [0x05, 0x40]
    return ", ".join("0x%02X" % x for x in b)
_jpq = [0x81A5] + _jpw("すぐに　そうびする？") + [0x000A, 0x81BC, 0x000B, 0x0C02] + _jpw("はい") + [0x000A] + _jpw("いいえ") + [0x000B, 0x0C00]
PORT_ASKEQ = PORT_ASKEQ.replace("@@EN@@", _nesb("Would you like to equip it now?", "Yes", "No")) \
                       .replace("@@PT@@", _nesb("Quer equipar agora?", "Sim", "Nao")) \
                       .replace("@@DE@@", _nesb("Jetzt ausr\u00fcsten?", "Ja", "Nein")) \
                       .replace("@@FR@@", _nesb("L'\u00e9quiper maintenant?", "Oui", "Non")) \
                       .replace("@@IT@@", _nesb("Vuoi equipaggiarlo ora?", "S\u00ef", "No")) \
                       .replace("@@ES@@", _cbytes([0x04] + _es("\u00bfQuieres equiparlo ahora?") + [0x01, 0x01, 0x1B, 0x05, 0x42] +
                                                  _es("S\u00ed") + [0x01] + _es("No") + [0x05, 0x40])) \
                       .replace("@@JP@@", ", ".join("0x%04X" % x for x in _jpq))
patch_file("src/code/z_message.c", [
    ("SoH AskToEquip: mensagem",
     "static void Port_MsgInject(PlayState* play, u16 textId, u8* buf, s32 cap) {\n",
     PORT_ASKEQ + "static void Port_MsgInject(PlayState* play, u16 textId, u8* buf, s32 cap) {\n"),
    ("SoH AskToEquip: chamada",
     "    if (Port_MsgMarketSneak(play, textId, buf, cap)) {\n        return;\n    }\n",
     "    if (Port_MsgMarketSneak(play, textId, buf, cap)) {\n        return;\n    }\n"
     "    Port_MsgAskEquip(play, textId, buf, cap); /* port: SoH AskToEquip */\n"),
])
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("SoH AskToEquip: guarda o item",
     "        giEntry = &sGetItemTable[this->getItemId - 1];\n        this->av1.actionVar1 = 1;\n",
     "        giEntry = &sGetItemTable[this->getItemId - 1];\n        this->av1.actionVar1 = 1;\n"
     "        sPortEquipItem = giEntry->itemId; /* port: SoH AskToEquip */\n"
     "        sPortEquipNow = " + O("ask_to_equip") + " &&\n"
     "                        ((LINK_IS_ADULT && (giEntry->textId == 0x0C || giEntry->textId == 0x4B ||\n"
     "                                            giEntry->textId == 0x4E || giEntry->textId == 0x50 ||\n"
     "                                            giEntry->textId == 0x51)) ||\n"
     "                         (!LINK_IS_ADULT && (giEntry->textId == 0x4C || giEntry->textId == 0xA4)) ||\n"
     "                         giEntry->textId == 0x4D);\n"),
    ("SoH AskToEquip: resposta",
     "    } else {\n        if (Message_GetState(&play->msgCtx) == TEXT_STATE_CLOSING) {\n"
     "            if (this->getItemId == GI_SILVER_GAUNTLETS) {\n",
     "    } else if (sPortEquipNow && Message_GetState(&play->msgCtx) == TEXT_STATE_CHOICE &&\n"
     "               ({ u8 Message_ShouldAdvanceSilent(PlayState * play); Message_ShouldAdvanceSilent(play); })) { /* port */\n"
     "        s32 it = sPortEquipItem;\n"
     "        if (play->msgCtx.choiceIndex == 0) {\n"
     "            if (it >= ITEM_SWORD_KOKIRI && it <= ITEM_SWORD_BIGGORON) {\n"
     "                gSaveContext.save.info.equips.buttonItems[0] = it;\n"
     "                Inventory_ChangeEquipment(EQUIP_TYPE_SWORD, it - ITEM_SWORD_KOKIRI + 1);\n"
     "                Player_PlaySfx(this, NA_SE_IT_SWORD_PUTAWAY);\n"
     "            } else if (it >= ITEM_SHIELD_DEKU && it <= ITEM_SHIELD_MIRROR) {\n"
     "                Inventory_ChangeEquipment(EQUIP_TYPE_SHIELD, it - ITEM_SHIELD_DEKU + 1);\n"
     "                Player_PlaySfx(this, NA_SE_IT_SHIELD_REMOVE);\n"
     "                Player_SetEquipmentData(play, this);\n"
     "            } else if (it == ITEM_TUNIC_GORON || it == ITEM_TUNIC_ZORA) {\n"
     "                Inventory_ChangeEquipment(EQUIP_TYPE_TUNIC, it - ITEM_TUNIC_KOKIRI + 1);\n"
     "                Player_PlaySfx(this, NA_SE_PL_CHANGE_ARMS);\n"
     "                Player_SetEquipmentData(play, this);\n"
     "            }\n"
     "        }\n"
     "        sPortEquipNow = false;\n"
     "        Message_CloseTextbox(play);\n"
     "        play->msgCtx.msgMode = MSGMODE_TEXT_DONE;\n"
     "    } else {\n        if (Message_GetState(&play->msgCtx) == TEXT_STATE_CLOSING) {\n"
     "            if (this->getItemId == GI_SILVER_GAUNTLETS) {\n"),
    ("SoH AskToEquip: variaveis",
     "s32 func_8084DFF4(PlayState* play, Player* this) {\n",
     "static s32 sPortEquipItem, sPortEquipNow; /* port: SoH AskToEquip */\n"
     "s32 func_8084DFF4(PlayState* play, Player* this) {\n"),
])

# Interface minima (SoH MinimalUI): com o minimapa desligado (SELECT, o L do N64) somem os botoes, as rupias e as chaves;
# os coracoes so aparecem quando nao estao cheios e a magia so quando esta sendo gasta. Os itens dos botoes C tambem somem
# (pedido do Regis; no SoH eles ficam).
# Ligar o minimapa (ou abrir o menu) mostra tudo. Comeca valendo na proxima troca de area (como no SoH).
patch_file("src/code/z_construct.c", [
    ("SoH MinimalUI: minimapa comeca desligado",
     "        R_MINIMAP_DISABLED = false;\n",
     "        R_MINIMAP_DISABLED = " + O("minimal_ui") + "; /* port: SoH MinimalUI */\n"),
])
_FULL = "portFull"
patch_file("src/code/z_parameter.c", [
    ("SoH MinimalUI: tudo ou minimo",
     "    OPEN_DISPS(play->state.gfxCtx, \"../z_parameter.c\", 3405);\n",
     "    s32 portFull = !" + O("minimal_ui") + " || !R_MINIMAP_DISABLED || (play->pauseCtx.state != 0); /* port */\n\n"
     "    OPEN_DISPS(play->state.gfxCtx, \"../z_parameter.c\", 3405);\n"),
    ("SoH MinimalUI: coracoes",
     "        Health_DrawMeter(play);\n",
     "        if (portFull || gSaveContext.save.info.playerData.health != gSaveContext.save.info.playerData.healthCapacity) {\n"
     "            Health_DrawMeter(play);\n        }\n"),
    ("SoH MinimalUI: rupias e chaves (comeco)",
     "        // Rupee Icon\n",
     "        if (portFull) { /* port: SoH MinimalUI */\n        // Rupee Icon\n"),
    ("SoH MinimalUI: rupias e chaves (fim) e magia",
     "        Magic_DrawMeter(play);\n",
     "        }\n"
     "        if (portFull || gSaveContext.magicState > MAGIC_STATE_IDLE) {\n"
     "            Magic_DrawMeter(play);\n        }\n"),
    ("SoH MinimalUI: botoes",
     "        Interface_DrawItemButtons(play);\n",
     "        if (portFull) {\n            Interface_DrawItemButtons(play);\n        }\n"),
    ("SoH MinimalUI: icone do B",
     "                Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment, 0);\n",
     "                if (portFull || (player->stateFlags1 & PLAYER_STATE1_23) || (play->shootingGalleryStatus > 1)) {\n"
     "                    Interface_DrawItemIconTexture(play, interfaceCtx->iconItemSegment, 0);\n"
     "                }\n"),
    ("SoH MinimalUI: item do C-esquerda",
     "        if (gSaveContext.save.info.equips.buttonItems[1] < 0xF0) {\n",
     "        if (portFull && gSaveContext.save.info.equips.buttonItems[1] < 0xF0) { /* port: (o SoH deixa os itens dos C) */\n"),
    ("SoH MinimalUI: item do C-baixo",
     "        if (gSaveContext.save.info.equips.buttonItems[2] < 0xF0) {\n",
     "        if (portFull && gSaveContext.save.info.equips.buttonItems[2] < 0xF0) { /* port */\n"),
    ("SoH MinimalUI: item do C-direita",
     "        if (gSaveContext.save.info.equips.buttonItems[3] < 0xF0) {\n",
     "        if (portFull && gSaveContext.save.info.equips.buttonItems[3] < 0xF0) { /* port */\n"),
    ("SoH MinimalUI: botao A",
     "        Interface_DrawActionButton(play);\n",
     "        if (portFull) {\n            Interface_DrawActionButton(play);\n        }\n"),
])

# ---------------------------------------------------------------- idiomas da ROM europeia (port/lang_pal.c)
# Menu de pausa: "To Equip", "To Decide" e "To Play Melody" em alemao e frances sao mais largos que em ingles (na ROM
# europeia cada idioma tem a sua largura). O pacote do idioma traz essas 3 texturas a parte (secao XTEX, lang.c), e
# aqui elas entram no lugar das inglesas, com a largura delas. Sem pacote (ingles, portugues, japones) nada muda.
patch_file("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c", [
    ("idiomas europeus: rotulos largos do menu de pausa",
     "    OPEN_DISPS(play->state.gfxCtx, \"../z_kaleido_scope_PAL.c\", 1676);\n\n"
     "    stepR = ABS(sLRSelectedPrimR - sLRSelectedPrimColors[sLRSelectedPrimState][0]) / sLRSelectedPrimTimer;\n",
     "    OPEN_DISPS(play->state.gfxCtx, \"../z_kaleido_scope_PAL.c\", 1676);\n\n"
     "    { /* port: rotulos do pacote de idioma (alemao/frances) */\n"
     "        extern void* Port_LangExtraTex(int id, void* def, unsigned short* width);\n"
     "        sToEquipTextTextures[LANGUAGE_ENG] = Port_LangExtraTex(0, sToEquipTextTextures[LANGUAGE_ENG], &sToEquipTextWidths[LANGUAGE_ENG]);\n"
     "        sToDecideTextTextures[LANGUAGE_ENG] = Port_LangExtraTex(1, sToDecideTextTextures[LANGUAGE_ENG], &sToDecideTextWidths[LANGUAGE_ENG]);\n"
     "        sToPlayMelodyTextTextures[LANGUAGE_ENG] =\n"
     "            Port_LangExtraTex(2, sToPlayMelodyTextTextures[LANGUAGE_ENG], &sToPlayMelodyTextWidths[LANGUAGE_ENG]);\n"
     "    }\n"
     "    stepR = ABS(sLRSelectedPrimR - sLRSelectedPrimColors[sLRSelectedPrimState][0]) / sLRSelectedPrimTimer;\n"),
])
