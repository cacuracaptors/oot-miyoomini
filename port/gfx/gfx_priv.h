/*
 * gfx_priv.h -- renderizador do port (RSP F3DZEX2 + RDP em software).
 *
 * Convencoes (importante):
 *  - Os comandos (Gfx = 2 palavras u32) sao lidos como u32 NATIVOS: os feitos pelo jogo ja saem
 *    nativos e os que vem da ROM foram convertidos pelo mapa (swap de 4 em 4).
 *  - Texturas, paletas (TLUT), JPEG ficam como BYTES ORIGINAIS do N64 (big-endian).
 *  - Estruturas (Vtx, Light, Vp) sao lidas nativas (campos s16 etc.).
 *  - Matrizes (Mtx): palavras u32 (parte inteira e fracao), como o guMtxF2L do jogo escreve.
 *  - Enderecos: o byte alto (4 bits) escolhe o segmento; o jogo guarda a base do segmento
 *    como (ponteiro - 0x80000000). O endereco real = 0x80000000 + ((base+offset) & 0x1FFFFFFF).
 *    O executavel e a RAM do N64 ficam em 0x80000000..0x80FFFFFF (ver oot-build.sh).
 *  - Framebuffer/z-buffer ficam na memoria do jogo: cor em RGBA5551 com os bytes na ordem
 *    do N64 (alto, baixo); o z-buffer tambem (u16 com os bytes na ordem do N64).
 */
#ifndef GFX_PRIV_H
#define GFX_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ comandos (F3DZEX2 / RDP) */
enum {
    GX_NOOP = 0x00, GX_VTX = 0x01, GX_MODIFYVTX = 0x02, GX_CULLDL = 0x03, GX_BRANCH_Z = 0x04,
    GX_TRI1 = 0x05, GX_TRI2 = 0x06, GX_QUAD = 0x07, GX_LINE3D = 0x08,
    /* S2DEX2 (fundos pre-renderizados): nao colidem com os codigos do F3DZEX2 */
    GX_BG_1CYC = 0x09, GX_BG_COPY = 0x0A, GX_OBJ_RENDERMODE = 0x0B,
    GX_TEXTURE = 0xD7, GX_POPMTX = 0xD8, GX_GEOMETRYMODE = 0xD9, GX_MTX = 0xDA, GX_MOVEWORD = 0xDB,
    GX_MOVEMEM = 0xDC, GX_LOAD_UCODE = 0xDD, GX_DL = 0xDE, GX_ENDDL = 0xDF, GX_SPNOOP = 0xE0,
    GX_RDPHALF_1 = 0xE1, GX_SETOTHERMODE_L = 0xE2, GX_SETOTHERMODE_H = 0xE3, GX_TEXRECT = 0xE4,
    GX_TEXRECTFLIP = 0xE5, GX_RDPLOADSYNC = 0xE6, GX_RDPPIPESYNC = 0xE7, GX_RDPTILESYNC = 0xE8,
    GX_RDPFULLSYNC = 0xE9, GX_SETKEYGB = 0xEA, GX_SETKEYR = 0xEB, GX_SETCONVERT = 0xEC,
    GX_SETSCISSOR = 0xED, GX_SETPRIMDEPTH = 0xEE, GX_RDPSETOTHERMODE = 0xEF, GX_LOADTLUT = 0xF0,
    GX_RDPHALF_2 = 0xF1, GX_SETTILESIZE = 0xF2, GX_LOADBLOCK = 0xF3, GX_LOADTILE = 0xF4,
    GX_SETTILE = 0xF5, GX_FILLRECT = 0xF6, GX_SETFILLCOLOR = 0xF7, GX_SETFOGCOLOR = 0xF8,
    GX_SETBLENDCOLOR = 0xF9, GX_SETPRIMCOLOR = 0xFA, GX_SETENVCOLOR = 0xFB, GX_SETCOMBINE = 0xFC,
    GX_SETTIMG = 0xFD, GX_SETZIMG = 0xFE, GX_SETCIMG = 0xFF,
    GX_RDPNOOP = 0xC0
};

/* movemem / moveword */
#define GX_MV_VIEWPORT 8
#define GX_MV_LIGHT 10
#define GX_MW_NUMLIGHT 0x02
#define GX_MW_CLIP 0x04
#define GX_MW_SEGMENT 0x06
#define GX_MW_FOG 0x08
#define GX_MW_LIGHTCOL 0x0A
#define GX_MW_PERSPNORM 0x0E

/* geometry mode */
/* menu de pausa opaco: dentro das marcas 0x5458543A/B os texels com algum alfa passam a alfa 255 (gfx_rdp.c) */
extern int gx_solid_alpha, gx_menu_opaco, gx_logo_opaca, gx_menu_320;
void gx_hr_menu_mark(int open);
void gx_ge_logo_mark(void);
extern int gx_hr_le; /* opcao cor_direta (gfx_hires.c) */
extern int gx_pontilhado; /* opcao pontilhado (gfx_tri.c) */
void gx_tex_acct(int64_t delta); /* (gfx_rdp.c) memoria a mais/menos de uma textura na cache */
extern int gx_logo_320; /* gfx_hires.c: o menu de pausa esta sendo desenhado neste desenho */

/* LOD_FRACTION do combinador. O port nao calcula LOD: com 0, so a 1a textura aparecia (o portal azul dos chefes
 * perdia as pontas de chama). Com o LOD de textura DESLIGADO (bit 16 do omH) vale 255 = so a 2a textura, como no
 * jogo original; com o LOD ligado fica 0 (comportamento antigo: a base, nitida). */
#define GX_LODF_SEM_LOD ((gx.omH & 0x10000u) ? 0 : 255)
#define GX_G_ZBUFFER 0x00000001u
#define GX_G_SHADE 0x00000004u
#define GX_G_SHADING_SMOOTH 0x00200000u
#define GX_G_CULL_FRONT 0x00000200u
#define GX_G_CULL_BACK 0x00000400u
#define GX_G_FOG 0x00010000u
#define GX_G_LIGHTING 0x00020000u
#define GX_G_TEXTURE_GEN 0x00040000u
#define GX_G_TEXTURE_GEN_LINEAR 0x00080000u

/* other mode H (shifts) */
#define GX_CYC_SHIFT 20
#define GX_CYC_1 0
#define GX_CYC_2 1
#define GX_CYC_COPY 2
#define GX_CYC_FILL 3
#define GX_OMH_PERSP (1u << 19)
#define GX_OMH_LOD (1u << 16)
/* other mode L (render mode) */
#define GX_RM_AA_EN 0x0008u
#define GX_RM_Z_CMP 0x0010u
#define GX_RM_Z_UPD 0x0020u
#define GX_RM_IM_RD 0x0040u
#define GX_RM_CLR_ON_CVG 0x0080u
#define GX_RM_ZMODE_MASK 0x0C00u
#define GX_RM_ZMODE_OPA 0x0000u
#define GX_RM_ZMODE_INTER 0x0400u
#define GX_RM_ZMODE_XLU 0x0800u
#define GX_RM_ZMODE_DEC 0x0C00u
#define GX_RM_CVG_X_ALPHA 0x1000u
#define GX_RM_ALPHA_CVG_SEL 0x2000u
#define GX_RM_FORCE_BL 0x4000u

/* ------------------------------------------------------------ tipos */
typedef struct {
    uint32_t w0, w1;
} GxCmd;

typedef struct {
    float x, y, z, w;     /* espaco de recorte */
    float sx, sy, sz;     /* tela (pixels; sz 0..1) */
    float s, t;           /* coordenadas de textura (texels * 32, S10.5) */
    float r, g, b, a;     /* sombreamento 0..255 */
    float fog;            /* 0..255 */
    uint8_t clip;         /* bits: 1 esq, 2 dir, 4 baixo, 8 cima, 16 perto, 32 longe */
    uint8_t valid;
} GxVtx;

typedef struct {
    uint8_t fmt, siz;     /* formato / tamanho do texel */
    uint16_t line;        /* palavras de 64 bits por linha */
    uint16_t tmem;        /* endereco em palavras de 64 bits */
    uint8_t palette;
    uint8_t cmt, cms;     /* bit0 espelho, bit1 clamp */
    uint8_t maskt, masks;
    uint8_t shiftt, shifts;
    uint16_t uls, ult, lrs, lrt; /* 10.2 */
} GxTile;

typedef struct GxTex_ {
    int w, h;             /* dimensoes decodificadas (potencia de 2 se ha mascara) */
    int clipw, cliph;     /* dimensao util (para clamp) */
    uint32_t *px;         /* RGBA8888, byte 0 = R */
    uint64_t key;
    uint32_t use;
    /* copia com 1 texel de borda (caminho rapido, gfx_fast.inc): pp[(t+1)*pw + (s+1)], ja com a borda
     * repetida/espelhada/presa como a tile pede (pkey); feita na 1a vez que o caminho rapido usa */
    uint32_t *pp;
    int pw;
    uint32_t pkey;
    /* filtro pre-calculado (opcao filtro_rapido, gfx_fast.inc): 4x4 celulas por texel, cada uma ja
     * com o filtro de 3 pontos do meio dela; feito a partir de pp (mesma chave pkey) */
    uint32_t *pf;
    int pfw;
    uint32_t pfBytes; /* memoria do filtro pre-calculado (conta na cache: gx_tex_acct) */
    /* tabela 4x4 so para os triangulos de perto (opcao filtro_perto_fino; gfx_fast.inc hd_close) */
    uint32_t *pf4;
    int pf4w;
    uint32_t pf4key, pf4Bytes;
    uint32_t pfkey;
    /* textura nova do pacote de texturas (gfx_hd.c): hdk vezes maior, mesma tile; NULL = nenhuma */
    struct GxTex_ *hd;
    int hdk;     /* escala desta textura sobre a original (1 = a propria original) */
    int hdTried; /* ja procurou no pacote */
    int hdPend;  /* textura nova sendo carregada em segundo plano: indice no pacote + 1 (0 = nenhuma) */
    int hdRow;   /* pedaco de uma textura grande: 1a linha (na original) */
    int hdCol;   /* e 1a coluna */
    int solid;   /* alfa solido pedido quando foi decodificada (1 = menu de pausa, 2 = logo): vale tambem para a textura nova */
    /* retangulo dos texels com alfa > 0 (gfx_fast.inc, recorte dos transparentes): x0, y0, x1, y1 (x1/y1 exclusivos);
     * bbKey = key da textura quando foi calculado (0 = ainda nao) */
    int16_t bb[4];
    uint64_t bbKey;
} GxTex;

/* sonda de pixel (--probe X,Y): guarda a ultima escrita naquele pixel, por framebuffer */
#define GX_PROBE_MAX 4
#define GX_PROBE_SLOTS 4
typedef struct {
    uint8_t *fb;          /* framebuffer desta fatia (NULL = livre) */
    char kind;            /* 'T' triangulo, 'R' retangulo, 'C' copia, 'F' preenchimento */
    uint32_t task;        /* numero da tarefa de gfx que escreveu */
    uint32_t cmd;         /* endereco do comando da display list */
    uint32_t geom, omH, omL, combW0, combW1;
    int texTile;
    GxTile tile[2];
    int texW[2], texH[2];
    uint32_t texSrc[2];   /* endereco (SETTIMG) que carregou a TMEM da tile */
    int V[32];            /* entradas do combinador (ver gfx_tri.c) */
    int cc[4], aIn, out[3];
    int z14, zm, useZ;
    float S, T;
    uint8_t fogc[4], blendc[4];
    uint32_t fill;
    /* tentativas que nao escreveram nesta tarefa */
    uint32_t rejTask, rejZ, rejAlpha;
} GxProbeSlot;
typedef struct {
    int x, y;
    GxProbeSlot slot[GX_PROBE_SLOTS];
} GxProbe;

/* (int)floorf(f) sem chamar a libm (no ARMv7 floorf e uma chamada de funcao). Igual a (int)floorf(f)
 * para |f| < 2^31, so que subnormais contam como zero (como no NEON, que o caminho rapido usa):
 * referencia e caminho rapido dao o mesmo resultado. */
static inline int gx_floori(float f) {
    int i;
    if (f > -1.17549435e-38f && f < 1.17549435e-38f) {
        return 0;
    }
    i = (int)f;
    return i - (f < (float)i);
}

/* primitiva pronta para os spans (ver gfx_tri.c) */
enum { GXP_R = 0, GXP_G, GXP_B, GXP_A, GXP_Z };
typedef struct {
    int64_t c[5];        /* valor em (0,0): r,g,b,a em 8.16 e z em 14.10 */
    int32_t dx[5], dy[5];
    float f0[3], fdx[3], fdy[3]; /* S*q, T*q, q no ponto (fx0,fy0) e gradientes */
    float fx0, fy0;
    int useZ, zTol, hasTex;
} GxPrim;

/* ---- captura de quadros (gfx_capture.c): estado do RDP/RSP que sobrevive entre tarefas ----
 * So tipos de tamanho fixo (sem ponteiros), para o arquivo valer no i386 do jogo e no x86-64 do reprodutor. */
typedef struct {
    uint32_t omL, omH, combW0, combW1;
    uint8_t prim[4], env[4], fogc[4], blendc[4];
    uint8_t primLodFrac, primLodMin, pad0[2];
    uint32_t fillColor;
    uint16_t primZ, primDZ;
    uint32_t scissorX0, scissorY0, scissorX1, scissorY1;
    GxTile tile[8];
    uint8_t tmem[4096];
    uint32_t timgAddr, cimgAddr, zimgAddr;
    uint8_t timgFmt, timgSiz, cimgFmt, cimgSiz;
    uint16_t timgWidth, cimgWidth;
    int32_t texOn, texTile, texLevel;
    float texScaleS, texScaleT;
    int16_t fogMul, fogOff;
    uint32_t rdphalf1, rdphalf2;
    struct { uint8_t col[3], colc[3]; int8_t dir[3]; } light[10];
    struct { int8_t dir[3]; } lookat[2];
} GxCapState;

/* parametros de um triangulo ja decidido para o rasterizador */
typedef struct GxRaster GxRaster;

/* ------------------------------------------------------------ estado */
/* fim da RAM do N64 (4 MB); o port pode mudar antes de rodar */
#ifndef GX_RAM_END
#define GX_RAM_END 0x80400000u
#endif
/* fim da memoria valida para framebuffer/z-buffer. Padrao GX_RAM_END; o port ajusta para o fim do
 * executavel, porque o z-buffer e outros buffers do jogo ficam no BSS, acima de 0x80400000 */
extern uint32_t gx_ram_end;
#define GX_VTX_MAX 64
#define GX_MV_STACK 20
#define GX_DL_STACK 18

typedef struct {
    /* --- RSP --- */
    uint32_t seg[16];
    float mv[GX_MV_STACK][16];
    int mvTop;
    float proj[16];
    float mp[16];         /* modelview * projection (vetor linha) */
    int mpDirty;
    uint32_t geom;
    float texScaleS, texScaleT;  /* 0..1 */
    int texOn, texTile, texLevel;
    int numLights;
    struct { uint8_t col[3], colc[3]; int8_t dir[3]; } light[10];   /* 1-based */
    struct { int8_t dir[3]; } lookat[2];
    float lightDirModel[8][3];
    int lightsDirty;
    int16_t fogMul, fogOff;
    float vpScale[4], vpTrans[4];
    float vpRawS[2], vpRawT[2]; /* viewport x,y como o jogo mandou (vpScale/vpTrans = estes * rs) */
    GxVtx vtx[GX_VTX_MAX];
    uint32_t rdphalf1, rdphalf2;

    /* --- RDP --- */
    uint32_t omL, omH;
    uint32_t combW0, combW1;
    uint8_t prim[4], env[4], fogc[4], blendc[4];
    uint8_t primLodFrac, primLodMin;
    uint32_t fillColor;
    uint16_t primZ, primDZ;
    uint32_t scissorX0, scissorY0, scissorX1, scissorY1; /* pixels */
    GxTile tile[8];
    uint8_t tmem[4096];
    struct { uint8_t fmt, siz; uint16_t width; const uint8_t *addr; } timg;
    struct { uint8_t fmt, siz; uint16_t width; uint8_t *addr; } cimg;
    uint8_t *zimg;        /* z-buffer: u16 com os bytes na ordem do N64 (alto, baixo) */
    /* resolucao maior (gfx_hires.c): a tela do jogo (320x240) pode ir para um buffer nosso maior; cimg/zimg
     * acima sao o alvo de verdade, estes sao o que o jogo pediu */
    uint8_t *cimgRaw, *zimgRaw;
    uint16_t cimgRawW;
    float rs;             /* escala do alvo atual (1 = 320x240 do jogo) */
    int hrActive;         /* o alvo atual e um buffer da resolucao maior */
    int cle;              /* a cor do alvo atual esta na ordem do processador (imagem maior, opcao cor_direta), nao na do N64 */
    int hrW, hrH;         /* tamanho dele */
    uint32_t scRaw[4];    /* tesoura como o jogo mandou (1/4 de pixel): x0 y0 x1 y1 */
    int texDirty;
    GxTex *rtex[2];       /* texturas decodificadas das tiles de renderizacao */
    int maxYc, maxYz;     /* ultima linha que cabe na RAM (cor / cor+z): protege contra buffers errados */
    uint32_t tmemGen;     /* muda a cada carga de TMEM / mudanca de tile */

    /* --- estatisticas --- */
    uint32_t nCmd, nTri, nTriDrawn, nRect, nTexLoad, nBad, nCull, nClipRej, nVtx;
    int shownV, shownM, shownT, shownP; /* contadores internos do --trace */
    int traceArmed;       /* o --trace ja foi ligado (decodifica a camera mesmo depois de gastar traceN) */
    int traceSeg;         /* --trace-seg: so acompanha triangulos deste segmento (-1 = todos) */
    int traceN;           /* diagnostico: imprime os primeiros N triangulos rejeitados por estarem fora da tela */
    int cullFlip;         /* diagnostico: inverte o sentido da frente */
    int vpFlip;           /* diagnostico: NAO inverte o Y da viewport (o RSP inverte) */
    uint32_t opHist[256];
    int ucodeSwitch;

    /* --- sonda de pixel --- */
    int nProbe;
    GxProbe probe[GX_PROBE_MAX];
    char drawKind;        /* o que esta desenhando agora ('T'/'R') */
    uint32_t taskNo;      /* contador de tarefas de gfx */
    uint32_t curCmd;      /* endereco do comando atual */
    uint32_t timgRaw;     /* endereco bruto do ultimo SETTIMG */
    uint32_t tmemSrc[512];/* por palavra de TMEM: endereco do SETTIMG que a carregou */
    const uint8_t *tmemPtr[512]; /* por palavra de TMEM: de onde (na memoria) ela foi lida (texturas novas) */
    uint16_t tmemStride[512];     /* 0 = LOADBLOCK (linear); senao LOADTILE: bytes entre linhas na memoria */

    /* --- contagem por segmento (indice 0..15 = segmento; 16 = endereco direto) --- */
    int curSeg;           /* segmento do ultimo G_VTX */
    uint32_t segVtx[17], segDl[17], segTri[17], segDrawn[17], segClip[17], segCull[17];
    uint32_t segPx[17], segZRej[17];  /* pixels escritos / reprovados no z por segmento */
    int segX0[17], segY0[17], segX1[17], segY1[17]; /* retangulo coberto na tela */
    uint32_t segARej[17];  /* reprovados no teste de alfa/cobertura */
    uint32_t segVisit[17]; /* pixels que chegaram ao pipeline */
    struct { uint32_t comb0, comb1, omL, omH, geom; int a, shadeA, why; } segARej1[17]; /* estado do 1o reprovado */
} GxState;

extern GxState gx;

/* captura: marca as paginas de 4 KB lidas pela display list (so quando gx_capturing) */
extern int gx_capturing;
void gx_touch_slow(const void *p, size_t n);
static inline void gx_touch(const void *p, size_t n) {
    if (gx_capturing) {
        gx_touch_slow(p, n);
    }
}

/* ------------------------------------------------------------ funcoes */
/* gfx_rsp.c */
void gx_reset_frame(void);
int gx_run_dl(uint32_t addr, volatile int *yield);
const void *gx_resolve(uint32_t addr);
int gx_render(uint32_t dlAddr, volatile int *yield);
void gx_log(const char *fmt, ...);

/* gfx_rdp.c */
void gx_rdp_init(void);
void gx_rdp_cmd(uint32_t op, uint32_t w0, uint32_t w1);
void gx_texrect(uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, int flip);
void gx_bg_rect(uint32_t w1, int copy);
void gx_fillrect(uint32_t w0, uint32_t w1);
/* gfx_hires.c: resolucao maior */
extern int gx_hr_max;  /* largura maxima (opcao resolucao): 320 = desligado */
void gx_hr_retarget(void);                        /* depois de SETCIMG/SETZIMG */
void gx_hr_scissor(void);                         /* tesoura efetiva a partir de scRaw */
void gx_hr_viewport(void);                        /* viewport efetivo a partir de vpRaw */
void gx_hr_reading(const void *p);                /* uma carga de textura vai ler p (pode ser a tela) */
void gx_hr_pass_begin(void);                      /* antes de cada desenho: escolhe o tamanho */
void gx_hr_pass_end(double ms);                   /* depois: copia para a tela do N64, mede */
void gx_hr_screen(const void *fbRaw);             /* este framebuffer do jogo e uma tela (vai para o VI) */
int gx_hr_image(const void *fbRaw, const uint16_t **img, int *w, int *h); /* a imagem maior de fbRaw */
void gx_hr_print_stats(void);                     /* log: quanto tempo em cada tamanho */
void gx_hr_text(int begin, int x0, int y0, int x1, int y1); /* caixa de texto nitida (marcas do jogo) */
int gx_hr_text_of(const void *fbRaw, const uint8_t **buf, int rect[4]); /* a regiao em 640x480 da tela fbRaw */
extern int gx_hr_texto;                           /* opcao texto_nitido */
void gx_hr_tag(uint32_t tag, uint32_t w0);        /* marcas 0x54585434..39: fundo da pausa, Link do menu em 2x */
int gx_hr_skip_rect(void);                        /* 1 = retangulos ignorados (fundo da pausa trocado) */
struct GxTex_ *gx_hr_prv_lookup(struct GxTex_ *tx, const GxTile *tl, const uint8_t *src, unsigned stride);
extern int gx_tmem_note;                          /* 1 = anota a origem das cargas da TMEM mesmo sem o pacote */
#define GX_TAG_TXT_A 0x54585430u /* G_NOOP w1: comeco da caixa de texto; w0 = x0 << 12 | y0 */
#define GX_TAG_TXT_B 0x54585431u /*          w0 = x1 << 12 | y1 (depois da A) */
#define GX_TAG_TXT_END 0x54585432u
#define GX_TAG_GE 0x54585433u      /* G_NOOP w1: daqui ao fim do quadro, 2D que pode ir para o acelerador (gfx_ge.c) */

/* ---- acelerador 2D (gfx_ge.c) ---- */
#define GX_GE_PARTS 8
typedef struct {
    uint32_t off;            /* texels (ARGB, cor * alfa) na memoria da parte, com borda de 1 */
    uint16_t pw, ph, sw, sh; /* tamanho com a borda; o desenho dentro, em (1,1) */
    int16_t dx, dy, dw, dh;  /* onde, na tela de 640x480 */
} GxGeItem;
extern int gx_ge_on;
extern uint8_t *(*gx_ge_mem)(int part, uint32_t *cap);
void gx_ge_pass_begin(void);
int gx_ge_scan_begin(void);
void gx_ge_scan_end(int abort);
int gx_ge_scanning(void);
void gx_ge_text(int on, int x0, int y0, int x1, int y1); /* (varredura) caixa de texto, pixels de 320x240 */
int gx_ge_group(int on);                          /* desenho: 1 = a caixa de texto vai pela camada do acelerador */
int gx_ge_other(int x0, int y0, int x1, int y1);
int gx_ge_tri(const GxVtx *a, const GxVtx *b, const GxVtx *c);
int gx_ge_texrect(uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, int flip);
void gx_ge_attach(const void *key);
int gx_ge_items(const void *key, const GxGeItem **items, int *part);
const uint8_t *gx_ge_part_mem(int part, uint32_t *lo, uint32_t *hi);
void gx_ge_print_stats(void);
int gx_bake_mode(void);
uint32_t gx_bake_texel(float S, float T, int mode, int hk, const int *shade);
int gx_bake_hdk(void);
int gx_bake_uses(int t);
extern int gx_hr_alvo;                            /* ms por desenho que a resolucao dinamica tenta nao passar */
extern int gx_hr_fixo;                            /* 1 = sempre o tamanho maximo (teste no PC) */
int gx_img_size(const void *img, int *w, int *h); /* gfx_hook.c: tamanho de uma imagem intermediaria */
void gx_tex_resolve(void);
GxTex *gx_tex_get(int tileIndex);
void gx_tex_flush(void);

/* gfx_tri.c */
void gx_draw_tri(const GxVtx *a, const GxVtx *b, const GxVtx *c);
/* spans: referencia (pixel a pixel) e caminho rapido (gfx_fast.inc) */
extern int gx_fast;
void gx_span_ref(const GxPrim *p, int y, int xs, int xe);
/* caminho rapido: as primitivas vao para uma fila desenhada em gx_threads threads (1 ou 2) */
extern int gx_threads;
extern int gx_fast_exact; /* 1 = caminho rapido identico a referencia (testes) */
extern int gx_fused;      /* 1 = caminho fundido de 8 pixels quando o estado permite */
extern int gx_tex_prefilter; /* filtro de textura pre-calculado (aproximado; opcoes.txt filtro_rapido): 1 = 2x2, 2 = 4x4 */
extern int gx_sort_opaque;   /* 1 = opacos do mais perto para o mais longe (opcoes.txt ordenar_opacos) */
extern int gx_kern;       /* 1 = combinador em kernels prontos (formas mais comuns) */
extern int gx_bgdefer;    /* 1 = fundo (ceu) desenhado depois, so onde nada cobriu */
extern int gx_stream;     /* 1 = a thread auxiliar desenha enquanto a fila e montada */
void gx_batch_flush(void);                       /* desenha o que esta na fila */
/* estatistica por desenho (registro_quadros): texturas decodificadas, copias com borda, filtros
 * pre-calculados, esvaziamentos da fila no meio do quadro; e o tempo das 3 primeiras (ns) */
extern unsigned gx_stN[4];
extern uint64_t gx_stNs[3];
uint64_t gx_now_ns(void);
void gx_batch_reading(const void *p, size_t n);  /* algo vai ler a RAM: desenha antes se precisar */
int gx_fill_fast(int x0, int y0, int x1, int y1); /* preenchimento na fila (0 = nao serve) */
void gx_prim_const(GxPrim *p, const int *shade);
void gx_draw_rect_prim(GxPrim *p, int x0, int y0, int x1, int y1);
/* pipeline por pixel (combinador + blender + z + escrita) */
void gx_pipe_begin(void);
/* shade: r,g,b,a 0..255; S,T em texels*32 (antes de shift/subtracao de uls) */
void gx_pipe_px(int x, int y, int z18, int useZ, int zTol, const int *shade, float S, float T);

/* Profundidade como no RDP: z de 18 bits (0..0x3FFFF) guardado no z-buffer em 14 bits "flutuantes"
 * (3 de expoente, 11 de mantissa), com muito mais precisao perto do fundo da cena. Com 14 bits
 * lineares, coisas distantes quase coplanares (estrada sobre a grama do Hyrule Field) brigavam. */
#define GX_Z18_SCALE 261888.0f /* z de tela (0..1023) * 256 */
static inline uint32_t gx_zcomp(uint32_t z) {
    uint32_t e = (uint32_t)__builtin_clz(~(z << 14));
    if (e > 7) {
        e = 7;
    }
    return (e << 11) | ((z >> (e < 6 ? 6 - e : 0)) & 0x7FFu);
}
static inline uint32_t gx_zdecomp(uint32_t c) {
    static const uint32_t add[8] = { 0, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800 };
    static const uint8_t sh[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    uint32_t e = (c >> 11) & 7u;
    return add[e] + ((c & 0x7FFu) << sh[e]);
}
void gx_copy_px(int x, int y, float S, float T);
void gx_fill_px(int x, int y);
/* amostra uma tile (0..7) com a textura ja decodificada; saida RGBA 0..255 */
void gx_tex_sample(int tileIndex, const GxTex *tx, float S, float T, int bilerp, int *out);

/* sonda de pixel (gfx_tri.c) */
GxProbeSlot *gx_probe_slot(int x, int y);
void gx_probe_print(const void *fb);
/* contagem por segmento da ultima tarefa (gfx_rsp.c) */
void gx_seg_print(void);

/* gfx_capture.c */
void gx_cap_save(GxCapState *c);
void gx_cap_load(const GxCapState *c);
/* renderiza a tarefa como gx_render, gravando antes um arquivo de captura (0 = ok) */
int gx_capture_render(uint32_t dlAddr, const char *path);

/* gfx_dump.c */
int gx_dump_ppm(const uint8_t *fb5551be, int w, int h, const char *path);
int gx_dump_png(const uint8_t *fb5551be, int w, int h, const char *path);

/* gfx_hook.c */
extern void (*gx_debug_hook)(const char *msg);
/* quadros intermediarios (gfx_rsp.c): troca de matriz durante um desenho */
extern const float *(*gx_mtx_lookup)(const void *mtx);
extern const void *(*gx_vtx_lookup)(const void *src, unsigned bytes);
extern int (*gx_tile_lookup)(const void *cmd, uint32_t *w0, uint32_t *w1);
extern const uint32_t *(*gx_cmd_lookup)(const void *cmd);
/* quadros intermediarios (gfx_hook.c -> tela): imagens para mostrar antes/no lugar do quadro 'fb' do jogo.
 * img[i] (320x240 RGBA5551 do N64) entra na tela vs[i] vsyncs depois do jogo trocar para 'fb'. */
extern void (*gx_interp_sink)(const void *fb, const uint16_t *const *img, const int *vs, int n);

/* gfx_rdp.c: brilho (gamma) nas cores de entrada (opcao brightness) */
extern int gx_brilho, gx_gamma_on;
extern uint8_t gx_gamma[256];
extern float gx_gammaf[256];
void gx_gamma_update(void);

#endif
