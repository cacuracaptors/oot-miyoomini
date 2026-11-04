#!/usr/bin/env python3
"""oot-patch.py: prepara a "arvore do port" a partir do clone do decomp.

NAO altera o seu clone do decomp. Cria (do zero, a cada execucao) uma pasta
TREE com:
  TREE/include/...   copia dos cabecalhos, com IO_READ/IO_WRITE redirecionados
                     para port_io_read/port_io_write (camada de hardware do port)
  TREE/src/...       copia SO dos poucos arquivos do jogo que precisam de ajuste

Cada ajuste e uma troca de texto exata. Se o texto nao for encontrado (ou
aparecer mais de uma vez) o script PARA com erro: assim nunca aplicamos um
patch "no escuro" numa versao diferente do decomp.

uso: oot-patch.py <pasta-do-decomp> <TREE>
"""
import os
import re
import shutil
import sys

oot, tree = sys.argv[1], sys.argv[2]


def read(p):
    with open(p, encoding="utf-8", errors="surrogateescape", newline="") as f:
        return f.read()


def write(p, s):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(s)


def sub(s, old, new, what, expect=1):
    n = s.count(old)
    if n != expect:
        sys.exit("ERRO no patch '%s': texto encontrado %d vezes (esperado %d).\n"
                 "  procurando: %r" % (what, n, expect, old[:120]))
    return s.replace(old, new)


# ---------------------------------------------------------------- includes
if os.path.isdir(tree):
    shutil.rmtree(tree)
os.makedirs(tree)
shutil.copytree(os.path.join(oot, "include"), os.path.join(tree, "include"), symlinks=True)

p = os.path.join(tree, "include/ultra64/rcp.h")
s = read(p)
s = sub(s, "#define IO_READ(addr)       (*(vu32*)PHYS_TO_K1(addr))",
        "extern unsigned int port_io_read(unsigned int);\n"
        "#define IO_READ(addr)       port_io_read(addr)", "rcp.h IO_READ")
s = sub(s, "#define IO_WRITE(addr,data) (*(vu32*)PHYS_TO_K1(addr)=(u32)(data))",
        "extern void port_io_write(unsigned int, unsigned int);\n"
        "#define IO_WRITE(addr,data) port_io_write((addr),(unsigned int)(data))", "rcp.h IO_WRITE")
write(p, s)

# Cabecalhos que assumem big-endian ao empacotar dados (mesmas correcoes que o oot-dc faz).
# Os macros CMD_* montam uma palavra de 32 bits cujos BYTES, na memoria do N64, sao
# [a,b,c,d] / [a_hi,a_lo,b_hi,b_lo]. Em little-endian a palavra precisa ser montada ao
# contrario para que a memoria fique igual e o jogo leia os mesmos campos (u8 / u16).
p = os.path.join(tree, "include/command_macros_base.h")
s = read(p)
s = sub(s, "#define CMD_BBBB(a, b, c, d) (_SHIFTL(a, 24, 8) | _SHIFTL(b, 16, 8) | _SHIFTL(c, 8, 8) | _SHIFTL(d, 0, 8))",
        "#define CMD_BBBB(a, b, c, d) (_SHIFTL(a, 0, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(c, 16, 8) | _SHIFTL(d, 24, 8))",
        "CMD_BBBB")
s = sub(s, "#define CMD_BBH(a, b, c) (_SHIFTL(a, 24, 8) | _SHIFTL(b, 16, 8) | _SHIFTL(c, 0, 16))",
        "#define CMD_BBH(a, b, c) (_SHIFTL(a, 0, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(c, 16, 16))", "CMD_BBH")
s = sub(s, "#define CMD_HBB(a, b, c) (_SHIFTL(a, 16, 16) | _SHIFTL(b, 8, 8) | _SHIFTL(c, 0, 8))",
        "#define CMD_HBB(a, b, c) (_SHIFTL(a, 0, 16) | _SHIFTL(b, 16, 8) | _SHIFTL(c, 24, 8))", "CMD_HBB")
s = sub(s, "#define CMD_HH(a, b) (_SHIFTL(a, 16, 16) | _SHIFTL(b, 0, 16))",
        "#define CMD_HH(a, b) (_SHIFTL(a, 0, 16) | _SHIFTL(b, 16, 16))", "CMD_HH")
write(p, s)

# Musica das cenas: CS_START_SEQ/CS_STOP_SEQ montam a 1a palavra como HH (seqId+1, quadro), mas o jogo le
# como u8 (byte 1) + u16. No N64 da no mesmo (o byte 0 e zero); em little-endian o byte 1 virava 0 e a
# cena pedia a musica 0xFFFF: nao tocava (ex.: a Navi voando ate a casa do Link na abertura).
p = os.path.join(tree, "include/cutscene_commands.h")
s = read(p)
s = s.replace("    CMD_HH((seqId + 1), startFrame), CMD_HH(endFrame, unused0), \\",
              "    CMD_BBH(0, (seqId + 1), startFrame), CMD_HH(endFrame, unused0), /* port */ \\")
if s.count("CMD_BBH(0, (seqId + 1), startFrame)") != 2:
    sys.exit("patch: CS_START_SEQ/CS_STOP_SEQ nao encontrados em cutscene_commands.h")
write(p, s)

# Color_RGBA8_u32: o valor u32 e 0xRRGGBBAA; na memoria little-endian o byte 0 e o A.
# Color_RGBA16: campos de bits comecam pelo bit menos significativo em little-endian.
p = os.path.join(tree, "include/color.h")
s = read(p)
s = sub(s, "    struct {\n        u8 r, g, b, a;\n    };\n    u32 rgba;",
        "    struct {\n        u8 a, b, g, r; /* port: ordem little-endian */\n    };\n    u32 rgba;", "Color_RGBA8_u32")
s = sub(s, "        u16 r : 5;\n        u16 g : 5;\n        u16 b : 5;\n        u16 a : 1;",
        "        u16 a : 1; /* port: ordem little-endian */\n        u16 b : 5;\n        u16 g : 5;\n        u16 r : 5;", "Color_RGBA16")
write(p, s)

# Quadros intermediarios (port/frame_interp.c, ideia do Ship of Harkinian): cada OPEN_DISPS/CLOSE_DISPS
# abre/fecha um "filho" na arvore das matrizes do quadro (chave: arquivo e linha), para achar a mesma
# matriz no quadro anterior.
p = os.path.join(tree, "include/gfx.h")
s = read(p)
s = sub(s, "#define OPEN_DISPS(gfxCtx, file, line)             \\\n"
        "    {                                              \\\n"
        "        UNUSED GraphicsContext* __gfxCtx = gfxCtx; \\\n"
        "        UNUSED s32 __dispPad\n",
        "/* port: quadros intermediarios (port/frame_interp.c) */\n"
        "void Port_FiOpenChild(const void* a, int b);\n"
        "void Port_FiCloseChild(void);\n"
        "void Port_FiStartRecord(void);\n"
        "void Port_FiStopRecord(const void* tag);\n"
        "void Port_FiActorPosRot(void);\n"
        "void Port_FiPush(void);\n"
        "void Port_FiPop(void);\n"
        "void Port_FiMode(int mode);\n"
        "void Port_FiSetTRYXZ(float x, float y, float z, const short* rot, const float* cur);\n"
        "void Port_FiToMtx(const float* cur, void* dest);\n"
        "void Port_FiMtxF(const float* src, void* dest);\n"
        "void Port_FiSkip(int d);\n"
        "int Port_FiCameraEpoch(void);\n"
        "void Port_FiCameraCheck(const float* eye, const float* at, const float* up, int pauseMode);\n"
        "void Port_FiDontInterpolateChild(const void* key);\n"
        "#define OPEN_DISPS(gfxCtx, file, line)             \\\n"
        "    {                                              \\\n"
        "        UNUSED GraphicsContext* __gfxCtx = gfxCtx; \\\n"
        "        UNUSED s32 __dispPad = (Port_FiOpenChild(file, line), 0)\n", "gfx.h OPEN_DISPS")
s = sub(s, "#define CLOSE_DISPS(gfxCtx, file, line) \\\n"
        "        do {} while (0);                \\\n",
        "#define CLOSE_DISPS(gfxCtx, file, line) \\\n"
        "        do { Port_FiCloseChild(); } while (0); \\\n", "gfx.h CLOSE_DISPS")
write(p, s)

# ---------------------------------------------------------------- fontes
patched = []


def patch_file(rel, edits):
    src = os.path.join(oot, rel)
    if os.path.isfile(os.path.join(tree, rel)):
        src = os.path.join(tree, rel)  # ja ajustado antes (outro bloco de patches no mesmo arquivo): continua dele
    s = read(src)
    for ed in edits:
        what, old, new = ed[0], ed[1], ed[2]
        s = sub(s, old, new, "%s: %s" % (rel, what), ed[3] if len(ed) > 3 else 1)  # 4o item opcional: quantas vezes aparece
    write(os.path.join(tree, rel), s)
    patched.append(rel)


# tela de arquivos (o menu azul do "novo jogo"): marca 0x54585440 no comeco do desenho; com a opcao arquivos_320 a
# tela inteira vai em 320x240 (gfx_hires.c), sem a resolucao dinamica trocando de degrau (dava para ver)
patch_file("src/overlays/gamestates/ovl_file_choose/z_file_choose.c", [
    ("tela de arquivos: marca",
     "    gSPSegment(POLY_OPA_DISP++, 0x00, NULL);\n    gSPSegment(POLY_OPA_DISP++, 0x01, this->staticSegment);\n",
     "    { /* port: tela de arquivos (gfx_hires.c, opcao arquivos_320) */\n"
     "        Gfx* portG = POLY_OPA_DISP++;\n"
     "        portG->words.w0 = 0;\n        portG->words.w1 = 0x54585440;\n"
     "    }\n"
     "    gSPSegment(POLY_OPA_DISP++, 0x00, NULL);\n    gSPSegment(POLY_OPA_DISP++, 0x01, this->staticSegment);\n"),
])

# 1) idle.c: o segmento "code" ja esta no executavel (nao ha o que carregar da ROM
#    nem BSS do code para zerar; o BSS nativo ja nasce zerado).
patch_file("src/boot/idle.c", [
    ("nao carregar o segmento code",
     '    DMA_REQUEST_SYNC(_codeSegmentStart, (uintptr_t)_codeSegmentRomStart, _codeSegmentRomEnd - _codeSegmentRomStart,\n'
     '                     "../idle.c", 238);\n',
     "    /* port: o segmento code ja esta no executavel */\n"),
    ("nao zerar o BSS do code",
     "    bzero(_codeSegmentBssStart, _codeSegmentBssEnd - _codeSegmentBssStart);\n",
     "    /* port: BSS nativo ja comeca zerado */\n"),
])

# 2) boot_main.c: nao limpar a RAM a partir de _bootSegmentEnd (nao existe no port).
patch_file("src/boot/boot_main.c", [
    ("bootclear vazio",
     "    bzero(_bootSegmentEnd, osMemSize - OS_K0_TO_PHYSICAL(_bootSegmentEnd));\n",
     "    /* port: a RAM do N64 ja e mapeada zerada */\n"),
])

# 3) menu de pausa (kaleido): overlays ligados estaticamente -> sem copia/relocacao.
patch_file("src/code/z_kaleido_manager.c", [
    ("area minima",
     '    sKaleidoAreaPtr = GAME_STATE_ALLOC(&play->state, largestSize, "../z_kaleido_manager.c", 150);\n',
     "    if (largestSize < 16) {\n        largestSize = 16; /* port: overlays estaticos tem tamanho 0 */\n    }\n\n"
     '    sKaleidoAreaPtr = GAME_STATE_ALLOC(&play->state, largestSize, "../z_kaleido_manager.c", 150);\n'),
    ("GetRamAddr identidade",
     "    KaleidoMgrOverlay* ovl = iter;\n",
     "    KaleidoMgrOverlay* ovl = iter;\n\n"
     "    if (gKaleidoMgrOverlayTable[0].vramStart == NULL) {\n"
     "        return vram; /* port: overlay estatico, o endereco ja e o real */\n"
     "    }\n"),
])

# 4) marcas do mapa: mesma ideia.
patch_file("src/code/z_map_mark.c", [
    ("area minima",
     '    overlay->loadedRamAddr = GAME_STATE_ALLOC(&play->state, overlaySize, "../z_map_mark.c", 235);\n',
     '    overlay->loadedRamAddr = GAME_STATE_ALLOC(&play->state, overlaySize + 16, "../z_map_mark.c", 235);\n'),
    ("tabela direta",
     "#if PLATFORM_N64\n    if ((B_80121220 != NULL) && (B_80121220->unk_2C != NULL)) {\n",
     "    sLoadedMarkDataTable = gMapMarkDataTable; /* port: overlay estatico */\n\n"
     "#if PLATFORM_N64\n    if ((B_80121220 != NULL) && (B_80121220->unk_2C != NULL)) {\n"),
])

# 5) Locale_Init: le o codigo do pais da ROM como bytes de um u32 (assume big-endian).
patch_file("src/boot/z_locale.c", [
    ("pais da ROM em little-endian",
     "    countryCode = regionInfo[2];\n",
     "    countryCode = (u8)((*(u32*)regionInfo) >> 8); /* port: u32 lido da ROM, independente de endian */\n"),
])

# 6) 64DD: nao existe. Nao carrega o segmento n64dd (o destino seria NULL).
patch_file("src/code/code_n64dd_800AD410.c", [
    ("nao carregar o segmento n64dd",
     "        DMA_REQUEST_SYNC(_n64ddSegmentStart, (uintptr_t)_n64ddSegmentRomStart,\n"
     "                         _n64ddSegmentRomEnd - _n64ddSegmentRomStart, UNK_FILE, UNK_LINE);\n"
     "        bzero(_n64ddSegmentBssStart, _n64ddSegmentBssEnd - _n64ddSegmentBssStart);\n",
     "        /* port: sem 64DD, nada a carregar */\n"),
])

# 7a) audio: dados que vem da ROM em big-endian.
#  - soundfonts: convertidas para nativo ao carregar (port/audio/audio_font.c), antes da relocacao;
#  - envelopes ADSR: ficam TODOS em big-endian (os da soundfont, os que moram dentro das sequencias
#    e o gDefaultEnvelope do codigo, que passa a ser escrito em big-endian); o leitor (effects.c)
#    troca os bytes ao ler;
#  - duas leituras de u16 de dentro dos dados da sequencia (seqplayer.c).
p = os.path.join(tree, "include/audio.h")
s = read(p)
s = sub(s, "typedef struct EnvelopePoint {",
        "/* port: um s16 guardado em big-endian (dados de audio da ROM) <-> valor nativo */\n"
        "#define PORT_BE16S(v) ((s16)((((u16)(v) & 0xFF) << 8) | (((u16)(v) >> 8) & 0xFF)))\n"
        "#define PORT_RD_BE16(p) ((u16)((((const u8*)(p))[0] << 8) | ((const u8*)(p))[1]))\n"
        "void port_audio_font_native(void* fontData, int numDrums, int numInstruments, int numSfx);\n\n"
        "typedef struct EnvelopePoint {", "audio.h macros do port")
# Comandos do jogo para a thread de audio (AudioCmd): o jogo monta um u32 (op<<24 | arg0<<16 |
# arg1<<8 | arg2) e um dado de 4 bytes (s8 e u16 vao nos bits altos), e a thread le os campos
# de 1 e 2 bytes pelo nome. Em little-endian esses campos ficam do outro lado da palavra: sem
# isto, nenhum comando chegava certo (nenhuma musica nem efeito comecava a tocar).
s = sub(s, "        struct {\n            u8 op;\n            u8 arg0;\n            u8 arg1;\n            u8 arg2;\n        };\n",
        "        struct { /* port: little-endian */\n            u8 arg2;\n            u8 arg1;\n            u8 arg0;\n"
        "            u8 op;\n        };\n", "AudioCmd op/args")
s = sub(s, "        u16 asUShort;\n        s8 asSbyte;\n        u8 asUbyte;\n",
        "        struct { u16 portPad16_; u16 asUShort; }; /* port: little-endian (bits altos) */\n"
        "        struct { u8 portPad8a_[3]; s8 asSbyte; };\n"
        "        struct { u8 portPad8b_[3]; u8 asUbyte; };\n", "AudioCmd dado pequeno")
# Stereo: o byte vem pronto da sequencia (bits no formato do N64). No GCC little-endian o 1o campo
# de bits fica no bit 0 (no MIPS, no bit 7): declarados ao contrario, ficam nos mesmos bits.
s = sub(s, "typedef struct StereoData {\n"
        "    /* 0x00 */ u8 unused : 2;\n"
        "    /* 0x00 */ u8 bit2 : 2;\n"
        "    /* 0x00 */ u8 strongRight : 1;\n"
        "    /* 0x00 */ u8 strongLeft : 1;\n"
        "    /* 0x00 */ u8 stereoHeadsetEffects : 1;\n"
        "    /* 0x00 */ u8 usesHeadsetPanEffects : 1;\n",
        "typedef struct StereoData { /* port: ordem invertida (little-endian) */\n"
        "    /* 0x00 */ u8 usesHeadsetPanEffects : 1;\n"
        "    /* 0x00 */ u8 stereoHeadsetEffects : 1;\n"
        "    /* 0x00 */ u8 strongLeft : 1;\n"
        "    /* 0x00 */ u8 strongRight : 1;\n"
        "    /* 0x00 */ u8 bit2 : 2;\n"
        "    /* 0x00 */ u8 unused : 2;\n", "StereoData")
write(p, s)

patch_file("src/audio/internal/load.c", [
    ("soundfont para endian nativo",
     "    u32* fontData = (u32*)fontDataStartAddr;\n",
     "    u32* fontData = (u32*)fontDataStartAddr;\n\n"
     "    port_audio_font_native(fontDataStartAddr, numDrums, numInstruments, numSfx); /* port */\n"),
])
# texto japones (codigos de 16 bits) fica na ROM em big-endian: a fonte do teclado de nome
# (versao NTSC) le a mensagem 0xFFFC como u16 -> troca os bytes depois de carregar.
def kanji_tex_c():
    """code/kanji_tex.s (assembly MIPS) reescrito em C, com a tabela lida do proprio .s do decomp."""
    s = read(os.path.join(oot, "src/code/kanji_tex.s"))
    data = s[s.index("DATA(low_codepoints_lookup_table)"):]
    vals = re.findall(r"0x([0-9A-Fa-f]{4})", "\n".join(l for l in data.splitlines() if ".half" in l))
    rows = ["    " + ", ".join("0x" + v for v in vals[i:i + 16]) + "," for i in range(0, len(vals), 16)]
    return ("\n/* port: code/kanji_tex.s em C (codigo Shift-JIS -> posicao da letra na fonte kanji) */\n"
            "static const s16 sPortKanjiLow[%d] = {\n%s\n};\n\n"
            "s32 KanjiTex_CodepointToOffset(s32 codepoint) {\n"
            "    s32 i = (codepoint & 0xFF) - 0x40;\n\n"
            "    if (i >= 0x40) {\n        i -= 1;\n    }\n"
            "    if (codepoint < 0x8800) {\n"
            "        i += ((codepoint >> 8) - 0x81) * 0xBC;\n"
            "        if (i < 0 || i >= %d) {\n            return 0;\n        }\n"
            "        return sPortKanjiLow[i] * FONT_CHAR_TEX_SIZE;\n    }\n"
            "    i += 0x30A + ((codepoint >> 8) - 0x88) * 0xBC;\n"
            "    return i * FONT_CHAR_TEX_SIZE;\n}\n") % (len(vals), "\n".join(rows), len(vals))


patch_file("src/code/z_kanfont.c", [
    ("kanji_tex.s em C",
     "void Font_LoadMessageBoxIcon(Font* font, u16 icon) {\n",
     kanji_tex_c() + "\nvoid Font_LoadMessageBoxIcon(Font* font, u16 icon) {\n"),
    ("mensagem 0xFFFC em endian nativo",
     "    fontBufIndex = 0;\n    for (codePointIndex = 0; font->msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {\n",
     "    for (codePointIndex = 0; codePointIndex < len; codePointIndex++) { /* port: big-endian -> nativo */\n"
     "        u16 w_ = font->msgBufWide[codePointIndex];\n"
     "        font->msgBufWide[codePointIndex] = (u16)((w_ >> 8) | (w_ << 8));\n"
     "    }\n\n"
     "    fontBufIndex = 0;\n    for (codePointIndex = 0; font->msgBufWide[codePointIndex] != MESSAGE_WIDE_END; codePointIndex++) {\n"),
])
# JPEG dos fundos pre-renderizados (casas de Kokiri, casa do Link, mercado, lojas...): a CPU do
# jogo le o arquivo JPEG (bytes da ROM, big-endian) com leituras de u16/u32 nativas.
patch_file("src/code/z_room.c", [
    ("marcador JPEG em bytes",
     "    if (*(u32*)data == JPEG_MARKER) {\n",
     "    if ((u32)((((u8*)data)[0] << 24) | (((u8*)data)[1] << 16) | (((u8*)data)[2] << 8) | ((u8*)data)[3]) ==\n"
     "        JPEG_MARKER) { /* port: big-endian */\n"),
])
patch_file("src/code/z_jpeg.c", [
    ("u16 do cabecalho em big-endian",
     "    if (((uintptr_t)ptr & 1) == 0) {\n",
     "    if (1) { /* port: le os 2 bytes em big-endian, alinhado ou nao */\n"
     "        return (u16)((ptr[0] << 8) | ptr[1]);\n"
     "    } else if (((uintptr_t)ptr & 1) == 0) {\n"),
])
# "x >> -len": no MIPS o deslocamento usa so 5 bits (= x >> (32 - len)); no ARM, com o valor
# negativo, o resultado vira 0 e nenhuma imagem JPEG seria decodificada.
patch_file("src/code/jpegdecoder.c", [
    ("deslocamento negativo",
     "    ret = temp >> -len;\n",
     "    ret = temp >> (32 - len); /* port */\n"),
])
# O gerenciador de som comeca com as tarefas de audio desligadas; quem liga e a tela do logo da
# Nintendo (z_title.c, depois da checagem anti-copia). Com --warp essa tela e pulada e o jogo ficava
# mudo. No port o som ja comeca ligado.
patch_file("src/code/audio_thread_manager.c", [
    ("som ligado desde o inicio",
     "#if PLATFORM_N64\n    R_AUDIOMGR_ACTIVITY_LEVEL = AUDIOMGR_ACTIVITY_LEVEL_NO_RSP;\n#endif\n",
     "    R_AUDIOMGR_ACTIVITY_LEVEL = AUDIOMGR_ACTIVITY_LEVEL_ALL; /* port */\n"),
])
# Tabelas de audio (sequencias, soundfonts, bancos de amostras): o decomp define o cabecalho e as
# entradas como dois objetos separados e conta que o ligador os ponha colados (no N64 ficam). No PC o
# GCC alinha o vetor de entradas a 32 bytes e sobra um buraco de 16: o jogo lia a entrada N-1 no lugar
# da N (fonte errada, banco de amostras errado = som de chiado). Aqui viram um objeto so.
for tbl, name in (("sequence_table.c", "Sequence"), ("soundfont_table.c", "SoundFont"), ("samplebank_table.c", "SampleBank")):
    rel = "src/audio/tables/" + tbl
    s = read(os.path.join(oot, rel))
    s = sub(s, "= s%sTableHeader\n" % name, "= s%sTableAll\n" % name, rel + ": alias")
    s = sub(s, "NO_REORDER AudioTableHeader s%sTableHeader = {\n" % name,
            "NO_REORDER struct { AudioTableHeader h; AudioTableEntry e[]; } s%sTableAll = { { /* port: um objeto so */\n"
            % name, rel + ": cabecalho")
    s = sub(s, "    { 0, 0, 0, 0, 0, 0, 0, 0 },\n};\n\n// Table body\n\nNO_REORDER AudioTableEntry s%sTableEntries[] = {\n" % name,
            "    { 0, 0, 0, 0, 0, 0, 0, 0 },\n}, {\n", rel + ": entradas")
    k = s.rstrip().rfind("};")
    if k < 0:
        sys.exit("ERRO no patch '%s: fim': '};' nao encontrado" % rel)
    s = s[:k] + "} };" + s[k + 2:]
    write(os.path.join(tree, rel), s)
    patched.append(rel)
# Pontos de troca de thread: no N64 o retrace interrompe a CPU a qualquer momento e a thread do som
# (prioridade maior) roda na hora. Aqui as threads do jogo so trocam dentro das chamadas osXxx; num
# quadro pesado a thread grafica passava dezenas de ms sem chamar nenhuma, o som perdia atualizacoes e
# saia picotado. Agora, entre um ator e outro (e entre as etapas de colisao), o jogo entrega a CPU se
# houver retrace ou mensagem pendente. Custa quase nada quando nao ha nada pendente.
PREEMPT = "{ extern void port_preempt_point(void); port_preempt_point(); } /* port */\n"
patch_file("src/code/z_actor.c", [
    ("troca antes de atualizar ator",
     "                    actor->update(actor, play);\n",
     "                    " + PREEMPT + "                    actor->update(actor, play);\n"),
    ("troca antes de desenhar ator",
     "                            Actor_Draw(play, actor);\n",
     "                            " + PREEMPT + "                            Actor_Draw(play, actor);\n"),
    ("vida maxima do ator (barra de vida), Actor_Init",
     "        actor->init(actor, play);\n        actor->init = NULL;\n    }\n}\n",
     "        actor->init(actor, play);\n        actor->init = NULL;\n"
     "        { extern void Port_ActorInitDone(Actor*); Port_ActorInitDone(actor); } /* port */\n    }\n}\n"),
    ("vida maxima do ator (barra de vida), atores esperando o objeto",
     "                    actor->init(actor, play);\n                    actor->init = NULL;\n",
     "                    actor->init(actor, play);\n                    actor->init = NULL;\n"
     "                    { extern void Port_ActorInitDone(Actor*); Port_ActorInitDone(actor); } /* port */\n"),
    # quadros intermediarios: cada ator e um filho da arvore; a posicao/rotacao dele e interpolada como numero
    ("quadros intermediarios: ator abre um filho",
     '    Fault_AddClient(&faultClient, Actor_FaultPrint, actor, "Actor_draw");\n\n'
     '    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 6035);\n',
     '    Fault_AddClient(&faultClient, Actor_FaultPrint, actor, "Actor_draw");\n\n'
     "    Port_FiOpenChild(actor, 0); /* port */\n"
     '    OPEN_DISPS(play->state.gfxCtx, "../z_actor.c", 6035);\n'),
    ("quadros intermediarios: matriz do ator",
     "    if (actor->flags & ACTOR_FLAG_IGNORE_QUAKE) {\n"
     "        Matrix_SetTranslateRotateYXZ(actor->world.pos.x + play->mainCamera.quakeOffset.x,\n",
     "    Port_FiActorPosRot(); /* port */\n"
     "    if (actor->flags & ACTOR_FLAG_IGNORE_QUAKE) {\n"
     "        Matrix_SetTranslateRotateYXZ(actor->world.pos.x + play->mainCamera.quakeOffset.x,\n"),
    ("quadros intermediarios: ator fecha o filho",
     '    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 6119);\n\n    Fault_RemoveClient(&faultClient);\n',
     '    CLOSE_DISPS(play->state.gfxCtx, "../z_actor.c", 6119);\n'
     "    Port_FiCloseChild(); /* port */\n\n    Fault_RemoveClient(&faultClient);\n"),
    ("quadros intermediarios: ator novo nao e interpolado",
     "    Actor_Init(actor, play);\n",
     "    Actor_Init(actor, play);\n    Port_FiDontInterpolateChild(actor); /* port */\n"),
])
patch_file("src/code/z_play.c", [
    ("troca antes das colisoes AT", "    CollisionCheck_AT(this, &this->colChkCtx);\n",
     "    " + PREEMPT + "    CollisionCheck_AT(this, &this->colChkCtx);\n"),
    ("troca antes das colisoes OC", "    CollisionCheck_OC(this, &this->colChkCtx);\n",
     "    " + PREEMPT + "    CollisionCheck_OC(this, &this->colChkCtx);\n"),
    ("troca antes do dano", "    CollisionCheck_Damage(this, &this->colChkCtx);\n",
     "    " + PREEMPT + "    CollisionCheck_Damage(this, &this->colChkCtx);\n"),
    ("troca antes de desenhar os atores", "            Actor_DrawAll(this, &this->actorCtx);\n",
     "            " + PREEMPT + "            Actor_DrawAll(this, &this->actorCtx);\n"),
    # quadros intermediarios: grava as matrizes do desenho de cada quadro (a etiqueta e o display list)
    ("quadros intermediarios: gravacao do quadro",
     "    Play_Draw(this);\n\n    PLAY_LOG(4587);\n",
     "    { /* port: o menu de pausa roda a 30 passos por segundo (R_UPDATE_RATE 2): nao ha o que interpolar */\n"
     "        void Port_FiRate(int rate);\n\n"
     "        Port_FiRate(R_UPDATE_RATE);\n"
     "    }\n"
     "    Port_FiStartRecord(); /* port */\n"
     "    Play_Draw(this);\n"
     "    Port_FiStopRecord(this->state.gfxCtx->workBuffer); /* port */\n\n    PLAY_LOG(4587);\n"),
    # fundo do menu de pausa (port/gfx/gfx_hires.c): o jogo guarda a tela em 320x240 (com um filtro de bordas
    # do N64 feito na CPU) e a redesenha ampliada: fica quadriculada. Marcas: "guarde a imagem maior desta
    # tela" no quadro em que o jogo copia a tela, e "desenhe a guardada" em volta da copia de volta.
    # menu de pausa opaco (opcao menu_opaco): marcas 0x5458543A/B em volta do desenho do menu (KaleidoScopeCall_Draw)
    ("menu opaco: marcas",
     "    if (IS_PAUSED(&this->pauseCtx)) {\n        KaleidoScopeCall_Draw(this);\n    }\n",
     "    if (IS_PAUSED(&this->pauseCtx)) {\n"
     "        { /* port: comeco do menu de pausa (gfx_rdp.c, opcao menu_opaco) */\n"
     "            Gfx* portG = this->state.gfxCtx->polyOpa.p;\n"
     "            portG->words.w0 = 0;\n            portG->words.w1 = 0x5458543A;\n"
     "            this->state.gfxCtx->polyOpa.p = portG + 1;\n"
     "        }\n"
     "        KaleidoScopeCall_Draw(this);\n"
     "        {\n"
     "            Gfx* portG = this->state.gfxCtx->polyOpa.p;\n"
     "            portG->words.w0 = 0;\n            portG->words.w1 = 0x5458543B;\n"
     "            this->state.gfxCtx->polyOpa.p = portG + 1;\n"
     "        }\n"
     "    }\n"),
    # fundo do menu de pausa: com a resolucao maior, a tela do N64 ja recebe a imagem reduzida com media
    # (gfx_hires.c reduce_box); o filtro de bordas do N64 feito na CPU (le a "cobertura" de cada pixel, que aqui nao e
    # a do RDP) so pontilhava e deixava artefatos. Em 320x240 (resolution 320) fica o do jogo.
    ("fundo da pausa: sem o filtro de bordas na resolucao maior",
     "            PreRender_ApplyFilters(&this->pauseBgPreRender);\n",
     "            if (!({ extern int gx_hr_max; gx_hr_max > 320; })) { /* port */\n"
     "                PreRender_ApplyFilters(&this->pauseBgPreRender);\n"
     "            }\n"),
    ("fundo da pausa: guarda a imagem maior",
     "            this->pauseBgPreRender.fbufSave = (u16*)gZBuffer;\n"
     "            PreRender_SaveFramebuffer(&this->pauseBgPreRender, &gfxP);\n",
     "            this->pauseBgPreRender.fbufSave = (u16*)gZBuffer;\n"
     "            gfxP->words.w0 = 0; /* port: guarda a imagem maior (gfx_hires.c) */\n"
     "            gfxP->words.w1 = 0x54585434;\n"
     "            gfxP++;\n"
     "            PreRender_SaveFramebuffer(&this->pauseBgPreRender, &gfxP);\n"),
    ("fundo da pausa: desenha a imagem maior",
     "            PreRender_RestoreFramebuffer(&this->pauseBgPreRender, &gfxP);\n",
     "            gfxP->words.w0 = 0; /* port: no lugar da copia, a imagem maior guardada (gfx_hires.c) */\n"
     "            gfxP->words.w1 = 0x54585435;\n"
     "            gfxP++;\n"
     "            PreRender_RestoreFramebuffer(&this->pauseBgPreRender, &gfxP);\n"
     "            gfxP->words.w0 = 0;\n"
     "            gfxP->words.w1 = 0x54585436;\n"
     "            gfxP++;\n"),
])
# quadros intermediarios: camera (a matriz de visao e gravada; corte brusco de camera nao e interpolado)
patch_file("src/code/z_view.c", [
    ("quadros intermediarios: camera",
     '    LOG_UTILS_CHECK_NULL_POINTER("viewing", viewing, "../z_view.c", 667);\n'
     "    view->viewingPtr = viewing;\n\n"
     "    if (view->eye.x == view->at.x && view->eye.y == view->at.y && view->eye.z == view->at.z) {\n"
     "        view->eye.x += 1.0f;\n"
     "        view->eye.y += 1.0f;\n"
     "        view->eye.z += 1.0f;\n"
     "    }\n\n"
     "    VIEW_ERROR_CHECK_EYE_POS(view->eye.x, view->eye.y, view->eye.z);\n\n"
     "    guLookAt(viewing, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x, view->up.y,\n"
     "             view->up.z);\n",
     '    LOG_UTILS_CHECK_NULL_POINTER("viewing", viewing, "../z_view.c", 667);\n'
     "    view->viewingPtr = viewing;\n\n"
     "    if (view->eye.x == view->at.x && view->eye.y == view->at.y && view->eye.z == view->at.z) {\n"
     "        view->eye.x += 1.0f;\n"
     "        view->eye.y += 1.0f;\n"
     "        view->eye.z += 1.0f;\n"
     "    }\n\n"
     "    VIEW_ERROR_CHECK_EYE_POS(view->eye.x, view->eye.y, view->eye.z);\n\n"
     "    guLookAt(viewing, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x, view->up.y,\n"
     "             view->up.z);\n"
     "    { /* port: quadros intermediarios */\n"
     "        extern void guLookAtF(f32 mf[4][4], f32 xEye, f32 yEye, f32 zEye, f32 xAt, f32 yAt, f32 zAt, f32 xUp, f32 yUp,\n"
     "                              f32 zUp);\n"
     "        MtxF portMf_;\n\n"
     "        Port_FiCameraCheck(&view->eye.x, &view->at.x, &view->up.x, 0);\n"
     "        Port_FiOpenChild(NULL, Port_FiCameraEpoch());\n"
     "        guLookAtF(portMf_.mf, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x,\n"
     "                  view->up.y, view->up.z);\n"
     "        Port_FiMtxF(&portMf_.mf[0][0], viewing);\n"
     "        Port_FiCloseChild();\n"
     "    }\n"),
    # No fim do Play_Draw o jogo atualiza a camera e REESCREVE a mesma Mtx da visao (a do display list
    # deste quadro) com a posicao nova. Sem gravar isto, os quadros intermediarios ficavam um quadro atras
    # do quadro do jogo: na mira/primeira pessoa a imagem ia e voltava ("pulinhos").
    ("quadros intermediarios: camera atualizada no fim do quadro",
     "    guLookAt(view->viewingPtr, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x,\n"
     "             view->up.y, view->up.z);\n",
     "    guLookAt(view->viewingPtr, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x,\n"
     "             view->up.y, view->up.z);\n"
     "    { /* port: quadros intermediarios */\n"
     "        extern void guLookAtF(f32 mf[4][4], f32 xEye, f32 yEye, f32 zEye, f32 xAt, f32 yAt, f32 zAt, f32 xUp, f32 yUp,\n"
     "                              f32 zUp);\n"
     "        MtxF portMf_;\n\n"
     "        Port_FiOpenChild(NULL, Port_FiCameraEpoch());\n"
     "        guLookAtF(portMf_.mf, view->eye.x, view->eye.y, view->eye.z, view->at.x, view->at.y, view->at.z, view->up.x,\n"
     "                  view->up.y, view->up.z);\n"
     "        Port_FiMtxF(&portMf_.mf[0][0], view->viewingPtr);\n"
     "        Port_FiCloseChild();\n"
     "    }\n"),
])
# o ceu acompanha a camera (mesmo "corte" que ela)
patch_file("src/code/z_vr_box_draw.c", [
    ("quadros intermediarios: ceu abre",
     '    OPEN_DISPS(gfxCtx, "../z_vr_box_draw.c", 52);\n',
     '    OPEN_DISPS(gfxCtx, "../z_vr_box_draw.c", 52);\n'
     "    Port_FiOpenChild(NULL, Port_FiCameraEpoch()); /* port */\n"),
    ("quadros intermediarios: ceu fecha",
     '    CLOSE_DISPS(gfxCtx, "../z_vr_box_draw.c", 125);\n',
     "    Port_FiCloseChild(); /* port */\n"
     '    CLOSE_DISPS(gfxCtx, "../z_vr_box_draw.c", 125);\n'),
])
# efeitos (poeira, faiscas...): cada efeito e um filho; um efeito novo na mesma vaga nao e interpolado
patch_file("src/code/z_effect_soft_sprite.c", [
    ("quadros intermediarios: efeito novo",
     "            sEffectSsInfo.table[index] = *effectSs;\n",
     "            sEffectSsInfo.table[index] = *effectSs;\n"
     "            Port_FiDontInterpolateChild(&sEffectSsInfo.table[index]); /* port */\n"),
    # o caminho normal (EffectSs_Spawn: poeira, fogo, faiscas...) nao passa pelo EffectSs_Insert: sem isto o efeito
    # novo era misturado com o velho da mesma vaga e aparecia fora do lugar nos quadros intermediarios (chamas dos
    # morcegos de fogo e brilhos sobre a lava na Caverna dos Dodongos)
    ("quadros intermediarios: efeito novo (EffectSs_Spawn)",
     "    EffectSs_Delete(&sEffectSsInfo.table[index]);\n",
     "    EffectSs_Delete(&sEffectSsInfo.table[index]);\n"
     "    Port_FiDontInterpolateChild(&sEffectSsInfo.table[index]); /* port */\n"),
    ("quadros intermediarios: efeito e um filho",
     "    if (effectSs->draw != NULL) {\n        effectSs->draw(play, index, effectSs);\n    }\n",
     "    if (effectSs->draw != NULL) {\n"
     "        Port_FiOpenChild(effectSs, 0); /* port */\n"
     "        effectSs->draw(play, index, effectSs);\n"
     "        Port_FiCloseChild();\n"
     "    }\n"),
])
# quadros intermediarios: modelos "skin" (Epona etc.): o jogo calcula os vertices na CPU, num buffer que
# alterna a cada quadro. Os vertices calculados sao copiados para a gravacao do quadro; nos quadros extras o
# desenho usa uma mistura dos vertices do quadro anterior e do atual (port/frame_interp.c).
patch_file("src/code/z_skin.c", [
    ("quadros intermediarios: vertices do skin",
     "    gSPSegment(POLY_OPA_DISP++, 0x08, vtxEntry->buf[vtxEntry->index]);\n",
     "    { /* port: quadros intermediarios */\n"
     "        extern void Port_FiSkinVtx(const void* key, const void* buf, int count);\n"
     "        Port_FiSkinVtx(vtxEntry, vtxEntry->buf[vtxEntry->index], data->totalVtxCount);\n"
     "    }\n"
     "    gSPSegment(POLY_OPA_DISP++, 0x08, vtxEntry->buf[vtxEntry->index]);\n"),
])
# quadros intermediarios: texturas que rolam (agua, lava, cachoeiras...). O jogo monta um display list pequeno
# com o deslocamento da textura do quadro; a gravacao guarda o comando e os quadros extras usam o
# deslocamento intermediario (port/frame_interp.c).
FI_SCROLL = ("{ extern void Port_FiTexScroll(const void* cmd); /* port: quadros intermediarios */ "
             "Port_FiTexScroll(displayList + %d); }\n")
patch_file("src/code/z_rcp.c", [
    ("quadros intermediarios: Gfx_BranchTexScroll",
     "    Gfx* displayList = Gfx_Alloc(gfxP, 3 * sizeof(Gfx));\n\n"
     "    gDPTileSync(displayList);\n"
     "    gDPSetTileSize(displayList + 1, G_TX_RENDERTILE, x, y, x + ((width - 1) << 2), y + ((height - 1) << 2));\n"
     "    gSPEndDisplayList(displayList + 2);\n",
     "    Gfx* displayList = Gfx_Alloc(gfxP, 3 * sizeof(Gfx));\n\n"
     "    gDPTileSync(displayList);\n"
     "    gDPSetTileSize(displayList + 1, G_TX_RENDERTILE, x, y, x + ((width - 1) << 2), y + ((height - 1) << 2));\n"
     "    gSPEndDisplayList(displayList + 2);\n    " + FI_SCROLL % 1),
    ("quadros intermediarios: Gfx_TexScroll",
     "    y %= 512 << 2;\n\n"
     "    gDPTileSync(displayList);\n"
     "    gDPSetTileSize(displayList + 1, G_TX_RENDERTILE, x, y, x + ((width - 1) << 2), y + ((height - 1) << 2));\n"
     "    gSPEndDisplayList(displayList + 2);\n",
     "    y %= 512 << 2;\n\n"
     "    gDPTileSync(displayList);\n"
     "    gDPSetTileSize(displayList + 1, G_TX_RENDERTILE, x, y, x + ((width - 1) << 2), y + ((height - 1) << 2));\n"
     "    gSPEndDisplayList(displayList + 2);\n    " + FI_SCROLL % 1),
    ("quadros intermediarios: Gfx_TwoTexScroll",
     "    gSPEndDisplayList(displayList + 4);\n",
     "    gSPEndDisplayList(displayList + 4);\n    " + FI_SCROLL % 1 + "    " + FI_SCROLL % 3),
    ("quadros intermediarios: Gfx_TwoTexScrollEnvColor",
     "    gSPEndDisplayList(displayList + 5);\n",
     "    gSPEndDisplayList(displayList + 5);\n    " + FI_SCROLL % 1 + "    " + FI_SCROLL % 3),
])
# Pular cenas com START (o atalho das versoes de depuracao, ligado no port). O A nao serve: ele avanca
# os textos dentro das cenas.
# Textos: A ou B mostram a fala inteira na hora, inclusive as que o jogo marca como "nao pulaveis"
# (dentro das cenas). Depois o A avanca normalmente.
# (opcao segurar_b, port/port_options.c: com ela, SEGURAR o B tambem passa as falas, como no SoH)
# Um toque no B (que dura 2-3 quadros) nao pode completar a fala E passar para a proxima: segurar so
# acelera depois de meio segundo (10 quadros) com o B apertado.
PORT_B = "(CHECK_BTN_ALL(input->press.button, BTN_B) || Port_SegurarB(play))"
PORT_SEGURAR_B = """/* port: opcao segurar_b. Conta os quadros com o B apertado; so depois de 10 (meio segundo) segurar passa
 * as falas. Um toque (2-3 quadros) completa a fala; outro toque passa para a proxima. */
static s32 Port_SegurarB(PlayState* play) {
    extern int port_opt_segurar_b;
    static u32 sLastFrame;
    static s32 sHeld;
    Input* in = &play->state.input[0];

    if (play->state.frames != sLastFrame) {
        /* (se a funcao nao foi chamada no quadro anterior, a contagem recomeca) */
        sHeld = (CHECK_BTN_ALL(in->cur.button, BTN_B) && play->state.frames == sLastFrame + 1) ? sHeld + 1
                : CHECK_BTN_ALL(in->cur.button, BTN_B) ? 1 : 0;
        sLastFrame = play->state.frames;
    }
    return port_opt_segurar_b && (sHeld >= 10);
}

"""
OCA_LENTA = ("!({ extern int port_opt_ocarina_rapida; port_opt_ocarina_rapida; }) || "
             "msgCtx->lastPlayedSong == OCARINA_SONG_TIME || msgCtx->lastPlayedSong == OCARINA_SONG_STORMS || "
             "msgCtx->lastPlayedSong == OCARINA_SONG_SUNS")
patch_file("src/code/z_message.c", [
    # caixa de texto nitida (port/gfx/gfx_hires.c, opcao texto_nitido): marca no display list o comeco (com a
    # regiao da caixa, em pixels de 320x240) e o fim do desenho das caixas. Com a resolucao dinamica abaixo de
    # 640, a regiao e desenhada em 640x480 por cima da imagem 3D.
    # opcao caixas_opacas: caixa de dialogo opaca (alfa 255 em vez de 230/180/170). Alfa constante 255 deixa o
    # blender do desenho rapido simples (sem misturar com o fundo) e a resolucao nao cai por causa da caixa.
    ("caixas opacas: madeira", "            msgCtx->textboxColorAlphaTarget = 230;\n",
     "            msgCtx->textboxColorAlphaTarget = ({ extern int port_opt_caixas_opacas; port_opt_caixas_opacas; }) ? 255 : 230; /* port */\n"),
    ("caixas opacas: ocarina", "            msgCtx->textboxColorAlphaTarget = 180;\n",
     "            msgCtx->textboxColorAlphaTarget = ({ extern int port_opt_caixas_opacas; port_opt_caixas_opacas; }) ? 255 : 180; /* port */\n"),
    ("caixas opacas: normal", "            msgCtx->textboxColorAlphaTarget = 170;\n",
     "            msgCtx->textboxColorAlphaTarget = ({ extern int port_opt_caixas_opacas; port_opt_caixas_opacas; }) ? 255 : 170; /* port */\n"),
    ("caixa de texto nitida: comeco",
     "    plusOne = Gfx_Open(polyOpaP = POLY_OPA_DISP);\n    gSPDisplayList(OVERLAY_DISP++, plusOne);\n"
     "    Message_DrawMain(play, &plusOne);\n",
     "    if (play->msgCtx.msgMode != MSGMODE_NONE) { /* port: regiao da caixa (marcas para gfx_hires.c) */\n"
     "        s32 portX0 = R_TEXTBOX_X - 4, portY0 = R_TEXTBOX_Y - 4;\n"
     "        s32 portX1 = R_TEXTBOX_X + R_TEXTBOX_WIDTH + 4, portY1 = R_TEXTBOX_Y + R_TEXTBOX_HEIGHT + 4;\n\n"
     "        if (portX0 < 0) portX0 = 0;\n"
     "        if (portY0 < 0) portY0 = 0;\n"
     "        if (portX1 > 320) portX1 = 320;\n"
     "        if (portY1 > 240) portY1 = 240;\n"
     "        if (portX1 > portX0 && portY1 > portY0) {\n"
     "            OVERLAY_DISP->words.w0 = (u32)((portX0 << 12) | portY0);\n"
     "            OVERLAY_DISP->words.w1 = 0x54585430;\n"
     "            OVERLAY_DISP++;\n"
     "            OVERLAY_DISP->words.w0 = (u32)((portX1 << 12) | portY1);\n"
     "            OVERLAY_DISP->words.w1 = 0x54585431;\n"
     "            OVERLAY_DISP++;\n"
     "        }\n"
     "    }\n"
     "    plusOne = Gfx_Open(polyOpaP = POLY_OPA_DISP);\n    gSPDisplayList(OVERLAY_DISP++, plusOne);\n"
     "    Message_DrawMain(play, &plusOne);\n"),
    ("caixa de texto nitida: fim",
     "    POLY_OPA_DISP = plusOne;\n    CLOSE_DISPS(play->state.gfxCtx, \"../z_message_PAL.c\", 3582);\n",
     "    POLY_OPA_DISP = plusOne;\n"
     "    OVERLAY_DISP->words.w0 = 0; /* port: fim da caixa de texto */\n"
     "    OVERLAY_DISP->words.w1 = 0x54585432;\n"
     "    OVERLAY_DISP++;\n"
     "    CLOSE_DISPS(play->state.gfxCtx, \"../z_message_PAL.c\", 3582);\n"),
    # Pacote de idioma (port/lang.c, opcao idioma): tabela de mensagens e texto vem do pacote.
    ("idioma: tabela de mensagens",
     "    sStaffMessageEntryTablePtr = sStaffMessageEntryTable;\n\n#if PLATFORM_N64 && OOT_NTSC\n",
     "    sStaffMessageEntryTablePtr = sStaffMessageEntryTable;\n"
     "    { /* port: idioma */\n"
     "        extern void* Port_LangNesTable(void);\n"
     "        if (Port_LangNesTable() != NULL) {\n"
     "            sNesMessageEntryTablePtr = (MessageTableEntry*)Port_LangNesTable();\n"
     "        }\n"
     "    }\n\n#if PLATFORM_N64 && OOT_NTSC\n"),
    ("idioma: prototipo",
     "void Message_FindMessageNES(PlayState* play, u16 textId) {\n",
     "int Port_LangMsg(void* dst, u32 off, u32 len); /* port: idioma (port/lang.c) */\n\n"
     "void Message_FindMessageNES(PlayState* play, u16 textId) {\n"),
    ("idioma: texto da mensagem",
     "            if ((B_80121220 != NULL) && (B_80121220->unk_68 != NULL) && B_80121220->unk_68(&play->msgCtx.font)) {\n",
     "            if (Port_LangMsg(MSG_BUF, font->msgOffset, font->msgLength)) {\n\n"
     "            } else if ((B_80121220 != NULL) && (B_80121220->unk_68 != NULL) && B_80121220->unk_68(&play->msgCtx.font)) {\n"),
    # Um toque em A ou B so completa a fala da caixa atual (como nos Zelda novos). No original, o B ligava
    # sTextboxSkipped e o jogo passava direto por todas as caixas da mensagem (o "pular a conversa toda").
    # Segurar o B (opcao segurar_b) continua passando tudo rapido, menos nas falas que o jogo marca como
    # nao pulaveis (dentro das cenas).
    ("completar o texto com A ou B",
     "CHECK_BTN_ALL(input->press.button, BTN_B) && !msgCtx->textUnskippable) {\n"
     "                    sTextboxSkipped = true;\n",
     "(" + PORT_B + " || CHECK_BTN_ALL(input->press.button, BTN_A))) { /* port */\n"
     "                    if (Port_SegurarB(play) && !msgCtx->textUnskippable) {\n"
     "                        sTextboxSkipped = true;\n"
     "                    }\n"),
    ("segurar B passa as falas",
     "u8 Message_ShouldAdvance(PlayState* play) {\n"
     "    Input* input = &play->state.input[0];\n\n"
     "    if (CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_B) ||\n"
     "        CHECK_BTN_ALL(input->press.button, BTN_CUP)) {\n"
     "        SFX_PLAY_CENTERED(NA_SE_SY_MESSAGE_PASS);\n"
     "    }\n"
     "    return CHECK_BTN_ALL(input->press.button, BTN_A) || CHECK_BTN_ALL(input->press.button, BTN_B) ||\n"
     "           CHECK_BTN_ALL(input->press.button, BTN_CUP);\n",
     PORT_SEGURAR_B +
     "u8 Message_ShouldAdvance(PlayState* play) {\n"
     "    Input* input = &play->state.input[0];\n\n"
     "    if (CHECK_BTN_ALL(input->press.button, BTN_A) || " + PORT_B + " ||\n"
     "        CHECK_BTN_ALL(input->press.button, BTN_CUP)) {\n"
     "        SFX_PLAY_CENTERED(NA_SE_SY_MESSAGE_PASS);\n"
     "    }\n"
     "    return CHECK_BTN_ALL(input->press.button, BTN_A) || " + PORT_B + " ||\n"
     "           CHECK_BTN_ALL(input->press.button, BTN_CUP); /* port */\n"),
    # opcao ocarina_rapida (SoH FastOcarinaPlayback): depois de tocar uma musica, o jogo nao repete a
    # melodia inteira. Sol, Tempestades e Tempo continuam como no original (o efeito depende da melodia).
    ("ocarina rapida: sem repetir a melodia",
     "            case MSGMODE_SETUP_DISPLAY_SONG_PLAYED:\n"
     "                DRAW_TEXT(play, &gfx, false);\n"
     "                AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                AudioOcarina_SetPlaybackSong(msgCtx->lastPlayedSong + 1, 1);\n",
     "            case MSGMODE_SETUP_DISPLAY_SONG_PLAYED:\n"
     "                if (" + OCA_LENTA + ") { /* port */\n"
     "                    DRAW_TEXT(play, &gfx, false);\n"
     "                    AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                    AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                    AudioOcarina_SetPlaybackSong(msgCtx->lastPlayedSong + 1, 1);\n"
     "                } else {\n"
     "                    AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                    AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);\n"
     "                }\n"),
    ("ocarina rapida: texto do nome da musica",
     "                msgCtx->msgMode = MSGMODE_DISPLAY_SONG_PLAYED_TEXT;\n"
     "                msgCtx->stateTimer = 20;\n",
     "                msgCtx->msgMode = MSGMODE_DISPLAY_SONG_PLAYED_TEXT;\n"
     "                msgCtx->stateTimer = (" + OCA_LENTA + ") ? 20 : 1; /* port */\n"),
    # quadros intermediarios: a caixa de dialogo abre/fecha (tamanho e transparencia) a 30 fps
    ("quadros intermediarios: cor da caixa de dialogo",
     "    gDPSetPrimColor(gfx++, 0, 0, msgCtx->textboxColorRed, msgCtx->textboxColorGreen, msgCtx->textboxColorBlue,\n"
     "                    msgCtx->textboxColorAlphaCurrent);\n",
     "    gDPSetPrimColor(gfx++, 0, 0, msgCtx->textboxColorRed, msgCtx->textboxColorGreen, msgCtx->textboxColorBlue,\n"
     "                    msgCtx->textboxColorAlphaCurrent);\n"
     "    { extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(gfx - 1, 0); } /* port */\n"),
    ("quadros intermediarios: retangulo da caixa de dialogo",
     "                        (R_TEXTBOX_Y + R_TEXTBOX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0, R_TEXTBOX_TEXWIDTH << 1,\n"
     "                        R_TEXTBOX_TEXHEIGHT << 1);\n",
     "                        (R_TEXTBOX_Y + R_TEXTBOX_HEIGHT) << 2, G_TX_RENDERTILE, 0, 0, R_TEXTBOX_TEXWIDTH << 1,\n"
     "                        R_TEXTBOX_TEXHEIGHT << 1);\n"
     "    { extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(gfx - 3, 1); } /* port */\n"),
])
# opcao barra_inimigo: barra de vida em cima do inimigo mirado com Z
PORT_HEALTHBAR = """
/* port: barra de vida do inimigo mirado com Z (ideia do Ship of Harkinian; opcao barra_inimigo).
 * A vida maxima de cada ator e anotada logo depois do init dele (z_actor.c). */
#define PORT_HP_SLOTS 256
static struct {
    Actor* actor;
    u8 hp;
} sPortMaxHp[PORT_HP_SLOTS];

static s32 Port_HpSlot(Actor* actor) {
    return (s32)((((unsigned long)actor) >> 4) % PORT_HP_SLOTS);
}

void Port_ActorInitDone(Actor* actor) {
    s32 i = Port_HpSlot(actor);
    sPortMaxHp[i].actor = actor;
    sPortMaxHp[i].hp = actor->colChkInfo.health;
}

static void Port_DrawEnemyHealthBar(PlayState* play) {
    extern int port_opt_barra_inimigo;
    Attention* attention = &play->actorCtx.attention;
    Actor* actor = attention->reticleActor;
    Player* player = GET_PLAYER(play);
    Vec3f proj;
    f32 invW;
    s32 x, y, fill, maxHp, slot;

    if (!port_opt_barra_inimigo || (attention->reticleFadeAlphaControl == 0) || (actor == NULL) ||
        (actor->category != ACTORCAT_ENEMY) || (attention->reticleRadius >= 500.0f)) {
        return;
    }
    if ((player->stateFlags1 & PLAYER_STATE1_TALKING) && (actor == player->focusActor)) {
        return;
    }
    slot = Port_HpSlot(actor);
    maxHp = (sPortMaxHp[slot].actor == actor) ? sPortMaxHp[slot].hp : 0;
    if (maxHp < actor->colChkInfo.health) {
        maxHp = actor->colChkInfo.health;
    }
    if (maxHp <= 0) {
        return;
    }
    fill = (actor->colChkInfo.health * 64) / maxHp;

    Actor_ProjectPos(play, &attention->reticlePos, &proj, &invW);
    x = (s32)((SCREEN_WIDTH / 2) + (SCREEN_WIDTH / 2) * (proj.x * invW)) - 40;
    y = (s32)((SCREEN_HEIGHT / 2) - (SCREEN_HEIGHT / 2) * (proj.y * invW)) - 8 - 40;
    x = CLAMP(x, 0, SCREEN_WIDTH - 80);
    y = CLAMP(y, 0, SCREEN_HEIGHT - 16);

    OPEN_DISPS(play->state.gfxCtx, "../z_parameter.c", 0);

    Gfx_SetupDL_39Overlay(play->state.gfxCtx);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 255, 255, 255);
    gDPSetEnvColor(OVERLAY_DISP++, 100, 50, 50, 255);
    OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gMagicMeterEndTex, 8, 16, x, y, 8, 16, 1 << 10, 1 << 10);
    OVERLAY_DISP = Gfx_TextureIA8(OVERLAY_DISP, gMagicMeterMidTex, 24, 16, x + 8, y, 64, 16, 1 << 10, 1 << 10);
    gDPLoadTextureBlock(OVERLAY_DISP++, gMagicMeterEndTex, G_IM_FMT_IA, G_IM_SIZ_8b, 8, 16, 0,
                        G_TX_MIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 3, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPTextureRectangle(OVERLAY_DISP++, (x + 72) << 2, y << 2, (x + 80) << 2, (y + 16) << 2, G_TX_RENDERTILE, 256,
                        0, 1 << 10, 1 << 10);

    if (fill > 0) {
        gDPPipeSync(OVERLAY_DISP++);
        gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0, 0, 0, PRIMITIVE, PRIMITIVE,
                          ENVIRONMENT, TEXEL0, ENVIRONMENT, 0, 0, 0, PRIMITIVE);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
        gDPSetPrimColor(OVERLAY_DISP++, 0, 0, 255, 40, 40, 255);
        gDPLoadMultiBlock_4b(OVERLAY_DISP++, gMagicMeterFillTex, 0x0000, G_TX_RENDERTILE, G_IM_FMT_I, 16, 16, 0,
                             G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK,
                             G_TX_NOLOD, G_TX_NOLOD);
        gSPTextureRectangle(OVERLAY_DISP++, (x + 8) << 2, (y + 3) << 2, (x + 8 + fill) << 2, (y + 10) << 2,
                            G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
    }

    CLOSE_DISPS(play->state.gfxCtx, "../z_parameter.c", 0);
}

"""
patch_file("src/code/z_parameter.c", [
    # opcao caixas_opacas: minimapa opaco (o jogo o deixa em alfa 170 nos campos abertos; la o tipo e 255 nos demais)
    ("minimapa opaco",
     "                    if (interfaceCtx->minimapAlpha < 170) {\n"
     "                        interfaceCtx->minimapAlpha = risingAlpha;\n"
     "                    } else {\n"
     "                        interfaceCtx->minimapAlpha = 170;\n"
     "                    }\n",
     "                    if (({ extern int port_opt_caixas_opacas; port_opt_caixas_opacas; })) { /* port */\n"
     "                        if (interfaceCtx->minimapAlpha != 255) {\n"
     "                            interfaceCtx->minimapAlpha = risingAlpha;\n"
     "                        }\n"
     "                    } else if (interfaceCtx->minimapAlpha < 170) {\n"
     "                        interfaceCtx->minimapAlpha = risingAlpha;\n"
     "                    } else {\n"
     "                        interfaceCtx->minimapAlpha = 170;\n"
     "                    }\n", 2),
    ("barra de vida do inimigo: funcao", "void Interface_SetSubTimer(s16 seconds) {\n",
     PORT_HEALTHBAR + "void Interface_SetSubTimer(s16 seconds) {\n"),
    ("barra de vida do inimigo: desenho",
     "            Attention_Draw(&play->actorCtx.attention, play);\n",
     "            Attention_Draw(&play->actorCtx.attention, play);\n"
     "            Port_DrawEnemyHealthBar(play); /* port */\n"),
    ("HUD pelo acelerador 2D: marca o comeco do 2D",
     "    gSPSegment(OVERLAY_DISP++, 0x02, interfaceCtx->parameterSegment);\n",
     "    OVERLAY_DISP->words.w0 = 0; /* port: daqui ao fim do quadro, 2D que pode ir para o acelerador (gfx_ge.c) */\n"
     "    OVERLAY_DISP->words.w1 = 0x54585433;\n"
     "    OVERLAY_DISP++;\n"
     "    gSPSegment(OVERLAY_DISP++, 0x02, interfaceCtx->parameterSegment);\n"),
])
# logo da abertura pelo acelerador 2D: a mesma marca (gfx_ge.c)
patch_file("src/overlays/actors/ovl_En_Mag/z_en_mag.c", [
    ("logo pelo acelerador 2D: marca",
     "    gfxRef = POLY_OPA_DISP;\n    gfx = Gfx_Open(gfxRef);\n    gSPDisplayList(OVERLAY_DISP++, gfx);\n",
     "    gfxRef = POLY_OPA_DISP;\n    gfx = Gfx_Open(gfxRef);\n"
     "    OVERLAY_DISP->words.w0 = 0; /* port: daqui ao fim do quadro, 2D que pode ir para o acelerador (gfx_ge.c) */\n"
     "    OVERLAY_DISP->words.w1 = 0x54585433;\n"
     "    OVERLAY_DISP++;\n"
     "    gSPDisplayList(OVERLAY_DISP++, gfx);\n"),
    # PRESS START da abertura com as letras novas (gfx_hd.c): marcas 0x5458543E/F em volta de cada letra do titulo
    ("letras do titulo com textura nova",
     "    YREG(0) = 1024.0f / (YREG(1) / 100.0f);\n    YREG(2) = 16.0f * (YREG(1) / 100.0f);\n\n    gDPLoadTextureBlock_4b(",
     "    YREG(0) = 1024.0f / (YREG(1) / 100.0f);\n    YREG(2) = 16.0f * (YREG(1) / 100.0f);\n\n"
     "    gfx->words.w0 = 0; /* port */\n    gfx->words.w1 = 0x5458543E;\n    gfx++;\n"
     "    gDPLoadTextureBlock_4b("),
    ("letras do titulo: fim",
     "                        G_TX_RENDERTILE, 0, 0, YREG(0), YREG(0));\n\n    *gfxP = gfx;\n}\n\n#if PLATFORM_IQUE",
     "                        G_TX_RENDERTILE, 0, 0, YREG(0), YREG(0));\n\n"
     "    gfx->words.w0 = 0; /* port */\n    gfx->words.w1 = 0x5458543F;\n    gfx++;\n"
     "    *gfxP = gfx;\n}\n\n#if PLATFORM_IQUE"),
    # opcao logo_opaca: o jogo desenha o escudo com alfa geral de no maximo 210 (82%: a cena aparecia atras dele).
    # Aqui o alfa vai ate 255, mantendo o aparecer gradual proporcional (210 -> 255).
    ("logo do escudo: alfa geral ate 255",
     "    gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, (s16)this->mainAlpha);\n",
     "    gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255,\n"
     "                    ({ extern int gx_logo_opaca; gx_logo_opaca; }) /* port */\n"
     "                        ? (s16)CLAMP_MAX((s32)this->mainAlpha * 255 / 210, 255)\n"
     "                        : (s16)this->mainAlpha);\n"),
    # opcao logo_opaca: textos "THE LEGEND OF" / "OCARINA OF TIME" (sombra e letra) tambem ate alfa 255
    ("logo: sombra do texto opaca",
     "    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, (s16)this->mainAlpha);\n",
     "    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, ({ extern int gx_logo_opaca; gx_logo_opaca; }) ? (s16)CLAMP_MAX((s32)this->mainAlpha * 255 / 210, 255) : (s16)this->mainAlpha); /* port */\n"),
    ("logo: texto opaco",
     "        gDPSetPrimColor(gfx++, 0, 0, 200, 200, 150, (s16)this->mainAlpha);\n",
     "        gDPSetPrimColor(gfx++, 0, 0, 200, 200, 150, ({ extern int gx_logo_opaca; gx_logo_opaca; }) ? (s16)CLAMP_MAX((s32)this->mainAlpha * 255 / 210, 255) : (s16)this->mainAlpha); /* port */\n"),
    # opcao logo_opaca: marcas 0x5458543C/D so em volta da logo do escudo (as chamas ficam como estao)
    ("logo do escudo sem transparencia",
     "        EnMag_DrawImageRGBA32(&gfx, 160 + LOGO_X_SHIFT, 100, (u8*)gTitleZeldaShieldLogoTex, 160, 160);\n",
     "        gfx->words.w0 = 0; /* port: logo do escudo sem transparencia (gfx_rdp.c, opcao logo_opaca) */\n"
     "        gfx->words.w1 = 0x5458543C;\n"
     "        gfx++;\n"
     "        EnMag_DrawImageRGBA32(&gfx, 160 + LOGO_X_SHIFT, 100, (u8*)gTitleZeldaShieldLogoTex, 160, 160);\n"
     "        gfx->words.w0 = 0;\n"
     "        gfx->words.w1 = 0x5458543D;\n"
     "        gfx++;\n"),
])
# halos de luz (Navi, tochas): (1) cada halo tem o seu proprio "filho" na arvore de matrizes da interpolacao, assim o
# halo que liga/desliga no meio nao troca de matriz com outra luz (aparecia fora do lugar); (2) folga no teste do
# z-buffer que decide se o halo aparece.
patch_file("src/code/z_lights.c", [
    ("halo: filho proprio na interpolacao",
     "                MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, play->state.gfxCtx, \"../z_lights.c\", 918);\n"
     "                gSPDisplayList(POLY_XLU_DISP++, gGlowCircleDL);\n",
     "                MATRIX_FINALIZE_AND_LOAD(POLY_XLU_DISP++, play->state.gfxCtx, \"../z_lights.c\", 918);\n"
     "                gSPDisplayList(POLY_XLU_DISP++, gGlowCircleDL);\n"
     "                Port_FiCloseChild(); /* port */\n"),
    ("halo: abre o filho",
     "                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, params->color[0], params->color[1], params->color[2], 50);\n",
     "                Port_FiOpenChild(node->info, 0); /* port: este halo, onde quer que esteja na lista */\n"
     "                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, params->color[0], params->color[1], params->color[2], 50);\n"),
    # o z-buffer na RAM fica na ordem do N64 (big-endian, como o renderizador grava); o codigo do jogo, compilado
    # para little-endian, lia o numero com os bytes trocados: o teste do halo dava resultado quase aleatorio (o halo
    # da Navi e das tochas sumia e voltava). Aqui o valor e lido como o N64 le.
    ("halo: z com a ordem dos bytes do N64",
     "                zBuf = gZBuffer[(s32)((wY * -(SCREEN_HEIGHT / 2)) + (SCREEN_HEIGHT / 2))]\n"
     "                               [(s32)((wX * (SCREEN_WIDTH / 2)) + (SCREEN_WIDTH / 2))]\n"
     "                       << 2;\n",
     "                {\n"
     "                    extern unsigned short gx_hr_zread(const void* zram, int x, int y); /* port: gfx_hires.c */\n"
     "                    zBuf = gx_hr_zread(gZBuffer, (s32)((wX * (SCREEN_WIDTH / 2)) + (SCREEN_WIDTH / 2)),\n"
     "                                       (s32)((wY * -(SCREEN_HEIGHT / 2)) + (SCREEN_HEIGHT / 2)));\n"
     "                }\n"
     "                zBuf = (u16)((zBuf >> 8) | (zBuf << 8)) << 2; /* port: z na ordem do N64 (big-endian) */\n"),
    ("halo: folga no teste do z",
     "                if (wZ < (Environment_ZBufValToFixedPoint(zBuf) >> 3)) {\n                    params->drawGlow = true;\n                }\n",
     "                /* port: folga (opcao halo_folga): o centro do halo das tochas cai na borda do suporte/chama, uns\n"
     "                 * 300-500 pontos a frente da parede; sem folga ele sumia e voltava com a camera andando */\n"
     "                if (wZ < (Environment_ZBufValToFixedPoint(zBuf) >> 3) + ({ extern int port_opt_halo_folga; port_opt_halo_folga; })) {\n"
     "                    params->drawGlow = true;\n                }\n"),
])
patch_file("src/code/z_kankyo.c", [
    # sol: o gSunDL original carrega as texturas do sol (i4 64x64, em 3 faixas) como se fossem i8; cada faixa mostrava
    # um pedaco errado (2 sois espremidos lado a lado, meio sol e lixo). No N64 e igual; o SoH troca por um quadrado
    # so com a textura i4 inteira. Fazemos o mesmo (custo igual: 1 quadrado em vez de 3). A do sol da tarde
    # (gSunEvening1Tex) vem logo depois, em gSun1Tex + 0x800 (o simbolo dela nao existe na ligacao do port).
    ("sol: cabecalhos das texturas",
     '#include "assets/objects/gameplay_keep/eff_lightning.h"\n',
     '#include "assets/objects/gameplay_keep/eff_lightning.h"\n'
     '#include "assets/objects/gameplay_keep/sun_textures.h" /* port */\n'
     ),
    ("sol: textura certa (como o SoH)",
     "        gSPDisplayList(POLY_OPA_DISP++, gSunDL);\n",
     "        { /* port: substitui o gSunDL */\n"
     "            static Vtx sPortSunVtx[] = {\n"
     "                VTX(-31, -31, 0, 0, 0, 255, 255, 255, 255),\n"
     "                VTX(32, -31, 0, 2016, 0, 255, 255, 255, 255),\n"
     "                VTX(-31, 32, 0, 0, 2016, 255, 255, 255, 255),\n"
     "                VTX(32, 32, 0, 2016, 2016, 255, 255, 255, 255),\n"
     "            };\n"
     "            gSPMatrix(POLY_OPA_DISP++, SEGMENT_ADDR(1, 0), G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW);\n"
     "            gDPPipeSync(POLY_OPA_DISP++);\n"
     "            gDPLoadTextureBlock_4b(POLY_OPA_DISP++, gSun1Tex, G_IM_FMT_I, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP,\n"
     "                                   G_TX_NOMIRROR | G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);\n"
     "            gDPLoadMultiBlock_4b(POLY_OPA_DISP++, (u8*)gSun1Tex + 0x800, 0x0100, 1, G_IM_FMT_I, 64, 64, 0,\n"
     "                                 G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);\n"
     "            gSPVertex(POLY_OPA_DISP++, sPortSunVtx, 4, 0);\n"
     "            gSP2Triangles(POLY_OPA_DISP++, 0, 1, 2, 0, 2, 1, 3, 0);\n"
     "        }\n"),
    # lens flare do sol: o jogo so desenha se o pixel do sol tiver o z do ceu (0xFFFC); lido com os bytes trocados
    # dava 0xFCFF e o flare nunca aparecia (tunel da abertura)
    ("sol: z com a ordem dos bytes do N64",
     "    s32 pixelDepth = gZBuffer[y][x];\n",
     "    extern unsigned short gx_hr_zread(const void* zram, int x, int y); /* port: gfx_hires.c */\n"
     "    u16 zv = gx_hr_zread(gZBuffer, x, y);\n"
     "    s32 pixelDepth = (u16)((zv >> 8) | (zv << 8)); /* port: z na ordem do N64 (big-endian) */\n"),
    # reflexo da lente (lens flare): de dia, com a camera virada para o lado do sol, o jogo desenha 10 circulos
    # transparentes por cima da tela (e um clarao na tela inteira) mesmo quando estao invisiveis (o sol escondido
    # atras de um morro: transparencia 0). Pula os invisiveis (a imagem e a mesma) e, com a opcao lens_flare = 0,
    # todos. As contas do jogo (o reflexo acendendo/apagando aos poucos) continuam rodando.
    ("lens flare: pula os circulos invisiveis (e todos com lens_flare = 0)",
     "            POLY_XLU_DISP = func_800947AC(POLY_XLU_DISP++);\n"
     "            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, lensFlareColors[i].r, lensFlareColors[i].g, lensFlareColors[i].b,\n",
     "            if (({ extern int port_opt_lens_flare; port_opt_lens_flare; }) &&\n"
     "                (u8)(alpha * envCtx->lensFlareAlphaScale) != 0) { /* port */\n"
     "            POLY_XLU_DISP = func_800947AC(POLY_XLU_DISP++);\n"
     "            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, lensFlareColors[i].r, lensFlareColors[i].g, lensFlareColors[i].b,\n"),
    ("lens flare: fim do bloco dos circulos",
     "                    gSPDisplayList(POLY_XLU_DISP++, gLensFlareRingDL);\n"
     "                    break;\n"
     "            }\n",
     "                    gSPDisplayList(POLY_XLU_DISP++, gLensFlareRingDL);\n"
     "                    break;\n"
     "            }\n"
     "            } /* port */\n"),
    ("lens flare: clarao na tela inteira so quando aparece",
     "                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, (u8)(temp * 75.0f) + 180, (u8)(temp * 155.0f) + 100,\n"
     "                                (u8)envCtx->glareAlpha);\n"
     "                gDPFillRectangle(POLY_XLU_DISP++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);\n",
     "                if (({ extern int port_opt_lens_flare; port_opt_lens_flare; }) && (u8)envCtx->glareAlpha != 0) { /* port */\n"
     "                gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, (u8)(temp * 75.0f) + 180, (u8)(temp * 155.0f) + 100,\n"
     "                                (u8)envCtx->glareAlpha);\n"
     "                gDPFillRectangle(POLY_XLU_DISP++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);\n"
     "                }\n"),
])
# opcao alarme_vida: 0 tira o bipe de vida baixa
patch_file("src/code/z_lifemeter.c", [
    ("bipe de vida baixa opcional",
     "                Sfx_PlaySfxCentered(NA_SE_SY_HITPOINT_ALARM);\n",
     "                if (({ extern int port_opt_alarme_vida; port_opt_alarme_vida; })) { /* port */\n"
     "                    Sfx_PlaySfxCentered(NA_SE_SY_HITPOINT_ALARM);\n"
     "                }\n"),
])
# opcao sem_tremor: 1 tira os trancos de camera com vida baixa
patch_file("src/code/z_camera.c", [
    ("tremor de vida baixa opcional",
     "        if (gSaveContext.save.info.playerData.health <= 16 && ((camera->play->state.frames % 256) == 0)) {\n",
     "        if (!({ extern int port_opt_sem_tremor; port_opt_sem_tremor; }) && /* port */\n"
     "            gSaveContext.save.info.playerData.health <= 16 && ((camera->play->state.frames % 256) == 0)) {\n"),
])
patch_file("src/code/z_demo.c", [
    ("pular cena com START",
     "        (DEBUG_FEATURES && (csCtx->curFrame > 20) && CHECK_BTN_ALL(play->state.input[0].press.button, BTN_START) &&\n",
     "        (({ extern int port_opt_pular_cenas; port_opt_pular_cenas; }) && /* port */\n"
     "         (gSaveContext.gameMode == GAMEMODE_NORMAL) && /* port: na abertura o START e do jogo (titulo) */\n"
     "         (csCtx->curFrame > 20) && CHECK_BTN_ALL(play->state.input[0].press.button, BTN_START) &&\n"
     "         (csCtx->script != gPortCsOcarinaScript) && /* port: cena com ocarina (aprender/tocar musica) nao pula */\n"),
    # Cena que tem ocarina (CS_TEXT_OCARINA_ACTION: Link aprende ou toca uma musica, ex. o poema do Tumulo Real e a
    # Cancao do Sol): pular com START ia direto para o fim e a musica ficava sem aprender. O jogo le todos os
    # comandos da cena a cada quadro; o de texto com ocarina marca a cena, e o START nao pula essa cena.
    ("pular cena: marca de cena com ocarina",
     "void CutsceneCmd_Destination(PlayState* play, CutsceneContext* csCtx, CsCmdDestination* cmd) {\n",
     "void* gPortCsOcarinaScript; /* port: cena que tem ocarina (nao pula com START) */\n\n"
     "void CutsceneCmd_Destination(PlayState* play, CutsceneContext* csCtx, CsCmdDestination* cmd) {\n"),
    ("pular cena: acha a ocarina na cena",
     "                    if (((CsCmdText*)cmd)->textId != CS_TEXT_ID_NONE) {\n"
     "                        CutsceneCmd_Text(play, csCtx, (void*)script);\n",
     "                    if (((CsCmdText*)cmd)->type == CS_TEXT_OCARINA_ACTION) {\n"
     "                        gPortCsOcarinaScript = csCtx->script; /* port */\n"
     "                    }\n"
     "                    if (((CsCmdText*)cmd)->textId != CS_TEXT_ID_NONE) {\n"
     "                        CutsceneCmd_Text(play, csCtx, (void*)script);\n"),
])
# Menu de pausa sem espada: o jogo le sPauseModelGroupBySword[-1] (bug conhecido do original; no N64 o
# byte antes da tabela e 0 por sorte). Aqui a memoria e outra e o lixo virava um ponteiro invalido:
# o jogo travava ao apertar START antes de pegar a espada. Usa o valor do N64 (0).
patch_file("src/code/z_player_lib.c", [
    ("pausa sem espada",
     "    u8 modelGroup = sPauseModelGroupBySword[playerSwordAndShield[0] - PLAYER_SWORD_KOKIRI];\n",
     "    u8 modelGroup = (playerSwordAndShield[0] >= PLAYER_SWORD_KOKIRI) /* port */\n"
     "                        ? sPauseModelGroupBySword[playerSwordAndShield[0] - PLAYER_SWORD_KOKIRI]\n"
     "                        : PLAYER_MODELGROUP_0;\n"),
    # Link da tela de equipamentos (port/gfx/gfx_hires.c): o jogo o desenha numa imagem de 64x112 que vira
    # textura do menu (ampliada, fica quadriculada). Marcas em volta do desenho: o port desenha em 2x e usa
    # essa imagem no lugar da textura.
    ("Link do menu em 2x: comeco",
     "    gSPSegment(POLY_OPA_DISP++, 0x00, NULL);\n\n    gDPPipeSync(POLY_OPA_DISP++);\n\n"
     "    gSPLoadGeometryMode(POLY_OPA_DISP++, 0);\n",
     "    gSPSegment(POLY_OPA_DISP++, 0x00, NULL);\n"
     "    POLY_OPA_DISP->words.w0 = 0; /* port: comeco do Link do menu (gfx_hires.c) */\n"
     "    POLY_OPA_DISP->words.w1 = 0x54585437;\n"
     "    POLY_OPA_DISP++;\n\n    gDPPipeSync(POLY_OPA_DISP++);\n\n"
     "    gSPLoadGeometryMode(POLY_OPA_DISP++, 0);\n"),
    ("Link do menu em 2x: fim",
     "    gSPEndDisplayList(POLY_OPA_DISP++);\n    gSPEndDisplayList(POLY_XLU_DISP++);\n",
     "    gSPEndDisplayList(POLY_OPA_DISP++);\n"
     "    POLY_XLU_DISP->words.w0 = 0; /* port: fim do Link do menu (a lista translucida vem depois da opaca) */\n"
     "    POLY_XLU_DISP->words.w1 = 0x54585438;\n"
     "    POLY_XLU_DISP++;\n"
     "    gSPEndDisplayList(POLY_XLU_DISP++);\n"),
])
# opcao atalho_roupa: SELECT+direita/esquerda troca botas/tunica (input_miyoo.c manda como D-pad do N64)
PORT_EQUIP = """/* port: SELECT+direita troca as botas, SELECT+esquerda troca a tunica (opcao atalho_roupa). O port
 * manda essas combinacoes como o direcional do controle do N64, que o jogo original nao usa. */
#include "inventory.h"

static s32 Port_NextEquip(s32 type) {
    extern char gEquipAgeReqs[4][4];
    s32 cur = CUR_EQUIP_VALUE(type);
    s32 v = cur;
    s32 i;

    for (i = 0; i < 2; i++) {
        v = (v >= 3) ? 1 : v + 1;
        if (CHECK_OWNED_EQUIP(type, v - 1) &&
            ((gEquipAgeReqs[type][v] == 9) || (gEquipAgeReqs[type][v] == ((void)0, gSaveContext.save.linkAge)))) {
            return v;
        }
    }
    return cur;
}

static void Port_EquipShortcuts(Player* this, PlayState* play, Input* input) {
    extern int port_opt_atalho_roupa;
    s32 type, v;

    if (!port_opt_atalho_roupa || Player_InCsMode(play) || (play->pauseCtx.state != 0) ||
        (play->msgCtx.msgMode != MSGMODE_NONE) || (this->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD))) {
        return;
    }
    if (CHECK_BTN_ALL(input->press.button, BTN_DRIGHT)) {
        type = EQUIP_TYPE_BOOTS;
    } else if (CHECK_BTN_ALL(input->press.button, BTN_DLEFT)) {
        type = EQUIP_TYPE_TUNIC;
    } else {
        return;
    }
    v = Port_NextEquip(type);
    if (v == CUR_EQUIP_VALUE(type)) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    Inventory_ChangeEquipment(type, v);
    Player_SetEquipmentData(play, this);
    Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
}

void Player_Update(Actor* thisx, PlayState* play) {
"""
# opcoes inverter_mira e mira_suave (primeira pessoa e mira). O direcional do Miyoo e digital: o
# "analogico" pula de 0 para o maximo e a mira andava aos trancos. Com mira_suave o valor sobe aos poucos
# enquanto a direcao fica apertada (um toque mexe pouco, segurar acelera ate a velocidade original em
# ~0,6 s), como a aceleracao do Mario Kart 64 do port.
PORT_MIRA = """/* port: mira (primeira pessoa). inverter_mira: para cima olha para cima. mira_suave: aceleracao. */
static s32 Port_MiraEixo(s32 raw, s32* st, s32 fresh) {
    s32 mag = ABS(raw);
    s32 cur;

    if (!({ extern int port_opt_mira_suave; port_opt_mira_suave; })) {
        *st = raw;
        return raw;
    }
    if (raw == 0 || fresh || ((raw > 0) != (*st > 0))) {
        *st = 0;
    }
    if (raw == 0) {
        return 0;
    }
    cur = ABS(*st) + 6;
    if (cur > mag) {
        cur = mag;
    }
    *st = (raw > 0) ? cur : -cur;
    return *st;
}

static void Port_Mira(PlayState* play, s32* x, s32* y) {
    static u32 sLastFrame;
    static s32 sX, sY, sOutX, sOutY;
    s32 rawY = sControlInput->rel.stick_y;

    if (({ extern int port_opt_inverter_mira; port_opt_inverter_mira; })) {
        rawY = -rawY;
    }
    if (play->state.frames != sLastFrame) {
        s32 fresh = play->state.frames != sLastFrame + 1;

        sOutX = Port_MiraEixo(sControlInput->rel.stick_x, &sX, fresh);
        sOutY = Port_MiraEixo(rawY, &sY, fresh);
        sLastFrame = play->state.frames;
    }
    *x = sOutX;
    *y = sOutY;
}

"""
# opcao bau_rapido: todo bau abre com o chute rapido (sem a cena longa de abrir o bau grande)
patch_file("src/overlays/actors/ovl_player_actor/z_player.c", [
    ("atalho de botas e tunica: funcao", "void Player_Update(Actor* thisx, PlayState* play) {\n", PORT_EQUIP),
    # opcao inverter_mira: na primeira pessoa (C-cima, estilingue, arco, gancho, bumerangue) o direcional
    # para cima olha para cima. O original faz como manche de aviao (para cima olha para baixo).
    ("inverter o Y da mira",
     "s16 func_8084ABD8(PlayState* play, Player* this, s32 arg2, s16 arg3) {\n"
     "    s32 temp1;\n    s16 temp2;\n    s16 temp3;\n\n",
     PORT_MIRA +
     "s16 func_8084ABD8(PlayState* play, Player* this, s32 arg2, s16 arg3) {\n"
     "    s32 temp1;\n    s16 temp2;\n    s16 temp3;\n"
     "    s32 portStickY, portStickX;\n\n"
     "    Port_Mira(play, &portStickX, &portStickY); /* port */\n"),
    ("mira suave: X livre",
     "        temp2 = sControlInput->rel.stick_x * -16.0f;\n        temp2 = CLAMP(temp2, -3000, 3000);\n",
     "        temp2 = portStickX * -16.0f; /* port */\n        temp2 = CLAMP(temp2, -3000, 3000);\n"),
    ("mira suave: X mirando",
     "        temp3 = ((sControlInput->rel.stick_x >= 0) ? 1 : -1) *\n"
     "                (s32)((1.0f - Math_CosS(sControlInput->rel.stick_x * 200)) * -1500.0f);\n",
     "        temp3 = ((portStickX >= 0) ? 1 : -1) * /* port */\n"
     "                (s32)((1.0f - Math_CosS(portStickX * 200)) * -1500.0f);\n"),
    ("inverter o Y da mira: modo livre",
     "        temp2 = sControlInput->rel.stick_y * 240.0f;\n        Math_SmoothStepToS(&this->actor.focus.rot.x, temp2, 14, 4000, 30);\n",
     "        if (({ extern int port_opt_mira_suave; port_opt_mira_suave; })) { /* port: acumula como a mira do estilingue, nao volta ao centro */\n"
     "            temp3 = ((portStickY >= 0) ? 1 : -1) * (s32)((1.0f - Math_CosS(portStickY * 200)) * 1500.0f);\n"
     "            this->actor.focus.rot.x += temp3;\n"
     "            this->actor.focus.rot.x = CLAMP(this->actor.focus.rot.x, -14000, 14000);\n"
     "        } else {\n"
     "            temp2 = portStickY * 240.0f; /* port */\n        Math_SmoothStepToS(&this->actor.focus.rot.x, temp2, 14, 4000, 30);\n"
     "        }\n"),
    ("inverter o Y da mira: mirando",
     "        temp3 = ((sControlInput->rel.stick_y >= 0) ? 1 : -1) *\n"
     "                (s32)((1.0f - Math_CosS(sControlInput->rel.stick_y * 200)) * 1500.0f);\n",
     "        temp3 = ((portStickY >= 0) ? 1 : -1) * /* port */\n"
     "                (s32)((1.0f - Math_CosS(portStickY * 200)) * 1500.0f);\n"),
    ("atalho de botas e tunica: chamada", "    Player_UpdateCommon(this, play, &input);\n",
     "    Port_EquipShortcuts(this, play, &input); /* port */\n    Player_UpdateCommon(this, play, &input);\n"),
    ("bau rapido",
     "                if ((giEntry->itemId != ITEM_NONE) && (giEntry->gi >= 0) &&\n",
     "                if (!({ extern int port_opt_bau_rapido; port_opt_bau_rapido; }) && /* port */\n"
     "                    (giEntry->itemId != ITEM_NONE) && (giEntry->gi >= 0) &&\n"),
])
patch_file("src/audio/internal/seqplayer.c", [
    ("u16 da sequencia (LDSEQTOPTR)",
     "channel->unk_22 = *(u16*)(seqPlayer->seqData + (u32)(cmdArgU16 + scriptState->value * 2));",
     "channel->unk_22 = PORT_RD_BE16(seqPlayer->seqData + (u32)(cmdArgU16 + scriptState->value * 2)); /* port */"),
    ("u16 da sequencia (DYNTBLTOPTR)",
     "channel->unk_22 = ((u16*)(channel->dynTable))[scriptState->value];",
     "channel->unk_22 = PORT_RD_BE16((u8*)(channel->dynTable) + scriptState->value * 2); /* port */"),
])
patch_file("src/audio/internal/effects.c", [
    ("envelope delay",
     "adsr->delay = adsr->envelope[adsr->envIndex].delay;",
     "adsr->delay = PORT_BE16S(adsr->envelope[adsr->envIndex].delay); /* port */"),
    ("envelope goto",
     "adsr->envIndex = adsr->envelope[adsr->envIndex].arg;",
     "adsr->envIndex = PORT_BE16S(adsr->envelope[adsr->envIndex].arg); /* port */"),
    ("envelope alvo",
     "adsr->target = adsr->envelope[adsr->envIndex].arg / 32767.0f;",
     "adsr->target = PORT_BE16S(adsr->envelope[adsr->envIndex].arg) / 32767.0f; /* port */"),
])
patch_file("src/audio/internal/data.c", [
    ("gDefaultEnvelope em big-endian",
     "EnvelopePoint gDefaultEnvelope[] = {\n    { 1, 32000 },\n    { 1000, 32000 },\n    { ADSR_HANG, 0 },\n"
     "    { ADSR_DISABLE, 0 },\n};",
     "EnvelopePoint gDefaultEnvelope[] = { /* port: big-endian, como os envelopes da ROM */\n"
     "    { PORT_BE16S(1), PORT_BE16S(32000) },\n    { PORT_BE16S(1000), PORT_BE16S(32000) },\n"
     "    { PORT_BE16S(ADSR_HANG), 0 },\n    { PORT_BE16S(ADSR_DISABLE), 0 },\n};"),
])

# 7b) teste: com --warp (port_main.c) o jogo pula a abertura e a tela de titulo e entra
#     direto num lugar (o mesmo que o menu de mapas do decomp faz), com um save de teste.
#     Serve para o passeio automatico (tools/oot-tour.sh) e para capturar quadros de um lugar.
patch_file("src/code/title_setup.c", [
    ("includes do warp",
     '#include "save.h"\n',
     '#include "save.h"\n#include "sram.h"\n#include "play_state.h"\n#include "scene.h"\n'
     '#include "sequence.h"\n#include "environment.h"\n#include "cic6105.h"\n\n'
     "extern int port_warp_entrance, port_warp_age, port_warp_cs; /* port_main.c */\n"),
    ("warp direto",
     "    SaveContext_Init();\n    this->state.running = false;\n",
     "    SaveContext_Init();\n    this->state.running = false;\n"
     "    if (port_warp_entrance >= 0) { /* port: --warp */\n"
     "        CIC6105_EnableAudio(); /* (quem liga o som e a tela do logo, que o warp pula) */\n"
     "        Sram_InitDebugSave();\n"
     "        gSaveContext.save.linkAge = port_warp_age;\n"
     "        gSaveContext.save.cutsceneIndex = port_warp_cs;\n"
     "        gSaveContext.save.entranceIndex = port_warp_entrance;\n"
     "        gSaveContext.gameMode = GAMEMODE_NORMAL;\n"
     "        gSaveContext.respawnFlag = 0;\n"
     "        gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;\n"
     "        gSaveContext.seqId = (u8)NA_BGM_DISABLED;\n"
     "        gSaveContext.natureAmbienceId = 0xFF;\n"
     "        gSaveContext.showTitleCard = true;\n"
     "        gWeatherMode = 0;\n"
     "        SET_NEXT_GAMESTATE(&this->state, Play_Init, PlayState);\n"
     "        return;\n"
     "    }\n"),
])

# graph.c: antes de comecar a montar um quadro novo na memoria (GfxPool) do quadro de 2 atras, espera o
# desenho adiado dele acabar (a imagem desenhada depois de o jogo saber que a tarefa acabou le aquela
# memoria: lista de desenho, matrizes, vertices; ver gfx_hook.c).
patch_file("src/code/graph.c", [
    ("espera o desenho adiado",
     "void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState) {\n",
     "void Port_GfxBarrier(const void* pool, unsigned size); /* port/gfx/gfx_hook.c */\n\n"
     "void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState) {\n"),
    ("chama a espera",
     "    gameState->inPreNMIState = false;\n    Graph_InitTHGA(gfxCtx);\n",
     "    gameState->inPreNMIState = false;\n"
     "    Port_GfxBarrier(&gGfxPools[gfxCtx->gfxPoolIdx & 1], sizeof(GfxPool)); /* port */\n"
     "    Graph_InitTHGA(gfxCtx);\n"),
    ("conta as tarefas mandadas (Port_GfxIdle)",
     "    osSendMesg(&gScheduler.cmdQueue, (OSMesg)scTask, OS_MESG_BLOCK);\n",
     "    {\n        void Port_GfxSent(void); /* port/gfx/gfx_hook.c */\n\n        Port_GfxSent();\n    }\n"
     "    osSendMesg(&gScheduler.cmdQueue, (OSMesg)scTask, OS_MESG_BLOCK);\n"),
])

# Menu de pausa (z_kaleido_scope.c):
#  - ao abrir, o jogo carrega o Link do menu e os icones EM CIMA da memoria dos objetos da cena, e ao fechar
#    recarrega os objetos em cima dos icones. O ultimo quadro ainda pode estar sendo desenhado aqui (o
#    desenho anda atras do jogo): antes de escrever, espera todo o desenho pendente (Port_GfxIdle). Sem isso,
#    piscada amarela e preta ao abrir o menu e "LOADBLOCK invalido" no log.
#  - quando a pagina do menu termina de girar, a ordem das paginas desenhadas muda: esse quadro nao pode ser
#    misturado com o anterior (Port_FiDontInterpolateFrame). Sem isso, piscava no fim do giro com L/R.
p = os.path.join(oot, "src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c")
s = read(p)
KAL_PROTO = ("void Port_GfxIdle(void);              /* port/gfx/gfx_hook.c */\n"
             "void Port_FiDontInterpolateFrame(void); /* port/frame_interp.c */\n\n")
s = sub(s, "void KaleidoScope_UpdateOpening(PlayState* play) {\n",
        KAL_PROTO + "void KaleidoScope_UpdateOpening(PlayState* play) {\n", "kaleido: prototipos")
# (os prototipos ficam antes de KaleidoScope_UpdateOpening; as chamadas abaixo dela usam eles, e as de
#  cima usam declaracao no proprio bloco)
s = sub(s, "            pauseCtx->playerSegment = (void*)(((uintptr_t)play->objectCtx.spaceStart + 0x30) & ~0x3F);\n",
        "            Port_GfxIdle(); /* port: o ultimo quadro ainda le a memoria dos objetos */\n"
        "            pauseCtx->playerSegment = (void*)(((uintptr_t)play->objectCtx.spaceStart + 0x30) & ~0x3F);\n",
        "kaleido: espera antes de carregar o menu")
s = sub(s, "            pauseCtx->iconItemSegment = (void*)(((uintptr_t)play->objectCtx.spaceStart + 0x30) & ~0x3F);\n",
        "            Port_GfxIdle(); /* port: o ultimo quadro ainda le a memoria dos objetos */\n"
        "            pauseCtx->iconItemSegment = (void*)(((uintptr_t)play->objectCtx.spaceStart + 0x30) & ~0x3F);\n",
        "kaleido: espera antes de carregar o fim de jogo")
# Link da tela de equipamentos em 2x (gfx_hires.c): o quadro em que o jogo copia a imagem do Link para a
# textura do menu marca o endereco dessa textura (24 bits baixos; a RAM do N64 fica em 0x80000000)
s = sub(s, "    PreRender_SaveFramebuffer(&sPlayerPreRender, &gfx);\n",
        "    gfx->words.w0 = (u32)(uintptr_t)sPlayerPreRender.fbufSave & 0xFFFFFF; /* port: textura do Link do menu */\n"
        "    gfx->words.w1 = 0x54585439;\n"
        "    gfx++;\n"
        "    PreRender_SaveFramebuffer(&sPlayerPreRender, &gfx);\n",
        "kaleido: textura do Link do menu em 2x")
RELOAD = "Object_ReloadAll(&play->objectCtx);\n"
n = s.count(RELOAD)
if n != 2:
    sys.exit("ERRO no patch 'kaleido: espera antes de recarregar os objetos': encontrado %d vezes (esperado 2)" % n)
s = s.replace(RELOAD, "Port_GfxIdle(); /* port: o ultimo quadro do menu ainda le os icones */ " + RELOAD)
PAGE = "pauseCtx->pageIndex = sPageSwitchNextPageIndex[pauseCtx->nextPageMode];\n"
n = s.count(PAGE)
if n < 2:
    sys.exit("ERRO no patch 'kaleido: troca de pagina': encontrado %d vezes" % n)
s = s.replace(PAGE, "{ void Port_FiDontInterpolateFrame(void); Port_FiDontInterpolateFrame(); } /* port */ " + PAGE)
write(os.path.join(tree, "src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c"), s)
patched.append("src/overlays/misc/ovl_kaleido_scope/z_kaleido_scope.c (troca de pagina: %d)" % n)

# game.c: ao trocar de estado do jogo (fim da abertura, tela titulo -> jogo...) a memoria do estado e
# liberada logo depois da ultima tarefa grafica; o desenho adiado dela tem de acabar antes.
patch_file("src/code/game.c", [
    ("prototipo da espera",
     "void GameState_Destroy(GameState* gameState) {\n",
     "void Port_GfxBarrier(const void* pool, unsigned size); /* port/gfx/gfx_hook.c */\n\n"
     "void GameState_Destroy(GameState* gameState) {\n"),
    ("espera o desenho adiado antes de liberar a memoria",
     "    osRecvMesg(&gameState->gfxCtx->queue, NULL, OS_MESG_BLOCK);\n",
     "    osRecvMesg(&gameState->gfxCtx->queue, NULL, OS_MESG_BLOCK);\n"
     "    Port_GfxBarrier(NULL, 0xFFFFFFFFu); /* port: qualquer desenho adiado */\n"),
])

# melhorias do Ship of Harkinian (tools/oot-patch-soh.py; opcoes na secao Enhancements do options.txt)
exec(compile(read(os.path.join(os.path.dirname(os.path.abspath(__file__)), "oot-patch-soh.py")), "oot-patch-soh.py", "exec"))

# quadros intermediarios: as cortinas pretas do L1 (letterbox) abrem e fecham a 30 fps. O tamanho muda a cada quadro
# do jogo (20 por segundo); os retangulos das cortinas, os preenchimentos da tela e a tesoura do 3D (que acompanha as
# cortinas) ganham o tamanho intermediario nas imagens do meio (Port_FiCmd tipo 2, gfx_rdp.c). As cortinas ficam num
# ramo proprio da gravacao (so existem com tamanho > 0: nao desalinham o resto).
PORT_FI_CMD = "{ extern void Port_FiCmd(const void* cmd, int kind); Port_FiCmd(%s - 1, 2); } /* port */\n"
patch_file("src/code/z_rcp.c", [
    ("quadros intermediarios: fundo dentro das cortinas",
     "        gDPFillRectangle(POLY_OPA_DISP++, 0, letterboxSize, gScreenWidth - 1, gScreenHeight - letterboxSize - 1);\n",
     "        gDPFillRectangle(POLY_OPA_DISP++, 0, letterboxSize, gScreenWidth - 1, gScreenHeight - letterboxSize - 1);\n"
     "        " + PORT_FI_CMD % "POLY_OPA_DISP", 2),
    ("quadros intermediarios: cortinas",
     "            gDPFillRectangle(OVERLAY_DISP++, 0, 0, gScreenWidth - 1, letterboxSize - 1);\n"
     "            gDPFillRectangle(OVERLAY_DISP++, 0, gScreenHeight - letterboxSize, gScreenWidth - 1, gScreenHeight - 1);\n",
     "            { extern void Port_FiOpenChild(const void* a, int b); static char sPortLetterboxKey;\n"
     "              Port_FiOpenChild(&sPortLetterboxKey, 1); } /* port */\n"
     "            gDPFillRectangle(OVERLAY_DISP++, 0, 0, gScreenWidth - 1, letterboxSize - 1);\n"
     "            " + PORT_FI_CMD % "OVERLAY_DISP" +
     "            gDPFillRectangle(OVERLAY_DISP++, 0, gScreenHeight - letterboxSize, gScreenWidth - 1, gScreenHeight - 1);\n"
     "            " + PORT_FI_CMD % "OVERLAY_DISP" +
     "            { extern void Port_FiCloseChild(void); Port_FiCloseChild(); } /* port */\n"),
])
patch_file("src/code/z_view.c", [
    ("quadros intermediarios: tesoura das cortinas",
     "    gDPSetScissor(POLY_OPA_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);\n    gDPPipeSync(POLY_XLU_DISP++);\n"
     "    gDPSetScissor(POLY_XLU_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);\n",
     "    gDPSetScissor(POLY_OPA_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);\n"
     "    " + PORT_FI_CMD % "POLY_OPA_DISP" +
     "    gDPPipeSync(POLY_XLU_DISP++);\n"
     "    gDPSetScissor(POLY_XLU_DISP++, G_SC_NON_INTERLACE, ulx, uly, lrx, lry);\n"
     "    " + PORT_FI_CMD % "POLY_XLU_DISP"),
])

# Estado rapido / Game Switcher com o som junto: os dados do som (src/audio/*, gAudioHeap) agora vao para o estado
# (oot-build.sh), e a musica continua do ponto em que o estado foi salvo. A copia so e feita com o som parado num ponto
# conhecido: a thread do som estaciona no comeco do AudioMgr_HandleRetrace (a tarefa de som anterior ja terminou e
# nenhuma leitura da ROM para o som esta em andamento) enquanto a thread grafica copia ou restaura (graph.c).
patch_file("src/code/audio_thread_manager.c", [
    ("estado rapido: som estacionado",
     "void AudioMgr_HandleRetrace(AudioMgr* audioMgr) {\n    AudioTask* rspTask;\n\n",
     """/* port: estado rapido com o som (graph.c Port_SsFrame chama Port_AudioPark / Port_AudioUnpark) */
static volatile s32 sPortAudioPark;
static OSMesgQueue sPortAudioAckQ, sPortAudioGoQ;
static OSMesg sPortAudioAckBuf[1], sPortAudioGoBuf[1];
static s32 sPortAudioQInit;
/* a tarefa de som que o AudioMgr vai mandar no proximo retrace: faz parte do estado (aponta para dentro do gAudioCtx) */
__attribute__((section("game_data"))) AudioTask* gPortAudioRspTask = NULL;

static void Port_AudioQInit(void) {
    if (!sPortAudioQInit) {
        osCreateMesgQueue(&sPortAudioAckQ, sPortAudioAckBuf, 1);
        osCreateMesgQueue(&sPortAudioGoQ, sPortAudioGoBuf, 1);
        sPortAudioQInit = true;
    }
}

/* 1 = a thread do som esta parada (pode copiar/restaurar os dados do som); 0 = nao respondeu (segue sem o som) */
s32 Port_AudioPark(void) {
    extern int os_hw_pi_pending(void);
    extern int usleep(unsigned int);
    s32 i;

    Port_AudioQInit();
    while (osRecvMesg(&sPortAudioAckQ, NULL, OS_MESG_NOBLOCK) == 0) {}
    while (osRecvMesg(&sPortAudioGoQ, NULL, OS_MESG_NOBLOCK) == 0) {}
    sPortAudioPark = true;
    for (i = 0; i < 400; i++) { /* (ate ~0,5 s; o som roda a cada retrace, 16 ms) */
        if (osRecvMesg(&sPortAudioAckQ, NULL, OS_MESG_NOBLOCK) == 0) {
            break;
        }
        usleep(1000);
    }
    if (i == 400) {
        sPortAudioPark = false;
        osSendMesg(&sPortAudioGoQ, NULL, OS_MESG_NOBLOCK); /* (se ele estacionar atrasado, segue) */
        PRINTF("[estado] o som nao parou a tempo\\n");
        return false;
    }
    for (i = 0; i < 2000 && os_hw_pi_pending(); i++) { /* leituras da ROM pedidas pelo som terminam */
        usleep(250);
    }
    osRecvMesg(&sPortAudioAckQ, NULL, OS_MESG_NOBLOCK); /* (entrega as mensagens dessas leituras) */
    return true;
}

void Port_AudioUnpark(void) {
    sPortAudioPark = false;
    osSendMesg(&sPortAudioGoQ, NULL, OS_MESG_NOBLOCK);
}

void AudioMgr_HandleRetrace(AudioMgr* audioMgr) {
    AudioTask* rspTask;

    if (sPortAudioPark) { /* port: estado rapido */
        /* a tarefa mandada no retrace anterior termina (a fila tem 1 vaga: a mensagem volta para o proximo retrace) */
        osRecvMesg(&audioMgr->taskDoneQueue, NULL, OS_MESG_BLOCK);
        osSendMesg(&audioMgr->taskDoneQueue, NULL, OS_MESG_NOBLOCK);
        gPortAudioRspTask = audioMgr->rspTask;
        osSendMesg(&sPortAudioAckQ, NULL, OS_MESG_NOBLOCK);
        osRecvMesg(&sPortAudioGoQ, NULL, OS_MESG_BLOCK);
        audioMgr->rspTask = gPortAudioRspTask; /* (a do estado carregado, ou a mesma) */
        return;
    }

"""),
])

# Recarga dos overlays como no N64 (port/ovl_reset.c, tools/oot-ovl.py): no N64 cada ator/efeito/tela/kaleido vem da
# ROM com os dados iniciais toda vez que e carregado. Aqui avisamos o port onde o N64 carregaria.
PORT_OVL_DECL = ("void Port_OvlActorLoad(int id); void Port_OvlActorRef(int id); "
                 "void Port_OvlActorRelease(int id, int persistent, int drop); void Port_OvlActorFreeAll(void); /* port */\n")
patch_file("src/code/z_actor.c", [
    ("overlays: cena nova",
     "        overlayEntry->numLoaded = 0;\n        overlayEntry++;\n    }\n",
     "        overlayEntry->numLoaded = 0;\n        overlayEntry++;\n    }\n"
     "    { " + PORT_OVL_DECL + "      Port_OvlActorFreeAll(); } /* port: overlays de atores carregados de novo */\n"),
    ("overlays: carga do ator",
     "    objectSlot = Object_GetSlot(&play->objectCtx, profile->objectId);\n",
     "    { " + PORT_OVL_DECL + "      Port_OvlActorLoad(actorId); } /* port: dados do overlay como o Overlay_Load deixa */\n"
     "    objectSlot = Object_GetSlot(&play->objectCtx, profile->objectId);\n"),
    ("overlays: ator que nao nasceu",
     "        Actor_FreeOverlay(overlayEntry);\n        return NULL;\n",
     "        { " + PORT_OVL_DECL + "          Port_OvlActorRelease(actorId, overlayEntry->allocType & ACTOROVL_ALLOC_PERSISTENT, 0); }\n"
     "        Actor_FreeOverlay(overlayEntry);\n        return NULL;\n", 2),
    ("overlays: mais uma instancia",
     "    overlayEntry->numLoaded++;\n",
     "    overlayEntry->numLoaded++;\n"
     "    { " + PORT_OVL_DECL + "      Port_OvlActorRef(actorId); } /* port */\n"),
    ("overlays: instancia apagada",
     "    ZELDA_ARENA_FREE(actor, \"../z_actor.c\", 7242);\n",
     "    ZELDA_ARENA_FREE(actor, \"../z_actor.c\", 7242);\n"
     "    { " + PORT_OVL_DECL + "      Port_OvlActorRelease(overlayEntry - gActorOverlayTable,\n"
     "                           overlayEntry->allocType & ACTOROVL_ALLOC_PERSISTENT, 1); } /* port */\n"),
])
patch_file("src/code/z_effect_soft_sprite.c", [
    ("overlays: efeitos liberados",
     "        overlay->loadedRamAddr = NULL;\n        overlay++;\n    }\n",
     "        overlay->loadedRamAddr = NULL;\n        overlay++;\n    }\n"
     "    { void Port_OvlEffectFreeAll(void); Port_OvlEffectFreeAll(); } /* port: carregados de novo no proximo uso */\n", 2),
    ("overlays: carga do efeito",
     "    sEffectSsInfo.searchStartIndex = index + 1;\n    overlaySize = (uintptr_t)overlayEntry->vramEnd - (uintptr_t)overlayEntry->vramStart;\n",
     "    sEffectSsInfo.searchStartIndex = index + 1;\n    overlaySize = (uintptr_t)overlayEntry->vramEnd - (uintptr_t)overlayEntry->vramStart;\n"
     "    { void Port_OvlEffectLoad(int type); Port_OvlEffectLoad(type); } /* port: dados do overlay como o Overlay_Load deixa */\n"),
])
patch_file("src/code/z_kaleido_manager.c", [
    ("overlays: menu de pausa / Link",
     "    ovl->loadedRamAddr = sKaleidoAreaPtr;\n",
     "    ovl->loadedRamAddr = sKaleidoAreaPtr;\n"
     "    { void Port_OvlKaleidoLoad(int index); Port_OvlKaleidoLoad(ovl - gKaleidoMgrOverlayTable); } /* port */\n"),
])
patch_file("src/code/graph.c", [
    ("overlays: tela nova",
     "        Overlay_LoadGameState(ovl);\n",
     "        Overlay_LoadGameState(ovl);\n"
     "        { void Port_OvlGameStateLoad(int index); Port_OvlGameStateLoad(ovl - gGameStateOverlayTable); } /* port */\n"),
])

# 7) texturas compiladas dentro do codigo (u64 gFoo[] = { #include "x.inc.c" }):
#    os literais u64 do .inc.c foram escritos pensando em N64 (big-endian). Num ARM/x86 o
#    u64 fica na memoria com os bytes ao contrario. Copiamos cada .inc.c usado pelo src
#    com os 8 bytes de cada literal invertidos: a memoria passa a ter os bytes ORIGINAIS
#    da textura (e o renderizador le textura como bytes N64, igual as texturas da ROM).

HEX64 = re.compile(r"0x([0-9A-Fa-f]{16})(?![0-9A-Fa-f])")


def swap64_lit(m):
    h = m.group(1)
    return "0x" + "".join(h[i:i + 2] for i in range(14, -2, -2))


inc_re = re.compile(r'#\s*include\s+"([^"]+\.inc\.c)"')
wanted = set()
for root, _, files in os.walk(os.path.join(oot, "src")):
    for fn in files:
        if fn.endswith(".c") and not fn.endswith(".inc.c"):
            for m in inc_re.finditer(read(os.path.join(root, fn))):
                wanted.add(m.group(1))
swapped = 0
swapped32 = 0
HEX32 = re.compile(r"0x([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")


def swap32_lit(m):
    h = m.group(1)
    return "0x" + h[6:8] + h[4:6] + h[2:4] + h[0:2]


missing_inc = []
# tambem os .inc.c dos assets (usados pela compilacao dos assets em oot-assets.py)
for base in ("build/ntsc-1.1", "extracted/ntsc-1.1"):
    top = os.path.join(oot, base, "assets")
    for root, _, files in os.walk(top):
        for fn in files:
            if fn.endswith(".inc.c"):
                wanted.add(os.path.relpath(os.path.join(root, fn), os.path.join(oot, base)))
for rel in sorted(wanted):
    found = None
    for base in ("build/ntsc-1.1", "extracted/ntsc-1.1", ""):
        cand = os.path.join(oot, base, rel)
        if os.path.isfile(cand):
            found = cand
            break
    if not found:
        missing_inc.append(rel)
        continue
    s = read(found)
    if rel.endswith(".u32.inc.c"):
        # texturas/paletas com alinhamento de 4 (o extrator usa u32 em vez de u64): mesma ideia,
        # cada literal de 32 bits com os 4 bytes invertidos (sem isso o mapa de conversao as
        # trocava de 4 em 4 e o renderizador, que le textura como bytes do N64, via cores erradas:
        # rostos roxos, letras faltando...)
        if HEX32.search(s):
            write(os.path.join(tree, rel), HEX32.sub(swap32_lit, s))
            swapped32 += 1
    elif HEX64.search(s):
        write(os.path.join(tree, rel), HEX64.sub(swap64_lit, s))
        swapped += 1

# sinf/cosf do libultra: tabelas de f64 escritas como pares {palavra alta, palavra baixa}.
# No little-endian a palavra baixa vem primeiro na memoria: invertemos cada par (sem isso
# sinf/cosf devolvem lixo e a perspectiva sai com cot(fov/2) = infinito).
DU_PAIR = re.compile(r"\{\s*(0x[0-9A-Fa-f]{8}),\s*(0x[0-9A-Fa-f]{8})\s*\}")
for rel in ("src/libultra/gu/sinf.c", "src/libultra/gu/cosf.c"):
    s = read(os.path.join(oot, rel))
    s, n = DU_PAIR.subn(lambda m: "{ %s, %s }" % (m.group(2), m.group(1)), s)
    if n < 6:
        sys.exit("ERRO no patch de %s: so %d pares f64 encontrados (esperado >= 6)" % (rel, n))
    write(os.path.join(tree, rel), s)
    patched.append(rel + " (tabelas f64 invertidas: %d)" % n)

# sys_matrix.c e z_skin_matrix.c montam/leem Mtx (formato do N64: 8 palavras de parte inteira e 8 de
# fracao) com meias-palavras de 16 bits (u16) por indice. No big-endian o indice par e a metade
# ALTA da palavra; no little-endian e a BAIXA. Trocamos a paridade de cada indice (k -> k ^ 1)
# para a memoria ficar igual a do N64 (e para os dois sentidos, escrever e ler, concordarem).
U16_IDX = re.compile(r"\b(intPart|fracPart|m1|m2)\[([^\]\n]+)\]")


def u16_swap(m):
    expr = m.group(2).strip()
    if expr.isdigit():
        return "%s[%s ^ 1]" % (m.group(1), expr)
    return "%s[(%s) ^ 1]" % (m.group(1), expr)


FI_MATRIX = [
    # quadros intermediarios (port/frame_interp.c): avisa as matrizes que viram Mtx e o que o SoH usa para
    # saber se a matriz depende da do ator (profundidade da pilha e operacoes que comecam matriz nova)
    ("push", "void Matrix_Push(void) {\n", "void Matrix_Push(void) {\n    Port_FiPush(); /* port */\n"),
    ("pop", "void Matrix_Pop(void) {\n", "void Matrix_Pop(void) {\n    Port_FiPop(); /* port */\n"),
    ("translate", "void Matrix_Translate(f32 x, f32 y, f32 z, u8 mode) {\n",
     "void Matrix_Translate(f32 x, f32 y, f32 z, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("scale", "void Matrix_Scale(f32 x, f32 y, f32 z, u8 mode) {\n",
     "void Matrix_Scale(f32 x, f32 y, f32 z, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("rotx", "void Matrix_RotateX(f32 x, u8 mode) {\n", "void Matrix_RotateX(f32 x, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("roty", "void Matrix_RotateY(f32 y, u8 mode) {\n", "void Matrix_RotateY(f32 y, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("rotz", "void Matrix_RotateZ(f32 z, u8 mode) {\n", "void Matrix_RotateZ(f32 z, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("rotzyx", "void Matrix_RotateZYX(s16 x, s16 y, s16 z, u8 mode) {\n",
     "void Matrix_RotateZYX(s16 x, s16 y, s16 z, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("rotaxis", "void Matrix_RotateAxis(f32 angle, Vec3f* axis, u8 mode) {\n",
     "void Matrix_RotateAxis(f32 angle, Vec3f* axis, u8 mode) {\n    Port_FiMode(mode); /* port */\n"),
    ("translate rotate yxz",
     "        cmf->yx = temp2 * sin;\n        cmf->yy = temp2 * cos;\n    } else {\n        cmf->yx = 0.0f;\n    }\n}\n\n"
     "Mtx* Matrix_MtxFToMtx(MtxF* src, Mtx* dest) {\n",
     "        cmf->yx = temp2 * sin;\n        cmf->yy = temp2 * cos;\n    } else {\n        cmf->yx = 0.0f;\n    }\n"
     "    Port_FiSetTRYXZ(translateX, translateY, translateZ, &rot->x, &cmf->mf[0][0]); /* port */\n}\n\n"
     "Mtx* Matrix_MtxFToMtx(MtxF* src, Mtx* dest) {\n"
     "    Port_FiMtxF(&src->mf[0][0], dest); /* port */\n"),
    ("tomtx",
     "Mtx* Matrix_ToMtx(Mtx* dest) {\n    return Matrix_MtxFToMtx(sCurrentMatrix, dest);\n}\n",
     "Mtx* Matrix_ToMtx(Mtx* dest) {\n"
     "    Mtx* portRet_;\n\n"
     "    Port_FiToMtx(&sCurrentMatrix->mf[0][0], dest); /* port: quadros intermediarios */\n"
     "    Port_FiSkip(1);\n"
     "    portRet_ = Matrix_MtxFToMtx(sCurrentMatrix, dest);\n"
     "    Port_FiSkip(-1);\n"
     "    return portRet_;\n}\n"),
]
for rel, minimum in (("src/code/sys_matrix.c", 70), ("src/code/z_skin_matrix.c", 30)):
    s = read(os.path.join(oot, rel))
    if rel == "src/code/sys_matrix.c":
        for what, old, new in FI_MATRIX:
            s = sub(s, old, new, rel + ": quadros intermediarios: " + what)
    else:
        # a Epona (e outros modelos com "skin") e varios efeitos montam a Mtx por aqui, sem passar pelo
        # Matrix_ToMtx: sem isto o cavalo andava a 20 quadros por segundo com o resto a 30
        s = sub(s, "void SkinMatrix_MtxFToMtx(MtxF* src, Mtx* dest) {\n",
                "void SkinMatrix_MtxFToMtx(MtxF* src, Mtx* dest) {\n"
                "    Port_FiMtxF(&src->mf[0][0], dest); /* port: quadros intermediarios */\n",
                rel + ": quadros intermediarios")
    s, n = U16_IDX.subn(u16_swap, s)
    if n < minimum:
        sys.exit("ERRO no patch de %s: so %d acessos u16 encontrados (esperado >= %d)" % (rel, n, minimum))
    write(os.path.join(tree, rel), s)
    patched.append(rel + " (indices u16 de Mtx: %d)" % n)

print("patch ok: %d includes copiados, %d fontes ajustados:" % (1, len(patched)))
for r in patched:
    print("   " + r)
print("   %d texturas .inc.c (u64) e %d (u32) copiadas com bytes invertidos (de %d referenciadas)"
      % (swapped, swapped32, len(wanted)))
if missing_inc:
    print("   AVISO: %d .inc.c referenciados nao encontrados, ex.: %s" % (len(missing_inc), missing_inc[0]))
