/*
 * gfx_fast.inc -- caminho rapido dos spans (incluido no fim de gfx_tri.c, que tem o estado do pipeline).
 *
 * Faz EXATAMENTE as mesmas contas da referencia (gx_span_ref + gx_pipe_px + gx_tex_sample), mas em
 * ETAPAS sobre listas de pixels, em blocos de ate FB pixels de um span:
 *   1. profundidade: calcula z, testa e monta a lista dos pixels que passaram (sem desvios)
 *   2. sombreamento: so os canais que o combinador/blender usam
 *   3. coordenadas de textura (uma divisao por pixel) e amostragem: uma funcao especializada por
 *      modo de borda/filtro, escolhida uma vez por estado
 *   4. combinador: cada canal e um laco curto sobre vetores (A-B)*C+D; constantes viram vetores
 *      preenchidos uma vez por span; canal trivial (A==B ou C==0) e so um ponteiro para D
 *   5. teste de alfa, blender e escrita
 * As decisoes ficam fora dos lacos (por estado ou por span), e os lacos de inteiros sao simples o
 * bastante para o compilador usar NEON (Miyoo) / SSE (PC). As contas com float sao escalares e
 * identicas as da referencia, entao o resultado e o mesmo bit a bit em qualquer maquina.
 * Os testes comparam os dois caminhos (tests/gfx_test.c e oot_replay --compare).
 *
 * O que nao esta aqui fica com a referencia (fast_ok devolve 0): sondas de pixel ligadas e
 * framebuffers que nao sao de 16 bits. O modo copia e o preenchimento fora da fila desenham na hora.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("tree-vectorize")
#endif

#define FB 64 /* pixels por bloco */

/* 1 = so contas identicas a referencia (para os testes); 0 = aproximacoes invisiveis e mais rapidas */
int gx_fast_exact = 0;
extern int gx_hd_use; /* gfx_hd.c (opcao texturas_novas) */
/* texturas novas (2x): 0 = le o texel mais proximo da nova (ela ja e 2x mais detalhada: suave como o filtro
 * rapido da original, com a memoria e o custo de uma textura sem filtro); 1 = filtra a nova tambem */
int gx_hd_filtro = 0;
/* com gx_hd_filtro = 0: filtro leve so nos triangulos em que um texel da nova ocupa pelo menos isto de pixels
 * (perto da camera); 0 = nunca */
int gx_hd_perto = 1;
/* opcao filtro_exato_perto: com o filtro pre-calculado (celulas de 1/2 texel), os triangulos em que um texel ocupa
 * N pixels ou mais na tela (perto da camera: as celulas viram quadradinhos) usam o filtro exato do N64 (3 pontos,
 * fracao de 1/32). Sem memoria a mais; custa so nesses triangulos. 0 = nunca */
int gx_filtro_exato_perto = 6;
int gx_filtro_perto_fino = 0; /* opcao filtro_perto_fino: 1 = de perto a tabela 4x4 (rapida); 0 = o filtro exato (mais lento) */
/* 1 = usa o caminho fundido (gx_span_fused) quando o estado permite */
int gx_fused = 1;
/* 1 = uma funcao de trecho por forma do blender (0 = a funcao unica de antes, para comparar) */
int gx_spanspec = 1;
/* 1 = trecho de um desenho com neblina em que a neblina e zero do comeco ao fim (o alfa do sombreamento, que leva a
 * neblina, e 0 nos dois pontas e varia em linha reta) vai pelo blender sem neblina: (neblina * 0 + cor * 255) / 255 e
 * exatamente a cor, entao a imagem e a mesma, sem as contas da mistura. A maior parte do que esta perto da camera. */
int gx_fog_skip = 1;
/* 1 = mistura do combinador com multiplicador constante 255 vira so o 1o termo (ver fast_build; no maximo 1/255) */
int gx_cc_aprox = 1;
/* 1 = multiplicador constante 0 do combinador vira o termo d (exato; ver fast_build) */
int gx_cc_fold = 1;
/* 2o ciclo que vira COMBINED direto pela dobra (lote 118 proibiu tudo por causa das asas da Navi; a causa foi corrigida no
 * lote 120): 0 = nunca dobra (lote 118), 1 = dobra a cor e nao o alfa, 2 = dobra tudo (como no lote 115). Em teste. */
int gx_dobra_comb = 0;
/* 1 = nao desenha os pixels de efeitos/texturas transparentes que caem na parte vazia da textura (ver crop) */
int gx_corte = 1;
/* 1 = textura ampliada com filtro: amostra os pixels pares e faz a media nos impares (ver tex_meia) */
int gx_tex_meia = 0; /* (lote 116: medido no aparelho, 1-3% mais lento; replay --com-meia) */
/* 1 = pixels de alfa 0 das misturas transparentes nao sao misturados nem gravados (ficam iguais; ver a0skip) */
int gx_a0_skip = 0; /* (lote 113: medido no aparelho, sem ganho; e o bit de cobertura do pixel nao era regravado) */
/* filtro de textura pre-calculado (tex_prefilter; aproximado): 0 = exato, 1 = 2x2 celulas por texel,
 * 2 = 4x4 celulas por texel */
int gx_tex_prefilter = 0;
/* 1 = pede a memoria antes de usar (pld): a proxima linha de cada triangulo e o proximo bloco do trecho
 * (ideia do port do Mario Kart 64: o Miyoo espera muito pela memoria). Nao muda a imagem. */
int gx_pld = 0; /* medido no aparelho: nao ajudou (o desenho nao espera a memoria da tela) */

typedef struct {
    const GxTex *tx;
    const uint32_t *px;
    float scaleS, scaleT, offS, offT;
    int modeS, modeT;     /* 0 repete (mascara), 1 espelha (mascara), 2 prende (e mascara), 3 generico */
    int maskS, maskT;     /* (1 << mascara) - 1 */
    int bitS, bitT;       /* mascara (bit do espelho) */
    int cms, cmt;
    int w, h, clipw, cliph;
    /* copia com borda (caminho fundido): ver tex_pad */
    const uint32_t *pp;
    int pw, pmS, pmT, pcS, pcT;
    /* filtro pre-calculado (opcao filtro_rapido): ver tex_prefilter */
    const uint32_t *pf;
    int pfw, pfs; /* largura em celulas; celulas por texel = 1 << pfs */
    int hdPoint;  /* textura nova lida pelo texel mais proximo (opcao filtro_novas = 0) */
} FastTex;

typedef void (*SampFn)(const FastTex *u, const float *S, const float *T, uint32_t *out, int n);
typedef void (*SampNFn)(const FastTex *u, const float *S, const float *T, uint32_t *out, int ng); /* ng grupos de 8 pixels */

typedef struct {
    uint32_t gen;               /* geracao do estado (gx_pipe_begin) para a qual foi montado */
    int cyc2;
    uint8_t c0[4][4], c1[4][4]; /* indices (como na referencia) para r,g,b,a de cada ciclo */
    uint8_t k0[4], k1[4];       /* 1 = canal trivial (A==B ou C==0): resultado e so D */
    int useT[2], bilerp;
    int zcmp, zupd, zdec;
    int cvgXAlpha, ac, alphaCvgSel, forceBl;
    int cdith; /* pontilhado da cor (gfx_tri.c, gx_dith) */
    int bl[2][4];
    int needMem;
    int dead;                   /* alfa constante reprovado no teste: a primitiva nao desenha nada */
    uint32_t needV;             /* bit i = o combinador le V[i] */
    int needShade[4];
    uint8_t need0[4], need1[4]; /* canais de cada ciclo do combinador que alguem usa */
    int needTC[2][4];           /* canais de cada textura que o combinador le */
    int nConst;                 /* constantes do combinador usadas (indice em V, valor) */
    uint8_t cIdx[16];
    int32_t cVal[16];
    int blUsesA[2];             /* o ciclo do blender le o alfa (A) */
    FastTex t[2];
    SampFn samp[2];
    SampNFn sampn[2];
    /* textura nova lida pelo texel mais proximo (hdPoint): versao com filtro leve para triangulos em que ela
     * aparece ampliada (perto da camera); NULL = nenhuma. Ver hd_close. */
    FastTex tF[2];
    SampNFn sampnF[2];
    uint8_t fExact[2];          /* tF/sampnF e o filtro exato da propria textura (opcao filtro_exato_perto), nao a nova */
    int fused;                  /* 1 = desenhado por gx_span_fused (8 pixels por vez, tudo em registradores) */
    /* preparado por fused_check para o caminho fundido */
    uint8_t fops[8][6];         /* operacoes do combinador: tipo, destino, a, b, c, d */
    int nfops;
    uint8_t fcc[4];             /* indice (em R) do resultado de cada canal do combinador */
    uint8_t fshk[4];            /* canais do sombreamento usados */
    int fnsh;
    uint32_t fzero;             /* entradas de R que ficam zeradas */
    int fblm;                   /* forma do blender (BLM_*) */
    /* 1 = a ultima mistura e (cor * a + memoria * (1 - a)) com o alfa da entrada, sem pontilhado nem gravar z: onde o
     * alfa e 0 o pixel fica exatamente como estava, entao ele sai da mascara (sem misturar nem gravar). As partes
     * vazias das texturas transparentes (fumaca, fogo, halos, brilhos) */
    int a0skip;
    /* recorte dos transparentes (lote 125): pixels cuja coordenada de textura cai fora do retangulo dos texels
     * visiveis (com folga) nao mudam a tela; o trecho e encurtado para a parte de dentro. Limites em 1/32 de texel
     * (a mesma escala do amostrador: S * escala - deslocamento) */
    int crop;
    float cropS0, cropS1, cropT0, cropT1, cropW, cropH;
    /* combinador com forma conhecida (KERN_*): as contas de cor saem direto em registradores, sem o
     * interpretador de operacoes; o alfa (se houver) continua pelo interpretador (fopsA) */
    int kern;
    uint8_t ka0, kb0, kc0, kd0, ks0, kc1, ks1, ka1, kb1, kd1;
    uint8_t kcls[8];            /* classe de cada entrada (a0, b0, c0, d0, c1, a1, b1, d1): KS_* */
    uint8_t kAcls, kAidx;       /* alfa do combinador sem o interpretador: KA_* (copia de T0.a ou constante) */
    uint8_t kAi[3];             /* constantes dos kernels de alfa (indices em R) */
    uint8_t fopsA[4][6];
    int nfopsA;
    /* copia do estado lido durante o desenho (a fila de primitivas roda depois, em outra thread) */
    uint8_t prim[4], env[4], blendc[4], fogc[4], primLodFrac;
    uint8_t *cimg, *zimg;
    int cle; /* cor do alvo na ordem do processador (gx.cle) */
    int cwidth;
} FastProg;

/* memoria de trabalho de uma thread de desenho. Todo valor de canal (textura, sombreamento,
 * combinador, blender) fica entre 0 e 255: os vetores por pixel sao de bytes */
typedef struct {
    uint8_t v[4][FB];
} Chan4;
typedef struct {
    uint8_t cV[V_N][FB]; /* constantes do combinador */
    Chan4 cBl[2];        /* [0] cor do blend, [1] cor da neblina (alfa da neblina em v[3]) */
    uint8_t c255[FB];
    /* valor + 1 que cada vetor constante tem agora (0 = nada): so preenche de novo quando muda */
    int32_t cHave[V_N];
    int32_t blHave[2][4];
    int c255Have;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* caminho fundido: entradas do combinador e constantes do blender (valem para rProg/rGen) */
    uint8x8_t R[V_N + 4];
    uint8x8_t fogc[4], blc[4];
    uint8x8_t kk[8][3]; /* entradas constantes dos kernels do combinador; kA = alfa (copia simples) */
    uint8x8_t kA, kAv[3];
    const void *rProg;
    uint32_t rGen;
#endif
    int covW, covR; /* fundo adiado: marcar os pixels escritos / desenhar so onde nada escreveu */
    int hdF;        /* trabalho com a textura nova ampliada: usa a versao com filtro (tF/sampnF) */
    const uint8_t *own; /* dono de cada par de linhas (NULL = g_own); muda quando a principal pega linhas */
} FastCtx;

static FastProg g_fp;

/* Fundo adiado (ceu, cor de fundo): ver bg_pre. Uma mascara de bytes por pixel da tela: 0xFF = algum
 * desenho posterior ja escreveu ali. Linhas com folga de 16 bytes (os grupos de 8 passam do fim). */
#define COV_W 656 /* ate 640 (resolucao maior) + 16 */
#define COV_H 480
static uint8_t g_cov[COV_H][COV_W] __attribute__((aligned(16)));

static float shift_scale(int sh) {
    if (sh == 0) {
        return 1.0f;
    }
    if (sh <= 10) {
        return 1.0f / (float)(1 << sh);
    }
    return (float)(1 << (16 - sh));
}

/* ------------------------------------------------------------------ textura (mesmas contas de gx_tex_sample) */
static inline __attribute__((always_inline)) int fast_axis(int i, const int mode, int mask, int bit, int cm, int clip,
                                                           int size) {
    if (mode == 0) {
        return i & mask;
    }
    if (mode == 1) {
        return (i ^ -((i >> bit) & 1)) & mask;
    }
    if (mode == 2) {
        /* prende em [0, clip-1]. O modo 2 so e usado sem mascara ou com largura == 1 << mascara, e
         * clip <= largura: depois de preso o indice ja cabe na mascara e o bit do espelho e 0 */
        if (i < 0) {
            i = 0;
        }
        if (i > clip - 1) {
            i = clip - 1;
        }
        return i;
    }
    {
        int v = wrap_coord(i, cm, bit, clip);
        return v >= size ? size - 1 : v;
    }
}

/* coordenada inteira de textura de n pixels: gx_floori(v * escala - deslocamento), 4 por vez no NEON.
 * O NEON trata subnormais como zero; gx_floori faz o mesmo, entao o resultado e identico ao escalar. */
static void tex_coord(const float *restrict v, float scale, float off, int32_t *restrict out, int n) {
    int j = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const float32x4_t vs = vdupq_n_f32(scale), vo = vdupq_n_f32(off);
    for (; j + 4 <= n; j += 4) {
        float32x4_t x = vsubq_f32(vmulq_f32(vld1q_f32(v + j), vs), vo);
        int32x4_t i = vcvtq_s32_f32(x);
        uint32x4_t gt = vcgtq_f32(vcvtq_f32_s32(i), x); /* trunc > x: x negativo nao inteiro */
        vst1q_s32(out + j, vaddq_s32(i, vreinterpretq_s32_u32(gt)));
    }
#endif
    for (; j < n; j++) {
        out[j] = gx_floori(v[j] * scale - off);
    }
}

static inline __attribute__((always_inline)) void samp_tpl(const FastTex *u, const float *restrict S,
                                                           const float *restrict T, uint32_t *restrict out, int n,
                                                           const int mS, const int mT, const int bil) {
    /* tudo em variaveis locais: sem isso o compilador rele a estrutura a cada pixel (-fno-strict-aliasing) */
    const uint32_t *restrict px = u->px;
    const int w = u->w, h = u->h, clipw = u->clipw, cliph = u->cliph;
    const int maskS = u->maskS, maskT = u->maskT, bitS = u->bitS, bitT = u->bitT, cms = u->cms, cmt = u->cmt;
    const uint32_t m = 0x00FF00FFu, rnd = 0x00100010u;
    int32_t sI[FB], tI[FB];
    int j;
    tex_coord(S, u->scaleS, u->offS, sI, n);
    tex_coord(T, u->scaleT, u->offT, tI, n);
    for (j = 0; j < n; j++) {
        int si = sI[j], ti = tI[j];
        int s0 = si >> 5, t0 = ti >> 5;
        int a0 = fast_axis(s0, mS, maskS, bitS, cms, clipw, w);
        int b0 = fast_axis(t0, mT, maskT, bitT, cmt, cliph, h);
        if (!bil) {
            out[j] = px[b0 * w + a0];
        } else {
            int a1 = fast_axis(s0 + 1, mS, maskS, bitS, cms, clipw, w);
            int b1 = fast_axis(t0 + 1, mT, maskT, bitT, cmt, cliph, h);
            uint32_t fs = (uint32_t)si & 31u, ft = (uint32_t)ti & 31u;
            uint32_t ca, cb, cc, wA, wB, wC, e, o;
            const uint32_t *r0 = px + b0 * w, *r1 = px + b1 * w;
            /* triangulo de baixo: c00, c10, c01; de cima: c11, c01, c10 (como a referencia). Sem desvio:
             * escolhe enderecos e pesos com selecao (o desvio e imprevisivel e caro no Cortex-A7) */
            int up = fs + ft >= 32u;
            const uint32_t *pa = up ? r1 + a1 : r0 + a0;
            const uint32_t *pb = up ? r1 + a0 : r0 + a1;
            const uint32_t *pc_ = up ? r0 + a1 : r1 + a0;
            wA = up ? fs + ft - 32u : 32u - fs - ft;
            wB = up ? 32u - fs : fs;
            wC = up ? 32u - ft : ft;
            ca = *pa;
            cb = *pb;
            cc = *pc_;
            e = (ca & m) * wA + (cb & m) * wB + (cc & m) * wC + rnd;
            o = ((ca >> 8) & m) * wA + ((cb >> 8) & m) * wB + ((cc >> 8) & m) * wC + rnd;
            out[j] = ((e >> 5) & m) | (((o >> 5) & m) << 8);
        }
    }
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* ---- versao NEON (modos 0, 1 e 2): enderecos, pesos e filtro 4 pixels por vez; so as leituras dos
 * texels ficam escalares. Mesmas contas de samp_tpl (inteiras), entao o resultado e identico. */
static inline __attribute__((always_inline)) int32x4_t nax(int32x4_t i, const int mode, int32x4_t vmask,
                                                         int32x4_t vnbit, int32x4_t vclip1) {
    if (mode == 0) {
        return vandq_s32(i, vmask);
    }
    if (mode == 1) {
        int32x4_t b = vandq_s32(vshlq_s32(i, vnbit), vdupq_n_s32(1)); /* (i >> bit) & 1 */
        return vandq_s32(veorq_s32(i, vnegq_s32(b)), vmask);
    }
    return vminq_s32(vmaxq_s32(i, vdupq_n_s32(0)), vclip1);
}

static inline __attribute__((always_inline)) int32x4_t nfloor(float32x4_t x) {
    int32x4_t i = vcvtq_s32_f32(x);
    uint32x4_t gt = vcgtq_f32(vcvtq_f32_s32(i), x);
    return vaddq_s32(i, vreinterpretq_s32_u32(gt));
}

static inline __attribute__((always_inline)) void samp_neon(const FastTex *u, const float *restrict S,
                                                            const float *restrict T, uint32_t *restrict out, int n,
                                                            const int mS, const int mT, const int bil) {
    const uint32_t *restrict px = u->px;
    const float32x4_t vsS = vdupq_n_f32(u->scaleS), voS = vdupq_n_f32(u->offS);
    const float32x4_t vsT = vdupq_n_f32(u->scaleT), voT = vdupq_n_f32(u->offT);
    const int32x4_t mskS = vdupq_n_s32(u->maskS), mskT = vdupq_n_s32(u->maskT);
    const int32x4_t nbS = vdupq_n_s32(-u->bitS), nbT = vdupq_n_s32(-u->bitT);
    const int32x4_t clS = vdupq_n_s32(u->clipw - 1), clT = vdupq_n_s32(u->cliph - 1);
    const int32x4_t one = vdupq_n_s32(1), v31 = vdupq_n_s32(31), v32 = vdupq_n_s32(32);
    const int w = u->w;
    const int n4 = (n + 3) & ~3;
    int32_t iA[FB], iB[FB], iC[FB];
    uint32_t wA[FB], wB[FB], wC[FB], ca[FB], cb[FB], cc[FB];
    float Sp[FB], Tp[FB];
    const float *restrict Sv = S, *restrict Tv = T;
    int j;
    if (n4 != n) {
        /* as pistas que sobram no ultimo grupo de 4 viram coordenada 0 (indice valido, resultado descartado) */
        memcpy(Sp, S, (size_t)n * sizeof *S);
        memcpy(Tp, T, (size_t)n * sizeof *T);
        for (j = n; j < n4; j++) {
            Sp[j] = 0.0f;
            Tp[j] = 0.0f;
        }
        Sv = Sp;
        Tv = Tp;
    }
    for (j = 0; j < n4; j += 4) {
        int32x4_t si = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(Sv + j), vsS), voS));
        int32x4_t ti = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(Tv + j), vsT), voT));
        int32x4_t s0 = vshrq_n_s32(si, 5), t0 = vshrq_n_s32(ti, 5);
        int32x4_t a0 = nax(s0, mS, mskS, nbS, clS);
        int32x4_t b0 = nax(t0, mT, mskT, nbT, clT);
        int32x4_t r0 = vmulq_n_s32(b0, w);
        if (!bil) {
            vst1q_s32(iA + j, vaddq_s32(r0, a0));
        } else {
            int32x4_t a1 = nax(vaddq_s32(s0, one), mS, mskS, nbS, clS);
            int32x4_t b1 = nax(vaddq_s32(t0, one), mT, mskT, nbT, clT);
            int32x4_t r1 = vmulq_n_s32(b1, w);
            int32x4_t fs = vandq_s32(si, v31), ft = vandq_s32(ti, v31);
            int32x4_t sum = vaddq_s32(fs, ft);
            uint32x4_t up = vcgeq_s32(sum, v32);
            /* triangulo de baixo: c00, c10, c01; de cima: c11, c01, c10 */
            vst1q_s32(iA + j, vbslq_s32(up, vaddq_s32(r1, a1), vaddq_s32(r0, a0)));
            vst1q_s32(iB + j, vbslq_s32(up, vaddq_s32(r1, a0), vaddq_s32(r0, a1)));
            vst1q_s32(iC + j, vbslq_s32(up, vaddq_s32(r0, a1), vaddq_s32(r1, a0)));
            /* pesos repetidos nos 4 bytes (um por canal) */
            vst1q_u32(wA + j, vmulq_n_u32(vreinterpretq_u32_s32(vbslq_s32(up, vsubq_s32(sum, v32), vsubq_s32(v32, sum))),
                                          0x01010101u));
            vst1q_u32(wB + j, vmulq_n_u32(vreinterpretq_u32_s32(vbslq_s32(up, vsubq_s32(v32, fs), fs)), 0x01010101u));
            vst1q_u32(wC + j, vmulq_n_u32(vreinterpretq_u32_s32(vbslq_s32(up, vsubq_s32(v32, ft), ft)), 0x01010101u));
        }
    }
    if (!bil) {
        for (j = 0; j < n; j++) {
            out[j] = px[iA[j]];
        }
        return;
    }
    for (j = 0; j < n4; j++) {
        ca[j] = px[iA[j]];
        cb[j] = px[iB[j]];
        cc[j] = px[iC[j]];
    }
    {
        const uint16x8_t rnd = vdupq_n_u16(16);
        for (j = 0; j < n4; j += 4) {
            uint8x16_t A = vld1q_u8((const uint8_t *)(ca + j)), B = vld1q_u8((const uint8_t *)(cb + j));
            uint8x16_t C = vld1q_u8((const uint8_t *)(cc + j));
            uint8x16_t WA = vld1q_u8((const uint8_t *)(wA + j)), WB = vld1q_u8((const uint8_t *)(wB + j));
            uint8x16_t WC = vld1q_u8((const uint8_t *)(wC + j));
            uint16x8_t lo = vmull_u8(vget_low_u8(A), vget_low_u8(WA));
            uint16x8_t hi = vmull_u8(vget_high_u8(A), vget_high_u8(WA));
            lo = vmlal_u8(lo, vget_low_u8(B), vget_low_u8(WB));
            hi = vmlal_u8(hi, vget_high_u8(B), vget_high_u8(WB));
            lo = vmlal_u8(lo, vget_low_u8(C), vget_low_u8(WC));
            hi = vmlal_u8(hi, vget_high_u8(C), vget_high_u8(WC));
            lo = vaddq_u16(lo, rnd);
            hi = vaddq_u16(hi, rnd);
            vst1q_u8((uint8_t *)(out + j), vcombine_u8(vshrn_n_u16(lo, 5), vshrn_n_u16(hi, 5)));
        }
    }
}
#define SAMP_IMPL(ms, mt, bl)                                                                        \
    if ((ms) < 3 && (mt) < 3) {                                                                      \
        samp_neon(u, S, T, out, n, (ms) < 3 ? (ms) : 0, (mt) < 3 ? (mt) : 0, bl);                     \
    } else {                                                                                         \
        samp_tpl(u, S, T, out, n, ms, mt, bl);                                                       \
    }
#else
#define SAMP_IMPL(ms, mt, bl) samp_tpl(u, S, T, out, n, ms, mt, bl);
#endif

#define SAMP_FN(ms, mt, bl)                                                                          \
    static void samp_##ms##mt##bl(const FastTex *u, const float *S, const float *T, uint32_t *out, int n) { \
        SAMP_IMPL(ms, mt, bl)                                                                        \
    }
#define SAMP_ROW(ms, bl) SAMP_FN(ms, 0, bl) SAMP_FN(ms, 1, bl) SAMP_FN(ms, 2, bl) SAMP_FN(ms, 3, bl)
SAMP_ROW(0, 0) SAMP_ROW(1, 0) SAMP_ROW(2, 0) SAMP_ROW(3, 0)
SAMP_ROW(0, 1) SAMP_ROW(1, 1) SAMP_ROW(2, 1) SAMP_ROW(3, 1)
#define SAMP_REF(ms, mt, bl) samp_##ms##mt##bl
static const SampFn g_sampTab[2][4][4] = {
    { { SAMP_REF(0, 0, 0), SAMP_REF(0, 1, 0), SAMP_REF(0, 2, 0), SAMP_REF(0, 3, 0) },
      { SAMP_REF(1, 0, 0), SAMP_REF(1, 1, 0), SAMP_REF(1, 2, 0), SAMP_REF(1, 3, 0) },
      { SAMP_REF(2, 0, 0), SAMP_REF(2, 1, 0), SAMP_REF(2, 2, 0), SAMP_REF(2, 3, 0) },
      { SAMP_REF(3, 0, 0), SAMP_REF(3, 1, 0), SAMP_REF(3, 2, 0), SAMP_REF(3, 3, 0) } },
    { { SAMP_REF(0, 0, 1), SAMP_REF(0, 1, 1), SAMP_REF(0, 2, 1), SAMP_REF(0, 3, 1) },
      { SAMP_REF(1, 0, 1), SAMP_REF(1, 1, 1), SAMP_REF(1, 2, 1), SAMP_REF(1, 3, 1) },
      { SAMP_REF(2, 0, 1), SAMP_REF(2, 1, 1), SAMP_REF(2, 2, 1), SAMP_REF(2, 3, 1) },
      { SAMP_REF(3, 0, 1), SAMP_REF(3, 1, 1), SAMP_REF(3, 2, 1), SAMP_REF(3, 3, 1) } }
};

static void samp_null(const FastTex *u, const float *S, const float *T, uint32_t *out, int n) {
    (void)u;
    (void)S;
    (void)T;
    memset(out, 0, (size_t)n * sizeof *out);
}

/* ------------------------------------------------------------------ programa do pixel (por estado) */
static void fused_check(FastProg *f);
#ifdef GX_REPLAY_DIAG
static void diag_ops(const FastProg *f);
#endif

/* Ceu pre-misturado (lote 121): o ceu mistura 2 texturas por pixel, (T1 - T0) * k + T0 com k constante (a
 * mistura entre dois horarios do dia). Como o filtro e linear, misturar as duas texturas antes (texel a texel, a
 * mesma conta) e filtrar depois da o mesmo ponto a menos do arredondamento: o ceu le uma textura so. A mistura e
 * refeita quando k anda 3 ou mais (o ceu muda devagar; diferenca de no maximo 3/255 da distancia entre as duas,
 * menos de 1 degrau da tela). Opcao gx_ceu_mistura (replay --sem-mistura). */
int gx_ceu_mistura = 1;
/* Lote 183: a agua (lago Hylia, Dominio Zora) mistura 2 texturas que deslizam em direcoes e escalas diferentes,
 * (T1 - T0) * k + T0 com k constante. As duas repetem, entao a mistura tambem repete: ela e feita numa textura
 * (ate 64x64) numa grade fina o bastante para as duas (a escala da mais detalhada em cada eixo), com a 2a ja
 * deslocada, e o pixel le uma textura so (metade das leituras de textura). Quando as grades coincidem e o
 * deslocamento e inteiro o resultado e o mesmo; senao uma das camadas passa por um filtro a mais (um pouco mais
 * suave). Refeita quando o deslocamento muda (todo quadro, enquanto desliza). Opcao gx_mistura_desliza
 * (options.txt premix_scrolling: 1 = ate 64x64, a agua; 2 = ate 128x128, tambem o fundo do lago, que mistura uma
 * textura 4x maior; replay --sem-mistura-desliza / --mistura-desliza N). */
int gx_cc_alias = 1; /* (lote 190: 1o ciclo que so copia uma entrada lido direto pelo 2o; replay --sem-alias) */
int gx_mistura_desliza = 2; /* (padrao 2 desde o lote 188: medido no lago Hylia, 384 -> 416 com o mesmo tempo) */
typedef struct {
    int sizeS, sizeT;   /* textura misturada */
    int raS, raT;       /* passo da 1a textura por texel da grade, em 1/64 de texel (64 = mesma grade) */
    int rbS, rbT;       /* idem, 2a */
    int obS, obT;       /* posicao da 2a no texel 0 da grade, em 1/64 de texel (0 .. 64*w-1) */
    GxTile tile;        /* tile da textura misturada (no lugar da tile da 1a) */
} PremixGeo;
static PremixGeo g_pmGeo;
#define PREMIX_MAX 128 /* lado maximo da textura misturada (opcao 2) */
static uint64_t g_pmNs; /* (medida: tempo gasto refazendo misturas deslocadas e quantas) */
static unsigned g_pmN;
void gx_premix_stats(double *ms, unsigned *n) {
    *ms = (double)g_pmNs / 1e6;
    *n = g_pmN;
}
static uint64_t g_pmNs0;
static unsigned g_pmN0;
void gx_premix_print(void) { /* (no log, junto da resolucao: so o trecho desde a ultima vez) */
    const unsigned n = g_pmN - g_pmN0;
    if (n) {
        fprintf(stderr, "[mistura] texturas deslizando misturadas: %u (%.3f ms cada)\n", n,
                (double)(g_pmNs - g_pmNs0) / 1e6 / n);
    }
    g_pmN0 = g_pmN;
    g_pmNs0 = g_pmNs;
}
static int g_premixTile; /* 1 = a textura misturada desta montagem usa g_pmGeo.tile */
#define PREMIX_N 16
typedef struct {
    const GxTex *a, *b;
    uint64_t ka, kb;
    int v, used;
    uint64_t geo; /* forma da mistura deslocada/escalada (0 = as duas tiles iguais; lote 183) */
    uint32_t shape; /* (so tamanhos e passos, sem o deslocamento) e a tile da mistura: reaproveitada nos quadros extras */
    GxTile tile;
    GxTex *m; /* (estrutura propria: ao refazer, a antiga vai para a fila de aposentadas) */
} PremixEnt;
static PremixEnt g_pm[PREMIX_N];
static int g_pmClock;
static GxTex *g_premixTx; /* textura misturada desta montagem (fast_build) ou NULL */
static void premix_free(GxTex *m) {
    /* o filtro pre-calculado feito para a mistura entrou na conta da cache de texturas (tex_prefilter_x): sai
     * daqui tambem. Sem isso a conta so crescia a cada mistura refeita, a cache achava que estava cheia e
     * jogava fora e decodificava texturas sem parar (lentidao crescente, em qualquer lugar; lote 122) */
    gx_tex_acct(-(int64_t)m->pfBytes - (int64_t)m->pf4Bytes);
    m->pfBytes = m->pf4Bytes = 0;
    free(m->px);
    free(m->pp);
    free(m->pf);
    free(m->pf4);
    m->px = m->pp = m->pf = m->pf4 = NULL;
}
/* Misturas substituidas (lote 140): antes, refazer uma mistura parava a fila do desenho (gx_batch_flush) para a
 * auxiliar nao ler a textura sendo trocada - um engasgo a cada mistura refeita (35-50 vezes em 10 s ao entardecer,
 * medido no aparelho). Agora a mistura nova e outra estrutura e a antiga fica guardada ate 32 trocas depois (a fila
 * do desenho dura um quadro) antes de ser liberada. */
#define PREMIX_RETIRE 32
static GxTex *g_pmRetire[PREMIX_RETIRE];
static unsigned g_pmRetGen[PREMIX_RETIRE];
static int g_pmRi;
static unsigned g_flushGen; /* conta as filas desenhadas (gx_batch_flush) */
static void premix_retire(GxTex *m) {
    GxTex *old = g_pmRetire[g_pmRi];
    if (old != NULL) {
        if (g_pmRetGen[g_pmRi] == g_flushGen) {
            gx_batch_flush(); /* (32 trocas na mesma fila: raro; so entao espera a fila como antes) */
        }
        premix_free(old);
        free(old);
    }
    g_pmRetire[g_pmRi] = m;
    g_pmRetGen[g_pmRi] = g_flushGen;
    g_pmRi = (g_pmRi + 1) % PREMIX_RETIRE;
}
/* expoente de 2 da escala de uma tile (texels por unidade de S): shift 1..10 divide, 11..15 multiplica */
static int shift_exp(int sh) {
    return sh == 0 ? 0 : sh <= 10 ? -sh : 16 - sh;
}
static int exp_shift(int e) {
    return e == 0 ? 0 : e < 0 ? -e : 16 - e;
}
static int ilog2i(int v) {
    int r = 0;
    while ((1 << r) < v) r++;
    return r;
}
/* mistura com as tiles diferentes (lote 183): as duas repetem inteiras (mascara = tamanho, sem espelho nem trava);
 * monta a grade da mistura e a tile dela. 0 = nao da (grande demais, escalas muito diferentes, ...) */
static int premix_geo(const GxTex *A, const GxTex *B, const GxTile *ta, const GxTile *tb, PremixGeo *g) {
    int ax;
    if (ta->cms || ta->cmt || tb->cms || tb->cmt || !ta->masks || !ta->maskt || !tb->masks || !tb->maskt ||
        (1 << ta->masks) != A->w || (1 << ta->maskt) != A->h || (1 << tb->masks) != B->w || (1 << tb->maskt) != B->h ||
        A->clipw != A->w || A->cliph != A->h || B->clipw != B->w || B->cliph != B->h) {
        return 0;
    }
    g->tile = *ta;
    for (ax = 0; ax < 2; ax++) {
        const int eA = shift_exp(ax ? ta->shiftt : ta->shifts), eB = shift_exp(ax ? tb->shiftt : tb->shifts);
        const int ge = eA > eB ? eA : eB; /* grade: a escala mais detalhada */
        const int wa = ax ? A->h : A->w, wb = ax ? B->h : B->w;
        const int ulA = ax ? ta->ult : ta->uls, ulB = ax ? tb->ult : tb->uls;
        int ra, rb, na, nb, size, ulM, ob;
        if (ge - eA > 2 || ge - eB > 2 || ge > 5 || ge < -10) {
            return 0;
        }
        ra = 64 >> (ge - eA); /* passo de cada uma por texel da grade, em 1/64 de texel */
        rb = 64 >> (ge - eB);
        na = wa << (ge - eA); /* periodo de cada uma em texels da grade */
        nb = wb << (ge - eB);
        size = na > nb ? na : nb;
        if (size < 8 || size > (gx_mistura_desliza >= 2 ? PREMIX_MAX : 64)) {
            return 0;
        }
        /* grade: u = S*2^ge - ulM/4. Com ulM = ulA * 2^(ge-eA) a 1a fica em u * ra/64 (reduzido ao periodo, que e
         * multiplo dos dois). A 2a fica em (u + ulM/4) * rb/64 - ulB/4: em 1/64 de texel, u*rb + (ulM*rb - 64*ulB)/4 */
        ulM = (ulA << (ge - eA)) & (4 * size - 1);
        ob = (ulM * rb - 64 * ulB) / 4;
        ob &= 64 * wb - 1;
        if (ax) {
            g->sizeT = size; g->raT = ra; g->rbT = rb; g->obT = ob;
            g->tile.shiftt = (uint8_t)exp_shift(ge); g->tile.maskt = (uint8_t)ilog2i(size); g->tile.ult = (uint16_t)ulM;
        } else {
            g->sizeS = size; g->raS = ra; g->rbS = rb; g->obS = ob;
            g->tile.shifts = (uint8_t)exp_shift(ge); g->tile.masks = (uint8_t)ilog2i(size); g->tile.uls = (uint16_t)ulM;
        }
    }
    g->tile.cms = g->tile.cmt = 0;
    return 1;
}

/* o[i] = (a[i] * wa + b[i] * wb + 2^(sh-1)) >> sh, n bytes (wa + wb = 2^sh; sh = 6 ou 8) */
static void premix_lerp8(uint8_t *o, const uint8_t *a, const uint8_t *b, int wa, int wb, int sh, int n) {
    int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8x8_t va = vdup_n_u8((uint8_t)wa), vb = vdup_n_u8((uint8_t)wb);
    for (; i + 16 <= n; i += 16) {
        const uint8x16_t x = vld1q_u8(a + i), y = vld1q_u8(b + i);
        const uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(x), va), vget_low_u8(y), vb);
        const uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(x), va), vget_high_u8(y), vb);
        if (sh == 6) {
            vst1q_u8(o + i, vcombine_u8(vrshrn_n_u16(lo, 6), vrshrn_n_u16(hi, 6)));
        } else {
            vst1q_u8(o + i, vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8)));
        }
    }
#endif
    for (; i < n; i++) {
        o[i] = (uint8_t)((a[i] * wa + b[i] * wb + (1 << (sh - 1))) >> sh);
    }
}

/* uma camada da mistura numa linha da grade (n texels): a textura (RGBA8888, repete; w e h potencias de 2) lida nas
 * posicoes oS + x * rS (1/64 de texel; rS = 64, 32 ou 16) da linha posT, linear entre os vizinhos. Primeiro a linha
 * da textura (vertical), repetida o bastante para nao dar a volta; depois, para cada fase x % k (k = 64 / rS), uma
 * mistura de texels seguidos com a mesma fracao; as fases sao intercaladas na saida. */
static void premix_row(const uint8_t *px, int w, int h, int oS, int rS, int posT, int n, uint8_t *out) {
    static uint8_t ext[(PREMIX_MAX * 3 + 4) * 4] __attribute__((aligned(16))), ph[(PREMIX_MAX + 4) * 4] __attribute__((aligned(16))); /* (so a thread que monta a fila chama) */
    const int y0 = (posT >> 6) & (h - 1), y1 = (y0 + 1) & (h - 1), ft = posT & 63;
    const int k = 64 / rS, m = n / k, L = w + m + 2;
    int t, p;
    if (ft == 0) {
        memcpy(ext, px + (size_t)4 * y0 * w, (size_t)4 * w);
    } else {
        premix_lerp8(ext, px + (size_t)4 * y0 * w, px + (size_t)4 * y1 * w, 64 - ft, ft, 6, 4 * w);
    }
    for (t = w; t < L; t += w) {
        memcpy(ext + 4 * t, ext, (size_t)4 * (L - t < w ? L - t : w));
    }
    for (p = 0; p < k; p++) {
        const int pos = oS + p * rS, i0 = (pos >> 6) % w, f = pos & 63;
        uint8_t *dst = k == 1 ? out : ph;
        if (f == 0) {
            memcpy(dst, ext + 4 * i0, (size_t)4 * m);
        } else {
            premix_lerp8(dst, ext + 4 * i0, ext + 4 * (i0 + 1), 64 - f, f, 6, 4 * m);
        }
        if (k > 1) {
            const uint32_t *src = (const uint32_t *)ph;
            uint32_t *o32 = (uint32_t *)out;
            int j;
            for (j = 0; j < m; j++) {
                o32[p + k * j] = src[j];
            }
        }
    }
}

static GxTex *premix_get(const GxTex *A, const GxTex *B, int v, PremixGeo *pg) {
    PremixEnt *e = NULL;
    GxTex *m;
    int i, n;
    const uint64_t geo = pg == NULL ? 0
                         : (((uint64_t)(uint32_t)pg->obS << 40) ^ ((uint64_t)(uint32_t)pg->obT << 20) ^
                            (uint64_t)(uint32_t)(pg->sizeS << 12 | pg->sizeT << 4) ^
                            ((uint64_t)(uint32_t)(pg->raS ^ pg->raT << 7 ^ pg->rbS << 14 ^ pg->rbT << 21) << 30)) | 1;
    const uint32_t shape = pg == NULL ? 0
                           : (uint32_t)(pg->sizeS << 24 ^ pg->sizeT << 16 ^ pg->raS << 12 ^ pg->raT << 8 ^ pg->rbS << 4 ^
                                        pg->rbT) | 1u;
    (void)shape;
    /* Lote 194: as misturas deslocadas guardam varias por par de texturas (uma por deslocamento; a mesma agua pode vir
     * em pedacos com deslizamentos diferentes) e so valem com o deslocamento exato. O reaproveitamento nos quadros
     * extras (lote 187) saiu: com a camera andando a agua dava pulinhos nos quadros extras. */
    for (i = 0; i < PREMIX_N; i++) {
        if (g_pm[i].a == A && g_pm[i].b == B && g_pm[i].ka == A->key && g_pm[i].kb == B->key && g_pm[i].m != NULL &&
            (geo == 0 || g_pm[i].geo == geo)) {
            e = &g_pm[i];
            break;
        }
    }
#ifdef GX_REPLAY_DIAG
    if (e != NULL && geo != 0 && getenv("PMFORCE")) {
        e->geo = ~geo; /* (diagnostico: refaz a mistura sempre, como no jogo com a agua deslizando) */
    }
#endif
    if (e != NULL && abs(e->v - v) < 3 && e->geo == geo) {
        e->used = ++g_pmClock;
        return e->m;
    }
    if (e == NULL || (geo != 0 && e->geo != geo)) {
        e = &g_pm[0];
        for (i = 1; i < PREMIX_N; i++) {
            if (g_pm[i].used < e->used) {
                e = &g_pm[i];
            }
        }
    }
    n = pg != NULL ? pg->sizeS * pg->sizeT : A->w * A->h;
    m = (GxTex *)calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    m->px = (uint32_t *)malloc((size_t)n * 4u);
    if (m->px == NULL) {
        free(m);
        return NULL;
    }
    if (pg == NULL) {
        const uint8_t *pa = (const uint8_t *)A->px, *pb = (const uint8_t *)B->px;
        uint8_t *pm = (uint8_t *)m->px;
        for (i = 0; i < n * 4; i++) {
            int r = ((((int)pb[i] - (int)pa[i]) * v + 0x80) >> 8) + pa[i];
            pm[i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
        }
        m->w = A->w;
        m->h = A->h;
        m->clipw = A->clipw < B->clipw ? A->clipw : B->clipw;
        m->cliph = A->cliph < B->cliph ? A->cliph : B->cliph;
    } else {
        const uint8_t *pa = (const uint8_t *)A->px, *pb = (const uint8_t *)B->px;
        uint8_t *pm = (uint8_t *)m->px;
        static uint8_t la[PREMIX_MAX * 4] __attribute__((aligned(16))), lb[PREMIX_MAX * 4] __attribute__((aligned(16)));
        int y;
        const uint64_t t0 = gx_now_ns();
        for (y = 0; y < pg->sizeT; y++) {
            premix_row(pa, A->w, A->h, 0, pg->raS, y * pg->raT, pg->sizeS, la);
            premix_row(pb, B->w, B->h, pg->obS, pg->rbS, pg->obT + y * pg->rbT, pg->sizeS, lb);
            premix_lerp8(pm + (size_t)4 * y * pg->sizeS, la, lb, 256 - v, v, 8, 4 * pg->sizeS);
        }
        /* a copia com borda (tex_pad, modo repete nos dois eixos) ja pronta, com memcpy: tex_pad faria texel a texel */
        {
            const int W = pg->sizeS, H = pg->sizeT, pw = W + 2;
            uint32_t *pp = (uint32_t *)malloc((size_t)pw * (size_t)(H + 2) * 4u);
            if (pp != NULL) {
                int yy;
                for (yy = -1; yy <= H; yy++) {
                    const uint32_t *src = m->px + (size_t)(yy & (H - 1)) * W;
                    uint32_t *dst = pp + (size_t)(yy + 1) * pw;
                    dst[0] = src[W - 1];
                    memcpy(dst + 1, src, (size_t)W * 4u);
                    dst[W + 1] = src[0];
                }
                m->pp = pp;
                m->pw = pw;
                m->pkey = (uint32_t)W << 4 | (uint32_t)H << 18; /* (= a chave de tex_pad: modos 0/0, clip = tamanho) */
            }
        }
        g_pmNs += gx_now_ns() - t0;
        g_pmN++;
        m->w = m->clipw = pg->sizeS;
        m->h = m->cliph = pg->sizeT;
    }
    m->key = A->key ^ (B->key * 0x9E3779B97F4A7C15ull) ^ (uint64_t)v ^ (geo * 0xC2B2AE3D27D4EB4Full);
    m->hdk = 1;
    m->hdTried = 1;
    if (e->m != NULL) {
        premix_retire(e->m);
    }
    e->m = m;
    e->a = A;
    e->b = B;
    e->ka = A->key;
    e->kb = B->key;
    e->v = v;
    e->geo = geo;
    e->shape = shape;
    if (pg != NULL) {
        e->tile = pg->tile;
    }
    e->used = ++g_pmClock;
    return e->m;
}

/* retangulo dos texels com alfa > 0 da textura (na area util clipw x cliph) */
static void tex_bbox(GxTex *tx) {
    int x, y, x0 = tx->clipw, y0 = tx->cliph, x1 = -1, y1 = -1;
    if (tx->bbKey == tx->key && tx->bbKey != 0) {
        return;
    }
    for (y = 0; y < tx->cliph && y < tx->h; y++) {
        const uint32_t *row = tx->px + (size_t)y * tx->w;
        for (x = 0; x < tx->clipw && x < tx->w; x++) {
            if (row[x] >> 24) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) {
        tx->bb[0] = tx->bb[1] = tx->bb[2] = tx->bb[3] = 0; /* toda transparente */
    } else {
        tx->bb[0] = (int16_t)x0;
        tx->bb[1] = (int16_t)y0;
        tx->bb[2] = (int16_t)(x1 + 1);
        tx->bb[3] = (int16_t)(y1 + 1);
    }
    tx->bbKey = tx->key ? tx->key : 1;
}

/* valor de um canal do combinador (como cc_calc da referencia) */
static int crop_cc(const uint8_t *c, const int *V) {
    int r = (((V[c[0]] - V[c[1]]) * V[c[2]] + 0x80) >> 8) + V[c[3]];
    return r < 0 ? 0 : (r > 255 ? 255 : r);
}
/* o alfa final do combinador e 0 sempre que T0.a = 0? (testado com as entradas por pixel em 0, 77, 200 e 255) */
static int crop_alpha_zero(const FastProg *f) {
    static const int vals[4] = { 0, 77, 200, 255 };
    int i, k;
    for (i = 0; i < 4; i++) {
        int V[V_N], a;
        for (k = 0; k < V_N; k++) {
            V[k] = vals[i];
        }
        for (k = 0; k < 4; k++) {
            V[V_PRIM + k] = gx.prim[k];
            V[V_ENV + k] = gx.env[k];
        }
        V[V_PLODF] = gx.primLodFrac;
        V[V_ONE] = 255;
        V[V_ZERO] = 0;
        V[V_T0 + 3] = 0;
        V[V_T1 + 3] = 0;
        if (f->cyc2) {
            for (k = 0; k < 3; k++) {
                V[V_COMB + k] = crop_cc(f->c0[k], V);
            }
            V[V_COMB + 3] = crop_cc(f->c0[3], V);
        }
        a = crop_cc(f->c1[3], V);
        if (a != 0) {
            return 0;
        }
    }
    return 1;
}

static void fast_build(void) {
    FastProg *f = &g_fp;
    int k, ch, i;
    f->gen = g_pipeGen;
    f->cyc2 = pc.cyc == GX_CYC_2;
    memcpy(f->prim, gx.prim, 4);
    memcpy(f->env, gx.env, 4);
    memcpy(f->blendc, gx.blendc, 4);
    memcpy(f->fogc, gx.fogc, 4);
    f->primLodFrac = gx.primLodFrac;
    f->cimg = gx.cimg.addr;
    f->cle = gx.cle;
    f->zimg = gx.zimg;
    f->cwidth = gx.cimg.width;
    for (k = 0; k < 2; k++) {
        const CombCycle *c = &comb.cy[k];
        uint8_t (*dst)[4] = k == 0 ? f->c0 : f->c1;
        for (ch = 0; ch < 3; ch++) {
            for (i = 0; i < 4; i++) {
                dst[ch][i] = c->rgb[ch][i];
            }
        }
        for (i = 0; i < 4; i++) {
            dst[3][i] = c->a[i];
        }
    }
    for (ch = 0; ch < 4; ch++) {
        f->k0[ch] = (uint8_t)(f->c0[ch][0] == f->c0[ch][1] || f->c0[ch][2] == V_ZERO);
        f->k1[ch] = (uint8_t)(f->c1[ch][0] == f->c1[ch][1] || f->c1[ch][2] == V_ZERO);
    }
    if (f->cyc2) {
        /* no 2o ciclo TEXEL0 passa a ser a tile seguinte: troca os indices em vez de copiar */
        for (ch = 0; ch < 4; ch++) {
            for (i = 0; i < 4; i++) {
                if (f->c1[ch][i] >= V_T0 && f->c1[ch][i] < V_T0 + 4) {
                    f->c1[ch][i] = (uint8_t)(f->c1[ch][i] - V_T0 + V_T1);
                }
            }
        }
    }
    /* multiplicador do combinador que e uma constante (cor primitiva/ambiente, fracao do LOD primitivo):
     *  - valor 0: (a - b) * 0 + d = d exatamente (a referencia da o mesmo: ((a-b)*0 + 0x80) >> 8 = 0). O ceu mistura
     *    2 texturas por (T1 - T0) * alfa primitivo + T0 e em boa parte do tempo o alfa e 0: a 2a textura deixa de
     *    ser lida (amostrar e o mais caro do pixel).
     *  - valor 255 numa mistura (b = d): (a - d) * 255/256 + d fica a no maximo 1 de a; com gx_cc_aprox vira a (o
     *    ceu a noite e no entardecer: so a 1a textura). Diferenca de no maximo 1 em 255 (invisivel, a tela tem 5
     *    bits por cor); o replay compara com --sem-aprox. */
    if (gx_cc_fold) {
        int cy;
        for (cy = 0; cy < 2; cy++) {
            uint8_t (*cs)[4] = cy ? f->c1 : f->c0;
            for (ch = 0; ch < 4; ch++) {
                const int m = cs[ch][2];
                const int v = m >= V_PRIM && m < V_PRIM + 4 ? gx.prim[m - V_PRIM]
                            : m >= V_ENV && m < V_ENV + 4   ? gx.env[m - V_ENV]
                            : m == V_PLODF                  ? gx.primLodFrac
                                                            : -1;
                /* (no 2o ciclo, nao simplifica se o resultado viraria COMBINED direto: o caminho fundido nao
                 * calculava o ciclo 1 nesse caso e o alfa saia 0 - as asas da Navi sumiam, lote 118) */
                const int pass = v == 0 ? cs[ch][3] : cs[ch][0];
                if (cy == 1 && pass >= V_COMB && pass < V_COMB + 4 &&
                    (gx_dobra_comb == 0 || (gx_dobra_comb == 1 && ch == 3))) {
                    continue;
                }
                if (v == 0) {
                    cs[ch][2] = V_ZERO;
                } else if (v == 255 && gx_cc_aprox && cs[ch][1] == cs[ch][3]) {
                    cs[ch][3] = cs[ch][0];
                    cs[ch][0] = cs[ch][1] = cs[ch][2] = V_ZERO;
                }
            }
        }
        for (ch = 0; ch < 4; ch++) {
            f->k0[ch] = (uint8_t)(f->c0[ch][0] == f->c0[ch][1] || f->c0[ch][2] == V_ZERO);
            f->k1[ch] = (uint8_t)(f->c1[ch][0] == f->c1[ch][1] || f->c1[ch][2] == V_ZERO);
        }
    }
    /* ceu pre-misturado (ver premix_get): 2 ciclos, o 1o mistura T1 e T0 por uma constante k (1..254) nos canais
     * que usam textura, o 2o nao le texturas; as duas tiles com a mesma forma */
    {
        const GxTex *A = gx.rtex[0], *B = gx.rtex[1];
        const GxTile *ta = &gx.tile[gx.texTile & 7], *tb = &gx.tile[(gx.texTile + 1) & 7];
        GxTex *mix = NULL;
        int ok = gx_ceu_mistura && f->cyc2 && A != NULL && B != NULL && A->hd == NULL && B->hd == NULL &&
                 A->px != NULL && B->px != NULL;
        const int same = ok && A != B && A->w == B->w && A->h == B->h && ta->shifts == tb->shifts && ta->shiftt == tb->shiftt &&
                         ta->uls == tb->uls && ta->ult == tb->ult && ta->masks == tb->masks && ta->maskt == tb->maskt &&
                         ta->cms == tb->cms && ta->cmt == tb->cmt;
        PremixGeo *pg = NULL;
        int kv = -1, msel = -1;
        g_premixTile = 0;
        if (ok && !same) {
            /* (a mesma textura nas duas tiles com a mesma forma: nada a misturar; premix_geo recusa as tiles iguais) */
            ok = gx_mistura_desliza &&
                 !(A == B && ta->uls == tb->uls && ta->ult == tb->ult && ta->shifts == tb->shifts && ta->shiftt == tb->shiftt) &&
                 premix_geo(A, B, ta, tb, &g_pmGeo);
            pg = &g_pmGeo;
        }
        for (ch = 0; ok && ch < 4; ch++) {
            const uint8_t *c = f->c0[ch];
            int i2, usesT = 0;
            for (i2 = 0; i2 < 4; i2++) {
                usesT |= (c[i2] >= V_T0 && c[i2] < V_T1 + 4);
            }
            if (!usesT) {
                continue;
            }
            if (c[0] == V_T1 + ch && c[1] == V_T0 + ch && c[3] == V_T0 + ch &&
                ((c[2] >= V_PRIM && c[2] < V_PRIM + 4) || (c[2] >= V_ENV && c[2] < V_ENV + 4)) &&
                (msel < 0 || msel == c[2])) {
                msel = c[2];
            } else {
                ok = 0;
            }
        }
        for (ch = 0; ok && ch < 4; ch++) {
            int i2;
            for (i2 = 0; i2 < 4; i2++) {
                if (f->c1[ch][i2] >= V_T0 && f->c1[ch][i2] < V_T1 + 4) {
                    ok = 0;
                }
            }
        }
        if (ok && msel >= 0) {
            kv = msel < V_ENV ? gx.prim[msel - V_PRIM] : gx.env[msel - V_ENV];
            if (kv > 0 && kv < 255) {
                mix = premix_get(A, B, kv, pg);
            }
        }
        g_premixTx = mix;
        g_premixTile = mix != NULL && pg != NULL;
        if (mix != NULL) {
            for (ch = 0; ch < 4; ch++) {
                if (f->c0[ch][2] == msel && f->c0[ch][0] == V_T1 + ch) {
                    f->c0[ch][0] = f->c0[ch][1] = f->c0[ch][2] = V_ZERO;
                    f->k0[ch] = 1;
                }
            }
        }
    }
    f->bilerp = pc.bilerp;
    f->zcmp = pc.zcmp;
    f->zupd = pc.zupd;
    f->zdec = pc.zmode == GX_RM_ZMODE_DEC;
    f->cvgXAlpha = pc.cvgXAlpha;
    f->ac = pc.ac;
    f->cdith = pc.cdith;
    f->alphaCvgSel = pc.alphaCvgSel;
    f->forceBl = pc.forceBl;
    memcpy(f->bl, pc.bl, sizeof f->bl);
    /* Alfa da entrada do blender constante (combinador trivial lendo uma constante, ou 255 fixo):
     *  - 255 com B = 1-A: (P*255 + M*0)/255 = P exato -> igual a B = 0 (nao le a memoria). E o caso do
     *    retangulo de tela inteira que pinta o fundo (cor do ceu/neblina) em cada quadro.
     *  - reprovado no teste de alfa/cobertura: a primitiva nao escreve nada (nem cor nem profundidade). */
    f->dead = 0;
    {
        int aConst = -1;
        if (f->alphaCvgSel && !f->cvgXAlpha) {
            aConst = 255;
        } else if (f->k1[3]) {
            int v = f->c1[3][3];
            aConst = v >= V_PRIM && v < V_PRIM + 4 ? gx.prim[v - V_PRIM]
                   : v >= V_ENV && v < V_ENV + 4   ? gx.env[v - V_ENV]
                   : v == V_ONE                    ? 255
                   : v == V_ZERO                   ? 0
                   : v == V_PLODF                  ? gx.primLodFrac
                                                   : -1;
        }
        if (aConst == 255) {
            int c, nc = f->cyc2 ? 2 : 1;
            for (c = 0; c < nc; c++) {
                if (f->bl[c][1] == 0 && f->bl[c][3] == 0) {
                    f->bl[c][3] = 3;
                }
            }
        }
        /* o teste usa o alfa do combinador mesmo com alphaCvgSel */
        if (f->k1[3] && !(f->alphaCvgSel && !f->cvgXAlpha)) {
            if ((f->cvgXAlpha && (aConst >= 0 && (aConst >> 5) == 0)) ||
                (f->ac == 1 && aConst >= 0 && aConst < gx.blendc[3])) {
                f->dead = 1;
            }
        }
    }
    /* o blender le a memoria so se algum seletor USADO for MEM (1):
     *  - ciclo de mistura: P sempre; M so se A != 0 (com A = 0 o resultado e M, com B = 0 e P)
     *  - ciclo sem mistura: so P */
    {
        int c, nc = f->cyc2 ? 2 : 1, need = 0;
        for (c = 0; c < nc; c++) {
            int blends = (f->cyc2 && c == 0) || f->forceBl;
            if (!blends) {
                need |= f->bl[c][0] == 1;
            } else if (f->bl[c][1] == 3) {
                need |= (f->bl[c][3] == 3 ? f->bl[c][0] : f->bl[c][2]) == 1;
            } else if (f->bl[c][3] == 3) {
                need |= f->bl[c][0] == 1;
            } else {
                need |= f->bl[c][0] == 1 || f->bl[c][2] == 1;
            }
        }
        f->needMem = need;
    }
    /* quais canais alguem usa (de tras para frente). A cor final sempre e usada; o alfa final so se
     * houver teste de alfa/cobertura ou se o blender ler o alfa da entrada */
    {
        int c, nc = f->cyc2 ? 2 : 1, aUsed = 0;
        for (c = 0; c < nc; c++) {
            int blends = (f->cyc2 && c == 0) || f->forceBl;
            f->blUsesA[c] = blends && f->bl[c][1] != 3 && f->bl[c][3] != 3;
            if (f->blUsesA[c] && f->bl[c][1] == 0) {
                aUsed = 1;
            }
        }
        if (f->alphaCvgSel && !f->cvgXAlpha) {
            aUsed = 0; /* o blender ve 255 */
        }
        if (f->cvgXAlpha || f->ac != 0) {
            aUsed = 1;
        }
        f->need1[0] = f->need1[1] = f->need1[2] = 1;
        f->need1[3] = (uint8_t)aUsed;
    }
    f->needV = 0;
    {
        uint32_t r1 = 0, r0 = 0;
        for (ch = 0; ch < 4; ch++) {
            if (!f->need1[ch]) {
                continue;
            }
            if (f->k1[ch]) {
                r1 |= 1u << f->c1[ch][3];
            } else {
                for (i = 0; i < 4; i++) {
                    r1 |= 1u << f->c1[ch][i];
                }
            }
        }
        for (ch = 0; ch < 4; ch++) {
            f->need0[ch] = (uint8_t)(f->cyc2 && ((r1 >> (V_COMB + ch)) & 1));
            if (!f->need0[ch]) {
                continue;
            }
            if (f->k0[ch]) {
                r0 |= 1u << f->c0[ch][3];
            } else {
                for (i = 0; i < 4; i++) {
                    r0 |= 1u << f->c0[ch][i];
                }
            }
        }
        f->needV = r0 | r1;
        if (!f->cyc2) {
            f->needV &= ~(0xFu << V_COMB); /* no 1o ciclo COMBINED e 0 */
        }
    }
    for (ch = 0; ch < 4; ch++) {
        f->needShade[ch] = (f->needV >> (V_SHADE + ch)) & 1;
        f->needTC[0][ch] = (f->needV >> (V_T0 + ch)) & 1;
        f->needTC[1][ch] = (f->needV >> (V_T1 + ch)) & 1;
    }
    /* o blender pode usar o alfa do sombreamento */
    if ((f->blUsesA[0] && pc.bl[0][1] == 2) || (f->cyc2 && f->blUsesA[1] && pc.bl[1][1] == 2)) {
        f->needShade[3] = 1;
    }
    /* so amostra a textura se algum canal dela for lido (a referencia amostra assim mesmo; nao muda nada) */
    f->useT[0] = pc.useT0 && (f->needV & (0xFu << V_T0)) != 0;
    f->useT[1] = pc.useT1 && (f->needV & (0xFu << V_T1)) != 0;
    /* constantes usadas */
    {
        int32_t cv[V_N];
        memset(cv, 0, sizeof cv);
        for (k = 0; k < 4; k++) {
            cv[V_PRIM + k] = gx.prim[k];
            cv[V_ENV + k] = gx.env[k];
        }
        cv[V_PLODF] = gx.primLodFrac;
        cv[V_LODF] = GX_LODF_SEM_LOD;
        cv[V_ONE] = 255;
        cv[V_NOISE] = 128;
        f->nConst = 0;
        for (k = 0; k < V_N; k++) {
            if (((f->needV >> k) & 1) && cv[k] != 0) {
                f->cIdx[f->nConst] = (uint8_t)k;
                f->cVal[f->nConst] = cv[k];
                f->nConst++;
            }
        }
    }
    for (k = 0; k < 2; k++) {
        const GxTile *tl = (k == 0 && g_premixTile) ? &g_pmGeo.tile : &gx.tile[(gx.texTile + k) & 7];
        FastTex *u = &f->t[k];
        int hk = 1, hb = 0;
        u->tx = (k == 0 && g_premixTx != NULL) ? g_premixTx : gx.rtex[k];
        if (u->tx != NULL && u->tx->hd != NULL && gx_hd_use) {
            /* textura nova (gfx_hd.c), hk vezes maior: coordenadas e mascaras na escala dela */
            u->tx = u->tx->hd;
            hk = u->tx->hdk;
            hb = hk == 4 ? 2 : hk == 2 ? 1 : 0;
        }
        u->scaleS = shift_scale(tl->shifts) * (float)hk;
        u->scaleT = shift_scale(tl->shiftt) * (float)hk;
        /* com filtro, o texel i da original fica no ponto i: o texel j da nova (i*hk .. i*hk+hk-1) fica em
         * (j - (hk-1)/2) / hk; em 1/32 de texel: s_nova = hk * s + 16 * (hk - 1). Sem filtro: hk * s. */
        u->hdPoint = hk > 1 && f->bilerp && !gx_hd_filtro;
        u->offS = (float)tl->uls * 8.0f * (float)hk - (f->bilerp ? 16.0f * (float)(hk - 1) : 0.0f);
        u->offT = (float)tl->ult * 8.0f * (float)hk - (f->bilerp ? 16.0f * (float)(hk - 1) : 0.0f);
        if (u->hdPoint) {
            /* texel mais proximo: arredonda (+ meio texel) em vez do filtro */
            u->offS -= 16.0f;
            u->offT -= 16.0f;
        }
        u->cms = tl->cms;
        u->cmt = tl->cmt;
        u->maskS = tl->masks ? (hk << tl->masks) - 1 : 0;
        u->maskT = tl->maskt ? (hk << tl->maskt) - 1 : 0;
        u->bitS = tl->masks ? tl->masks + hb : 0;
        u->bitT = tl->maskt ? tl->maskt + hb : 0;
        /* modo de cada eixo: so os casos que a referencia resolve com uma mascara simples */
        u->modeS = u->modeT = 3;
        if (u->tx == NULL) {
            u->px = NULL;
            f->samp[k] = samp_null;
            continue;
        }
        u->px = u->tx->px;
        u->w = u->tx->w;
        u->h = u->tx->h;
        u->clipw = u->tx->clipw;
        u->cliph = u->tx->cliph;
        if (tl->masks && !(tl->cms & 2) && u->w == (hk << tl->masks)) {
            u->modeS = (tl->cms & 1) ? 1 : 0;
        } else if (!tl->masks || u->w == (hk << tl->masks)) {
            u->modeS = 2; /* sem mascara: prende; com clamp + mascara: prende e mascara */
        }
        if (tl->maskt && !(tl->cmt & 2) && u->h == (hk << tl->maskt)) {
            u->modeT = (tl->cmt & 1) ? 1 : 0;
        } else if (!tl->maskt || u->h == (hk << tl->maskt)) {
            u->modeT = 2;
        }
        f->samp[k] = g_sampTab[f->bilerp && !u->hdPoint ? 1 : 0][u->modeS][u->modeT];
    }
    /* recorte dos transparentes: 1 textura (T0), sem textura nova, e um pixel de alfa 0 nao muda a tela: ou o teste de
     * alfa/cobertura o descarta, ou a mistura final e (cor * a + memoria * (1 - a)) sem pontilhado, sem gravar z */
    f->crop = 0;
    if (gx_corte && f->useT[0] && !f->useT[1] && f->t[0].tx != NULL && f->t[0].tx->hd == NULL && f->t[0].tx->px != NULL &&
        !f->t[0].hdPoint && f->t[0].scaleS > 0.0f && f->t[0].scaleT > 0.0f) {
        const int last = f->cyc2 ? 1 : 0;
        const int killed = (f->ac == 1 && gx.blendc[3] > 0) || f->cvgXAlpha;
        const int blendKeeps = f->forceBl && f->bl[last][0] == 0 && f->bl[last][1] == 0 && f->bl[last][2] == 1 &&
                               f->bl[last][3] == 0 && !(f->alphaCvgSel && !f->cvgXAlpha) && !f->cdith && !f->zupd;
        if ((killed || blendKeeps) && crop_alpha_zero(f)) {
            GxTex *tx = (GxTex *)f->t[0].tx;
            tex_bbox(tx);
            /* vale a pena: o retangulo visivel e menor que a textura */
            if ((tx->bb[2] - tx->bb[0]) * (tx->bb[3] - tx->bb[1]) * 2 <= tx->clipw * tx->cliph) { /* (so se corta metade ou mais: lote 126) */
                /* folga de 2 texels para o filtro (o texel vizinho entra na conta ate 1 texel de distancia) */
                f->crop = 1;
                f->cropS0 = (float)(tx->bb[0] - 2) * 32.0f;
                f->cropS1 = (float)(tx->bb[2] + 2) * 32.0f;
                f->cropT0 = (float)(tx->bb[1] - 2) * 32.0f;
                f->cropT1 = (float)(tx->bb[3] + 2) * 32.0f;
                f->cropW = (float)tx->w * 32.0f;
                f->cropH = (float)tx->h * 32.0f;
            }
        }
    }
    fused_check(f);
#ifdef GX_REPLAY_DIAG
    if (getenv("BGDBG")) {
        /* (diagnostico) como cada estado novo e desenhado: uma linha por textura+modo */
        static uint32_t seen[64][2];
        static int nseen;
        int k, dup = 0;
        for (k = 0; k < nseen; k++) {
            if (seen[k][0] == gx.timgRaw && seen[k][1] == gx.omL) dup = 1;
        }
        if (!dup && nseen < 64) {
            seen[nseen][0] = gx.timgRaw;
            seen[nseen++][1] = gx.omL;
            fprintf(stderr, "[prog] tex %08x omL %08x: fused %d kern %d fblm %d cyc2 %d bilerp %d needMem %d crop %d | t0 %dx%d modo %d/%d pfs %d | t1 %d %dx%d modo %d/%d pfs %d\n",
                    gx.timgRaw, gx.omL, f->fused, f->kern, f->fblm, f->cyc2, f->bilerp, f->needMem, f->crop,
                    f->t[0].w, f->t[0].h, f->t[0].modeS, f->t[0].modeT, f->t[0].pfs,
                    f->useT[1], f->t[1].w, f->t[1].h, f->t[1].modeS, f->t[1].modeT, f->t[1].pfs);
            diag_ops(f);
        }
    }
#endif
}

/* ------------------------------------------------------------------ lacos sobre vetores de pixels */
/* u16 com os bytes na ordem do N64 (alto, baixo): uma leitura/escrita de 16 bits + troca de bytes */
static inline int32_t rd_be16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap16(v);
#endif
    return v;
}
static inline void wr_be16(uint8_t *p, uint32_t x) {
    uint16_t v = (uint16_t)x;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap16(v);
#endif
    memcpy(p, &v, 2);
}

/* cor: na ordem do N64 (big-endian) ou, na imagem maior com a opcao cor_direta, na do processador (o chip 2D da tela
 * le direto como RGBA5551, sem a conversao na CPU) */
static inline int32_t rd_c16(const uint8_t *p, int le) {
    uint16_t v;
    memcpy(&v, p, 2);
    return le ? (int32_t)v : (int32_t)__builtin_bswap16(v);
}
static inline void wr_c16(uint8_t *p, uint32_t x, int le) {
    uint16_t v = (uint16_t)x;
    if (!le) {
        v = __builtin_bswap16(v);
    }
    memcpy(p, &v, 2);
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static inline uint8x16_t crev(uint8x16_t v, int le) {
    return le ? v : vrev16q_u8(v);
}
#endif

static inline int32_t clamp255i(int32_t v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void fill_u8(uint8_t *restrict d, int v, int n) {
    memset(d, v, (size_t)n);
}

/* (A - B) * C + D do combinador, com o arredondamento e o limite da referencia.
 * Os vetores tem FB bytes; os grupos de 8 podem passar de n (o que sobra e ignorado). */
static void cc_loop(uint8_t *restrict out, const uint8_t *restrict a, const uint8_t *restrict b,
                    const uint8_t *restrict c, const uint8_t *restrict d, int n) {
    int j = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; j < n; j += 8) {
        uint8x8_t va = vld1_u8(a + j), vb = vld1_u8(b + j), vc = vld1_u8(c + j), vd = vld1_u8(d + j);
        uint16x8_t ac = vmull_u8(va, vc), bc = vmull_u8(vb, vc);
        /* (a-b)*c = a*c - b*c (diferenca com sinal em 32 bits); (x + 0x80) >> 8 = arredondamento do vrshrn */
        int32x4_t lo = vreinterpretq_s32_u32(vsubl_u16(vget_low_u16(ac), vget_low_u16(bc)));
        int32x4_t hi = vreinterpretq_s32_u32(vsubl_u16(vget_high_u16(ac), vget_high_u16(bc)));
        int16x8_t m = vcombine_s16(vrshrn_n_s32(lo, 8), vrshrn_n_s32(hi, 8));
        int16x8_t r = vaddq_s16(m, vreinterpretq_s16_u16(vmovl_u8(vd)));
        vst1_u8(out + j, vqmovun_s16(r)); /* satura em 0..255 = clamp255 */
    }
#else
    for (; j < n; j++) {
        out[j] = (uint8_t)clamp255i((((a[j] - b[j]) * c[j] + 0x80) >> 8) + d[j]);
    }
#endif
}

/* um canal do blender: devolve o vetor do resultado (pode ser um dos de entrada) */
static const uint8_t *bl_chan(uint8_t *restrict out, int bA, int bB, const uint8_t *P, const uint8_t *M,
                              const uint8_t *A, int n) {
    int j = 0;
    if (bA == 3) {
        /* A = 0: (P*0 + M*B)/B = M (B > 0); com B = 0 a soma e 0 e fica P */
        return bB == 3 ? P : M;
    }
    if (bB == 3) {
        /* B = 0: (P*A)/A = P (e P quando A = 0) */
        return P;
    }
    if (bB == 0) {
        /* (1-A): soma 255, divisao exata por 255. x = P*A + M*(255-A) <= 65025 cabe em 16 bits */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const uint8x8_t v255 = vdup_n_u8(255);
        const uint16x8_t one = vdupq_n_u16(1);
        for (; j < n; j += 8) {
            uint8x8_t vA = vld1_u8(A + j);
            uint16x8_t x = vmull_u8(vld1_u8(P + j), vA);
            x = vmlal_u8(x, vld1_u8(M + j), vsub_u8(v255, vA));
            x = vaddq_u16(vsraq_n_u16(x, x, 8), one); /* x + (x >> 8) + 1 */
            vst1_u8(out + j, vshrn_n_u16(x, 8));
        }
#else
        for (; j < n; j++) {
            int32_t x = P[j] * A[j] + M[j] * (255 - A[j]);
            out[j] = (uint8_t)clamp255i((x + 1 + (x >> 8)) >> 8);
        }
#endif
        return out;
    }
    /* B = 255 (MEM_A ou 1): soma A + 255 > 0 */
    for (; j < n; j++) {
        int32_t sum = A[j] + 255;
        out[j] = (uint8_t)clamp255i((P[j] * A[j] + M[j] * 255) / sum);
    }
    return out;
}

static const uint8_t g_zero[FB];

/* seletor do blender (P ou M) */
static const uint8_t *bl_sel(int sel, int ch, const uint8_t *const *in, const uint8_t *const *mem, const Chan4 *cst) {
    switch (sel) {
        case 0: return in[ch];
        case 1: return mem[ch];
        case 2: return cst[0].v[ch]; /* cor do blend */
        default: return cst[1].v[ch]; /* cor da neblina */
    }
}

/* separa os texels RGBA8888 (byte 0 = R) em vetores por canal (so os canais pedidos) */
static void tex_unpack(const uint32_t *restrict tp, uint8_t (*restrict tc)[FB], const int *need, int n) {
    int j = 0, ch;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* grupos de 16 (podem passar de n: tp e tc tem FB posicoes, FB multiplo de 16) */
    for (; j < n; j += 16) {
        uint8x16x4_t v = vld4q_u8((const uint8_t *)(tp + j));
        for (ch = 0; ch < 4; ch++) {
            if (need[ch]) {
                vst1q_u8(tc[ch] + j, v.val[ch]);
            }
        }
    }
#endif
    for (ch = 0; ch < 4; ch++) {
        if (need[ch]) {
            const int sh8 = ch * 8;
            int i;
            for (i = j; i < n; i++) {
                tc[ch][i] = (uint8_t)(tp[i] >> sh8);
            }
        }
    }
}

/* ------------------------------------------------------------------ span */
/* indices 0, 1, 2... (lista de pixels quando todos passam) */
static const int32_t k_ident[FB + 8] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
};

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* Teste de profundidade de 4 em 4 pixels (NEON): mesmas contas do laco escalar do gx_span_fast.
 * Trata os n pixels (o ultimo grupo le a profundidade de uma copia, para nao passar do fim da linha).
 * ok[j] = 1 se o pixel passa; zc[j] = profundidade comprimida (14 bits) para gravar.
 * Devolve 1 se todos os n pixels passaram. */
static int z_test_neon(uint32_t bz, uint32_t dz, const uint8_t *zp, int n, int zcmp, int zdec, uint32_t tol,
                       int32_t *zc, uint32_t *ok) {
    uint32x4_t vb = vmlaq_n_u32(vdupq_n_u32(bz), vld1q_u32((const uint32_t *)k_ident), dz);
    const uint32x4_t step = vdupq_n_u32(dz * 4u);
    const int32x4_t r128 = vdupq_n_s32(128), zmax = vdupq_n_s32(0x3FFFF), zero = vdupq_n_s32(0);
    const uint32x4_t seven = vdupq_n_u32(7), m7ff = vdupq_n_u32(0x7FF), top = vdupq_n_u32(0x40000);
    const int32x4_t six = vdupq_n_s32(6);
    const uint32x4_t vtol = vdupq_n_u32(tol), all = vdupq_n_u32(~0u);
    uint32x4_t acc = all;
    uint8_t tail[8];
    int j;
    for (j = 0; j < n; j += 4) {
        int32x4_t zv = vshrq_n_s32(vaddq_s32(vreinterpretq_s32_u32(vb), r128), 8);
        uint32x4_t z18, e, zz, zm, pass;
        const uint8_t *src = zp + 2 * j;
        if (j + 4 > n) {
            memset(tail, 0, sizeof tail);
            memcpy(tail, src, (size_t)(n - j) * 2u);
            src = tail;
        }
        zv = vminq_s32(vmaxq_s32(zv, zero), zmax);
        z18 = vreinterpretq_u32_s32(zv);
        e = vminq_u32(vclzq_u32(vmvnq_u32(vshlq_n_u32(z18, 14))), seven);
        zz = vorrq_u32(vshlq_n_u32(e, 11),
                       vandq_u32(vshlq_u32(z18, vminq_s32(vsubq_s32(vreinterpretq_s32_u32(e), six), zero)), m7ff));
        zm = vshrq_n_u32(vmovl_u16(vreinterpret_u16_u8(vrev16_u8(vld1_u8(src)))), 2);
        if (!zcmp) {
            pass = all;
        } else if (!zdec) {
            pass = vcleq_u32(zz, zm);
        } else {
            uint32x4_t em = vandq_u32(vshrq_n_u32(zm, 11), seven);
            int32x4_t shl = vmaxq_s32(vsubq_s32(six, vreinterpretq_s32_u32(em)), zero);
            uint32x4_t dec = vaddq_u32(vshlq_u32(vandq_u32(zm, m7ff), shl),
                                       vsubq_u32(top, vshlq_u32(top, vnegq_s32(vreinterpretq_s32_u32(em)))));
            pass = vcleq_u32(z18, vaddq_u32(dec, vtol));
        }
        if (j + 4 > n) {
            /* pistas alem de n contam como aprovadas no "todos passaram" */
            pass = vorrq_u32(pass, vcgeq_u32(vld1q_u32((const uint32_t *)k_ident), vdupq_n_u32((uint32_t)(n - j))));
        }
        acc = vandq_u32(acc, pass);
        vst1q_s32(zc + j, vreinterpretq_s32_u32(zz));
        vst1q_u32(ok + j, vandq_u32(pass, vdupq_n_u32(1)));
        vb = vaddq_u32(vb, step);
    }
    {
        uint32x2_t m = vand_u32(vget_low_u32(acc), vget_high_u32(acc));
        return (vget_lane_u32(m, 0) & vget_lane_u32(m, 1)) != 0;
    }
}
#endif


/* ------------------------------------------------------------------ caminho fundido (NEON)
 * Mesmas contas do gx_span_fast, mas 8 pixels por vez do comeco ao fim (profundidade, sombreamento,
 * texturas, combinador, alfa, blender, escrita) sem passar por vetores intermediarios na memoria e
 * sem lista de pixels vivos: os reprovados sao calculados e descartados na escrita (mascara).
 * fused_check decide, por estado, se este caminho serve (os casos raros ficam no gx_span_fast). */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* ---- amostragem de 8 pixels pela copia com borda (GxTex.pp) ----
 * Com 1 texel de borda que ja guarda o que a tile da fora da textura (repete, espelha ou prende), os 2x2
 * texels vizinhos de um pixel estao sempre lado a lado: (a, a+1) na linha b e na linha b+1. Cada pixel
 * le 2 pares de 64 bits em vez de 3 texels com o endereco de cada um recalculado na borda.
 * O espelho vira repeticao com largura dobrada (a copia guarda o periodo inteiro: original + espelhado).
 * Mesmas contas (inteiras) do samp_tpl: o filtro de 3 pontos do N64 usa (c00, c10, c01) ou (c11, c01, c10);
 * aqui entram os 4 texels, com peso 0 no que nao conta, entao a soma e a mesma. */
static void tex_pad(FastTex *u) {
    GxTex *tx = (GxTex *)u->tx;
    const int w = tx->w, h = tx->h;
    /* largura/altura do periodo: espelho = 2x */
    const int W = u->modeS == 1 ? 2 * w : w, H = u->modeT == 1 ? 2 * h : h;
    const uint32_t key = (uint32_t)u->modeS | ((uint32_t)u->modeT << 2) | ((uint32_t)tx->clipw << 4) | ((uint32_t)tx->cliph << 18);
    int x, y;
    u->pmS = W - 1;
    u->pmT = H - 1;
    u->pcS = tx->clipw - 1;
    u->pcT = tx->cliph - 1;
    if (tx->pp == NULL || tx->pkey != key) {
        uint32_t *pp;
        const int pw = W + 2;
        const uint64_t t0 = gx_now_ns();
        if (tx->pp != NULL) {
            gx_batch_flush(); /* a fila pode estar usando a copia antiga */
            free(tx->pp);
            tx->pp = NULL;
            free(tx->pf);
            tx->pf = NULL;
            gx_tex_acct(-(int64_t)tx->pfBytes);
            tx->pfBytes = 0;
            free(tx->pf4);
            tx->pf4 = NULL;
            gx_tex_acct(-(int64_t)tx->pf4Bytes);
            tx->pf4Bytes = 0;
        }
        pp = (uint32_t *)malloc((size_t)pw * (size_t)(H + 2) * 4u);
        if (pp == NULL) {
            u->pp = NULL;
            return;
        }
        for (y = -1; y <= H; y++) {
            int sy;
            if (u->modeT == 2) {
                sy = y < 0 ? 0 : (y > tx->cliph - 1 ? tx->cliph - 1 : y);
            } else {
                sy = y & (H - 1);
                if (u->modeT == 1 && sy >= h) {
                    sy = 2 * h - 1 - sy;
                }
            }
            for (x = -1; x <= W; x++) {
                int sx;
                if (u->modeS == 2) {
                    sx = x < 0 ? 0 : (x > tx->clipw - 1 ? tx->clipw - 1 : x);
                } else {
                    sx = x & (W - 1);
                    if (u->modeS == 1 && sx >= w) {
                        sx = 2 * w - 1 - sx;
                    }
                }
                pp[(size_t)(y + 1) * pw + (x + 1)] = tx->px[(size_t)sy * w + sx];
            }
        }
        tx->pp = pp;
        tx->pw = pw;
        tx->pkey = key;
        gx_stN[1]++;
        gx_stNs[1] += gx_now_ns() - t0;
    }
    u->pp = tx->pp;
    u->pw = tx->pw;
}

/* Filtro de textura pre-calculado (opcao filtro_rapido; ideia do port do Mario Kart 64): cada texel da
 * copia com borda vira NxN celulas (N = 2 ou 4), e cada celula guarda o filtro de 3 pontos ja feito no
 * meio dela. Na hora de desenhar e uma leitura so por pixel, em vez de 4 texels e pesos. Diferenca: a
 * fracao do filtro anda em passos de 1/N de texel, em vez de 1/32. Com N = 2 a textura fica 4x maior na
 * memoria (com 4, 16x: o cache do Miyoo sofre). Texturas grandes (mais de 1 MB pre-calculado) ficam com
 * o filtro exato. Devolve 0 se nao da. */

/* Celulas 2x2 de 2 linhas de texels (b e b+1, n texels de cada, mais 1 a direita): com N = 2 os pesos do
 * filtro de 3 pontos sao fixos em cada uma das 4 celulas de um texel (fracoes 8 e 24 de 32):
 *   (0,0): (2*c00 + c10 + c01 + 2) >> 2      (1,0): (3*c10 + c01 + 2) >> 2
 *   (0,1): (c10 + 3*c01 + 2) >> 2            (1,1): (c10 + c01 + 2*c11 + 2) >> 2
 * (as mesmas contas do laco geral, que da (8*x + 16) >> 5). 4 texels por vez, canais juntos. */
static void pf2_rows(uint32_t *o0, uint32_t *o1, const uint8_t *r0, const uint8_t *r1, int n) {
    int a = 0;
    for (; a + 4 <= n; a += 4) {
        const uint8x16_t c00 = vld1q_u8(r0 + 4 * a), c10 = vld1q_u8(r0 + 4 * a + 4);
        const uint8x16_t c01 = vld1q_u8(r1 + 4 * a), c11 = vld1q_u8(r1 + 4 * a + 4);
        uint16x8_t s_lo = vaddl_u8(vget_low_u8(c10), vget_low_u8(c01)), s_hi = vaddl_u8(vget_high_u8(c10), vget_high_u8(c01));
        uint32x4x2_t w;
        /* linha de cima: (0,0) e (1,0) */
        w.val[0] = vreinterpretq_u32_u8(vcombine_u8(vrshrn_n_u16(vaddq_u16(s_lo, vshll_n_u8(vget_low_u8(c00), 1)), 2),
                                                    vrshrn_n_u16(vaddq_u16(s_hi, vshll_n_u8(vget_high_u8(c00), 1)), 2)));
        w.val[1] = vreinterpretq_u32_u8(vcombine_u8(vrshrn_n_u16(vaddq_u16(s_lo, vshll_n_u8(vget_low_u8(c10), 1)), 2),
                                                    vrshrn_n_u16(vaddq_u16(s_hi, vshll_n_u8(vget_high_u8(c10), 1)), 2)));
        vst2q_u32(o0 + 2 * a, w);
        /* linha de baixo: (0,1) e (1,1) */
        w.val[0] = vreinterpretq_u32_u8(vcombine_u8(vrshrn_n_u16(vaddq_u16(s_lo, vshll_n_u8(vget_low_u8(c01), 1)), 2),
                                                    vrshrn_n_u16(vaddq_u16(s_hi, vshll_n_u8(vget_high_u8(c01), 1)), 2)));
        w.val[1] = vreinterpretq_u32_u8(vcombine_u8(vrshrn_n_u16(vaddq_u16(s_lo, vshll_n_u8(vget_low_u8(c11), 1)), 2),
                                                    vrshrn_n_u16(vaddq_u16(s_hi, vshll_n_u8(vget_high_u8(c11), 1)), 2)));
        vst2q_u32(o1 + 2 * a, w);
    }
    for (; a < n; a++) {
        const uint8_t *c00 = r0 + 4 * a, *c10 = c00 + 4, *c01 = r1 + 4 * a, *c11 = c01 + 4;
        uint8_t *p00 = (uint8_t *)(o0 + 2 * a), *p10 = p00 + 4, *p01 = (uint8_t *)(o1 + 2 * a), *p11 = p01 + 4;
        int k;
        for (k = 0; k < 4; k++) {
            const int s = c10[k] + c01[k];
            p00[k] = (uint8_t)((s + 2 * c00[k] + 2) >> 2);
            p10[k] = (uint8_t)((s + 2 * c10[k] + 2) >> 2);
            p01[k] = (uint8_t)((s + 2 * c01[k] + 2) >> 2);
            p11[k] = (uint8_t)((s + 2 * c11[k] + 2) >> 2);
        }
    }
}

/* fine = 1: a tabela 4x4 de perto (GxTex.pf4: filtro_perto_fino), independente da principal (pf, filtro_rapido) */
static int tex_prefilter_x(FastTex *u, int fine) {
    GxTex *tx = (GxTex *)u->tx;
    /* (8x8 testado no aparelho: a memoria do filtro e 64x a da textura - a cache estourava, o jogo ficou muito lento e
     * travou ao carregar uma cena. So 2x2 e 4x4) */
    const int ps = fine ? 2 : (gx_tex_prefilter >= 2 ? 2 : 1), n = 1 << ps, step = 32 >> ps;
    uint32_t **PF = fine ? &tx->pf4 : &tx->pf;
    int *PFW = fine ? &tx->pf4w : &tx->pfw;
    uint32_t *PFKEY = fine ? &tx->pf4key : &tx->pfkey;
    uint32_t *PFB = fine ? &tx->pf4Bytes : &tx->pfBytes;
    const int pw = tx->pw, ph = u->pmT + 1 + 2;
    const int cw = (pw - 1) * n, ch = (ph - 1) * n;
    if (tx->pp == NULL || (size_t)cw * (size_t)ch * 4u > (ps >= 3 ? (4u << 20) : (1u << 20))) {
        return 0;
    }
    {
        /* com celulas menores o filtro pre-calculado ocupa 4x (4x4) ou 16x (8x8) mais: a cache de texturas cresce */
        extern uint32_t gx_tex_cache_bytes;
        const uint32_t want = ps >= 2 ? (fine ? (24u << 20) : (32u << 20)) : 0u;
        if (gx_tex_cache_bytes < want) {
            gx_tex_cache_bytes = want;
        }
    }
    if (*PF == NULL || *PFKEY != tx->pkey || *PFW != cw) {
        uint32_t *pf;
        const uint8_t *pp = (const uint8_t *)tx->pp;
        int cx, cy, k;
        const uint64_t t0 = gx_now_ns();
        if (*PF != NULL) {
            gx_batch_flush();
            free(*PF);
            *PF = NULL;
            gx_tex_acct(-(int64_t)*PFB);
            *PFB = 0;
        }
        pf = (uint32_t *)malloc((size_t)cw * (size_t)ch * 4u);
        if (pf == NULL) {
            return 0;
        }
        for (cy = ps == 1 ? ch : 0; cy < ch; cy++) {
            const int b = cy >> ps, ft = (cy & (n - 1)) * step + step / 2;
            uint8_t *o = (uint8_t *)(pf + (size_t)cy * cw);
            for (cx = 0; cx < cw; cx++, o += 4) {
                const int a = cx >> ps, fs = (cx & (n - 1)) * step + step / 2, sum = fs + ft;
                const uint8_t *c00 = pp + ((size_t)b * pw + a) * 4, *c10 = c00 + 4;
                const uint8_t *c01 = c00 + (size_t)pw * 4, *c11 = c01 + 4;
                int w00, w10, w01, w11;
                if (sum < 32) {
                    w00 = 32 - sum;
                    w10 = fs;
                    w01 = ft;
                    w11 = 0;
                } else {
                    w00 = 0;
                    w11 = sum - 32;
                    w01 = 32 - fs;
                    w10 = 32 - ft;
                }
                for (k = 0; k < 4; k++) {
                    o[k] = (uint8_t)((c00[k] * w00 + c10[k] * w10 + c01[k] * w01 + c11[k] * w11 + 16) >> 5);
                }
            }
        }
        if (ps == 1) {
            for (cy = 0; cy < ch; cy += 2) {
                const int b = cy >> 1;
                pf2_rows(pf + (size_t)cy * cw, pf + (size_t)(cy + 1) * cw, pp + (size_t)b * pw * 4, pp + (size_t)(b + 1) * pw * 4,
                         pw - 1);
            }
        }
        *PF = pf;
        *PFB = (uint32_t)((size_t)cw * (size_t)ch * 4u);
        gx_tex_acct((int64_t)*PFB); /* (conta na cache de texturas) */
        *PFW = cw;
        *PFKEY = tx->pkey;
        gx_stN[2]++;
        gx_stNs[2] += gx_now_ns() - t0;
    }
    u->pf = *PF;
    u->pfw = *PFW;
    u->pfs = ps;
    return 1;
}

static int tex_prefilter(FastTex *u) {
    return tex_prefilter_x(u, 0);
}

/* le os 8 texels dos enderecos em A0/A1 e grava em out[0..7]: os enderecos saem dos registradores NEON
 * de 2 em 2 (vmov), sem passar pela pilha (o compilador passava tudo pela pilha duas vezes) */
static inline __attribute__((always_inline)) void gather8(uint32x4_t A0, uint32x4_t A1, uint32_t *out) {
    uint32_t r0, r1, r2, r3;
    __asm__ __volatile__("vmov %[r0], %[r1], %e[a]\n\t"
                         "vmov %[r2], %[r3], %f[a]\n\t"
                         "ldr %[r0], [%[r0]]\n\t"
                         "ldr %[r1], [%[r1]]\n\t"
                         "ldr %[r2], [%[r2]]\n\t"
                         "ldr %[r3], [%[r3]]\n\t"
                         "str %[r0], [%[o]]\n\t"
                         "str %[r1], [%[o], #4]\n\t"
                         "str %[r2], [%[o], #8]\n\t"
                         "str %[r3], [%[o], #12]\n\t"
                         "vmov %[r0], %[r1], %e[b]\n\t"
                         "vmov %[r2], %[r3], %f[b]\n\t"
                         "ldr %[r0], [%[r0]]\n\t"
                         "ldr %[r1], [%[r1]]\n\t"
                         "ldr %[r2], [%[r2]]\n\t"
                         "ldr %[r3], [%[r3]]\n\t"
                         "str %[r0], [%[o], #16]\n\t"
                         "str %[r1], [%[o], #20]\n\t"
                         "str %[r2], [%[o], #24]\n\t"
                         "str %[r3], [%[o], #28]\n\t"
                         : [r0] "=&r"(r0), [r1] "=&r"(r1), [r2] "=&r"(r2), [r3] "=&r"(r3)
                         : [a] "w"(A0), [b] "w"(A1), [o] "r"(out)
                         : "memory");
}

/* Amostra 8*ng pixels (grupos de 8 seguidos em S/T/out). cS/cT: 1 = eixo preso (indice entre -1 e clip-1;
 * a borda de -1 e a de clip repetem a ponta); 0 = repete (ou espelho desdobrado): indice & (periodo - 1).
 * As constantes ficam em registradores durante todos os grupos; os pesos do filtro sao calculados em bytes
 * (8 pixels por instrucao) e espalhados para os 4 canais com vtbl. */
static const uint8_t k_wsel[4][8] = { { 0, 0, 0, 0, 1, 1, 1, 1 }, { 2, 2, 2, 2, 3, 3, 3, 3 },
                                      { 4, 4, 4, 4, 5, 5, 5, 5 }, { 6, 6, 6, 6, 7, 7, 7, 7 } };
static inline __attribute__((always_inline)) void sampn_tpl(const FastTex *u, const float *restrict S,
                                                            const float *restrict T, uint32_t *restrict out, int ng,
                                                            const int cS, const int cT, const int bil) {
    const uint32x4_t vpp = vdupq_n_u32((uint32_t)(uintptr_t)u->pp);
    const float32x4_t vsS = vdupq_n_f32(u->scaleS), voS = vdupq_n_f32(u->offS);
    const float32x4_t vsT = vdupq_n_f32(u->scaleT), voT = vdupq_n_f32(u->offT);
    const int32x4_t mS = vdupq_n_s32(cS ? u->pcS : u->pmS), mT = vdupq_n_s32(cT ? u->pcT : u->pmT);
    const int32x4_t m1 = vdupq_n_s32(-1);
    const int pw = u->pw;
    const int32x4_t org = vdupq_n_s32(pw + 1);
    const uint8x8_t v32 = vdup_n_u8(32), v31 = vdup_n_u8(31);
    const uint16x8_t rnd = vdupq_n_u16(16);
    const uint8x8_t sel0 = vld1_u8(k_wsel[0]), sel1 = vld1_u8(k_wsel[1]), sel2 = vld1_u8(k_wsel[2]), sel3 = vld1_u8(k_wsel[3]);
    int g;
    for (g = 0; g < ng; g++, S += 8, T += 8, out += 8) {
        uint32_t ad[8];
        int32x4_t si0 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(S), vsS), voS));
        int32x4_t si1 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(S + 4), vsS), voS));
        int32x4_t ti0 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(T), vsT), voT));
        int32x4_t ti1 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(T + 4), vsT), voT));
        int32x4_t a0 = vshrq_n_s32(si0, 5), a1 = vshrq_n_s32(si1, 5);
        int32x4_t b0 = vshrq_n_s32(ti0, 5), b1 = vshrq_n_s32(ti1, 5);
        if (cS) {
            a0 = vminq_s32(vmaxq_s32(a0, m1), mS);
            a1 = vminq_s32(vmaxq_s32(a1, m1), mS);
        } else {
            a0 = vandq_s32(a0, mS);
            a1 = vandq_s32(a1, mS);
        }
        if (cT) {
            b0 = vminq_s32(vmaxq_s32(b0, m1), mT);
            b1 = vminq_s32(vmaxq_s32(b1, m1), mT);
        } else {
            b0 = vandq_s32(b0, mT);
            b1 = vandq_s32(b1, mT);
        }
        if (!bil) {
            gather8(vaddq_u32(vpp, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_n_s32(vaddq_s32(a0, org), b0, pw), 2))),
                    vaddq_u32(vpp, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_n_s32(vaddq_s32(a1, org), b1, pw), 2))), out);
            continue;
        }
        vst1q_u32(ad, vaddq_u32(vpp, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_n_s32(vaddq_s32(a0, org), b0, pw), 2))));
        vst1q_u32(ad + 4, vaddq_u32(vpp, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_n_s32(vaddq_s32(a1, org), b1, pw), 2))));
        {
            /* fracoes (5 bits) dos 8 pixels em bytes */
            uint8x8_t fs = vand_u8(vmovn_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(si0)), vmovn_u32(vreinterpretq_u32_s32(si1)))), v31);
            uint8x8_t ft = vand_u8(vmovn_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(ti0)), vmovn_u32(vreinterpretq_u32_s32(ti1)))), v31);
            uint8x8_t sum = vadd_u8(fs, ft);
            uint8x8_t up = vcge_u8(sum, v32);
            /* embaixo (soma < 32): c00 = 32-fs-ft, c10 = fs, c01 = ft; em cima: c11 = fs+ft-32, c01 = 32-fs, c10 = 32-ft */
            uint8x8_t w00 = vqsub_u8(v32, sum), w11 = vqsub_u8(sum, v32);
            uint8x8_t w10 = vbsl_u8(up, vsub_u8(v32, ft), fs), w01 = vbsl_u8(up, vsub_u8(v32, fs), ft);
            /* 4 pares de pixels: por par, [c00,c10] da linha b e [c01,c11] da linha b+1 de cada pixel,
             * vtrn junta o mesmo texel dos 2 pixels; pesos espalhados por vtbl; soma, +16, >>5.
             * Em assembly: o GCC 8 guardava constantes na pilha e copiava registradores a toa. */
            {
                const uint32_t *adp = ad;
                uint32_t *op = out;
                const int pw4 = pw * 4;
                int t0, t1;
#define SPAIR(OFS, SEL)                                                              \
                "ldr %[t0], [%[ad], #" #OFS "]\n\t"                                  \
                "ldr %[t1], [%[ad], #" #OFS "+4]\n\t"                                \
                "vld1.32 {d16}, [%[t0]], %[pw4]\n\t"                                 \
                "vld1.32 {d17}, [%[t1]], %[pw4]\n\t"                                 \
                "vld1.32 {d18}, [%[t0]]\n\t"                                         \
                "vld1.32 {d19}, [%[t1]]\n\t"                                         \
                "vtbl.8 d20, {%P[w00]}, %P[" SEL "]\n\t"                             \
                "vtbl.8 d21, {%P[w10]}, %P[" SEL "]\n\t"                             \
                "vtrn.32 d16, d17\n\t"                                               \
                "vtbl.8 d22, {%P[w01]}, %P[" SEL "]\n\t"                             \
                "vtbl.8 d23, {%P[w11]}, %P[" SEL "]\n\t"                             \
                "vtrn.32 d18, d19\n\t"                                               \
                "vmull.u8 q12, d16, d20\n\t"                                         \
                "vmlal.u8 q12, d17, d21\n\t"                                         \
                "vmlal.u8 q12, d18, d22\n\t"                                         \
                "vmlal.u8 q12, d19, d23\n\t"                                         \
                "vadd.i16 q12, q12, %q[rnd]\n\t"                                     \
                "vshrn.i16 d24, q12, #5\n\t"                                         \
                "vst1.8 {d24}, [%[op]]!\n\t"
                __asm__ __volatile__(SPAIR(0, "s0") SPAIR(8, "s1") SPAIR(16, "s2") SPAIR(24, "s3")
                                     : [op] "+r"(op), [t0] "=&r"(t0), [t1] "=&r"(t1)
                                     : [ad] "r"(adp), [pw4] "r"(pw4), [w00] "w"(w00), [w10] "w"(w10), [w01] "w"(w01),
                                       [w11] "w"(w11), [s0] "w"(sel0), [s1] "w"(sel1), [s2] "w"(sel2), [s3] "w"(sel3),
                                       [rnd] "w"(rnd)
                                     : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "memory");
#undef SPAIR
            }
        }
    }
}
/* amostra 8*ng pixels pelo filtro pre-calculado: celula = N * (texel + 1) + (fracao >> (5 - pfs)) */
static inline __attribute__((always_inline)) void sampf_tpl(const FastTex *u, const float *restrict S,
                                                            const float *restrict T, uint32_t *restrict out, int ng,
                                                            const int cS, const int cT, const int PFS) {
    const uint32x4_t vpf = vdupq_n_u32((uint32_t)(uintptr_t)u->pf);
    const float32x4_t vsS = vdupq_n_f32(u->scaleS), voS = vdupq_n_f32(u->offS);
    const float32x4_t vsT = vdupq_n_f32(u->scaleT), voT = vdupq_n_f32(u->offT);
    const int32x4_t mS = vdupq_n_s32(cS ? u->pcS : u->pmS), mT = vdupq_n_s32(cT ? u->pcT : u->pmT);
    const int32x4_t m1 = vdupq_n_s32(-1), one = vdupq_n_s32(1);
    const int32x4_t vpfw = vdupq_n_s32(u->pfw);
    int g;
    for (g = 0; g < ng; g++, S += 8, T += 8, out += 8) {
        int32x4_t si0 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(S), vsS), voS));
        int32x4_t si1 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(S + 4), vsS), voS));
        int32x4_t ti0 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(T), vsT), voT));
        int32x4_t ti1 = nfloor(vsubq_f32(vmulq_f32(vld1q_f32(T + 4), vsT), voT));
        int32x4_t a0 = vshrq_n_s32(si0, 5), a1 = vshrq_n_s32(si1, 5);
        int32x4_t b0 = vshrq_n_s32(ti0, 5), b1 = vshrq_n_s32(ti1, 5);
        if (cS) {
            a0 = vminq_s32(vmaxq_s32(a0, m1), mS);
            a1 = vminq_s32(vmaxq_s32(a1, m1), mS);
        } else {
            a0 = vandq_s32(a0, mS);
            a1 = vandq_s32(a1, mS);
        }
        if (cT) {
            b0 = vminq_s32(vmaxq_s32(b0, m1), mT);
            b1 = vminq_s32(vmaxq_s32(b1, m1), mT);
        } else {
            b0 = vandq_s32(b0, mT);
            b1 = vandq_s32(b1, mT);
        }
        /* celula: x = N*(a+1) + ((si >> (5 - pfs)) & (N - 1)), y igual; endereco = pf + 4*(y*pfw + x) */
        /* PFS fixo em cada versao: deslocamentos imediatos; vsli = ((a+1) << PFS) | (bits baixos da fracao) */
        a0 = vsliq_n_s32(vshrq_n_s32(si0, 5 - PFS), vaddq_s32(a0, one), PFS);
        a1 = vsliq_n_s32(vshrq_n_s32(si1, 5 - PFS), vaddq_s32(a1, one), PFS);
        b0 = vsliq_n_s32(vshrq_n_s32(ti0, 5 - PFS), vaddq_s32(b0, one), PFS);
        b1 = vsliq_n_s32(vshrq_n_s32(ti1, 5 - PFS), vaddq_s32(b1, one), PFS);
        {
            gather8(vaddq_u32(vpf, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_s32(a0, b0, vpfw), 2))),
                    vaddq_u32(vpf, vreinterpretq_u32_s32(vshlq_n_s32(vmlaq_s32(a1, b1, vpfw), 2))), out);
        }
    }
}
#define SFFN(cs, ct, ps)                                                                                        \
    static void sampf_##cs##ct##ps(const FastTex *u, const float *S, const float *T, uint32_t *out, int ng) { \
        sampf_tpl(u, S, T, out, ng, cs, ct, ps);                                                                \
    }
SFFN(0, 0, 1) SFFN(0, 1, 1) SFFN(1, 0, 1) SFFN(1, 1, 1) SFFN(0, 0, 2) SFFN(0, 1, 2) SFFN(1, 0, 2) SFFN(1, 1, 2)
SFFN(0, 0, 3) SFFN(0, 1, 3) SFFN(1, 0, 3) SFFN(1, 1, 3)
/* [celulas por texel: 1 = 2x2, 2 = 4x4][S preso][T preso] */
static const SampNFn g_sampf[4][2][2] = { { { NULL, NULL }, { NULL, NULL } },
                                          { { sampf_001, sampf_011 }, { sampf_101, sampf_111 } },
                                          { { sampf_002, sampf_012 }, { sampf_102, sampf_112 } },
                                          { { sampf_003, sampf_013 }, { sampf_103, sampf_113 } } };

#define SNFN(cs, ct, bl)                                                                                    \
    static void sampn_##cs##ct##bl(const FastTex *u, const float *S, const float *T, uint32_t *out, int ng) { \
        sampn_tpl(u, S, T, out, ng, cs, ct, bl);                                                           \
    }
SNFN(0, 0, 0) SNFN(0, 1, 0) SNFN(1, 0, 0) SNFN(1, 1, 0) SNFN(0, 0, 1) SNFN(0, 1, 1) SNFN(1, 0, 1) SNFN(1, 1, 1)
/* [filtro][S preso][T preso] */
static const SampNFn g_sampn[2][2][2] = {
    { { sampn_000, sampn_010 }, { sampn_100, sampn_110 } },
    { { sampn_001, sampn_011 }, { sampn_101, sampn_111 } },
};
#endif

typedef struct {
    uint8_t kind, dst, a, b, c, d; /* kind: 0 copia d, 1 a*c (b = d = 0), 2 (a - b) * c + d */
} FusedOp;
#ifdef GX_REPLAY_DIAG
static void diag_ops(const FastProg *f) { /* (diagnostico: as operacoes do combinador de um programa) */
    {
                const FusedOp *o = (const FusedOp *)f->fops;
                int q;
                fprintf(stderr, "[prog]   premix %d | fcc %d %d %d %d | %d ops:", g_premixTx != NULL, f->fcc[0], f->fcc[1],
                        f->fcc[2], f->fcc[3], f->nfops);
                for (q = 0; q < f->nfops; q++) {
                    fprintf(stderr, " [k%x d%d %d %d %d %d]", o[q].kind, o[q].dst, o[q].a, o[q].b, o[q].c, o[q].d);
                }
                fprintf(stderr, " | c0 %d %d %d %d / %d %d %d %d | prim %d %d %d %d env %d\n", f->c0[0][0], f->c0[0][1],
                        f->c0[0][2], f->c0[0][3], f->c0[3][0], f->c0[3][1], f->c0[3][2], f->c0[3][3], gx.prim[0],
                        gx.prim[1], gx.prim[2], gx.prim[3], gx.env[3]);
            }
}
#endif

/* Junta 3 operacoes seguidas que fazem a mesma conta nos canais r, g, b (destino e entradas em indices
 * seguidos; o C pode ser o mesmo nos 3) numa so: o caminho fundido decide o tipo uma vez para os 3.
 * kind |= FOP_RGB (3 canais), FOP_C1 (C igual nos 3). Nao junta se um canal le o que outro escreveu. */
#define FOP_RGB 0x10
#define FOP_C1 0x20
static int fused_merge(FusedOp *o, int n) {
    int i = 0, m = 0;
    while (i < n) {
        if (i + 2 < n && o[i].kind == o[i + 1].kind && o[i].kind == o[i + 2].kind) {
            const FusedOp *x = &o[i];
            int ok = 1, k, cs = -1;
            for (k = 1; k < 3; k++) {
                const FusedOp *y = &o[i + k];
                ok &= y->dst == x->dst + k && y->d == x->d + (x->kind == 1 ? 0 : k);
                if (x->kind == 2) {
                    ok &= y->a == x->a + k && y->b == x->b + k;
                } else if (x->kind == 1) {
                    ok &= y->a == x->a + k;
                }
            }
            if (x->kind != 0) {
                if (o[i + 1].c == x->c && o[i + 2].c == x->c) {
                    cs = 0;
                } else if (o[i + 1].c == x->c + 1 && o[i + 2].c == x->c + 2) {
                    cs = 1;
                } else {
                    ok = 0;
                }
            }
            /* nenhuma entrada de um canal pode ser o destino de outro canal do trio */
            for (k = 0; k < 3 && ok; k++) {
                const FusedOp *y = &o[i + k];
                int d2;
                for (d2 = 0; d2 < 3; d2++) {
                    int dd = x->dst + d2;
                    if (d2 == k) continue;
                    if (y->a == dd || y->b == dd || y->c == dd || y->d == dd) ok = 0;
                }
            }
            if (ok) {
                o[m] = *x;
                o[m].kind = (uint8_t)(x->kind | FOP_RGB | (cs == 0 ? FOP_C1 : 0));
                m++;
                i += 3;
                continue;
            }
        }
        o[m++] = o[i++];
    }
    return m;
}

static inline void fused_op(FusedOp *o, int dst, const uint8_t *c) {
    o->dst = (uint8_t)dst;
    o->a = c[0];
    o->b = c[1];
    o->c = c[2];
    o->d = c[3];
    o->kind = (c[1] == V_ZERO && c[3] == V_ZERO) ? 1 : 2;
}

/* formas comuns do blender no caminho fundido (o resto usa a forma geral) */
enum {
    BLM_GERAL,
    BLM_COR,     /* 1 ciclo sem mistura: sai a cor do combinador */
    BLM_NEBLINA, /* 2 ciclos: neblina com o alfa do sombreamento e nada no 2o ciclo */
    BLM_NEB_ALFA /* 2 ciclos: neblina e depois mistura com a memoria pelo alfa (transparentes) */
};

/* O blender so repassa a cor do combinador? Cada ciclo que se reduz a uma selecao (sem mistura, ou com
 * A = 0 / B = 0) e a cadeia termina na cor do combinador: sai cc (BLM_COR). */
static int bl_red(const FastProg *f, int c) {
    int blends = (f->cyc2 && c == 0) || f->forceBl;
    if (!blends) {
        return f->bl[c][0];
    }
    if (f->bl[c][1] == 3) {
        return f->bl[c][3] == 3 ? f->bl[c][0] : f->bl[c][2];
    }
    if (f->bl[c][3] == 3) {
        return f->bl[c][0];
    }
    return -1;
}
static int bl_pass(const FastProg *f) {
    if (!f->cyc2) {
        return bl_red(f, 0) == 0;
    }
    return bl_red(f, 1) == 0 && bl_red(f, 0) == 0;
}

/* Reconhece as formas mais comuns do combinador (as operacoes de cor ja juntadas em trios RGB):
 *   KERN_MUL1     a*c                       (ex.: textura * cor primitiva, HUD e imagens 2D)
 *   KERN_MUL2     (a*c) * c2                (ex.: textura * sombreamento * cor primitiva: o chao, as paredes)
 *   KERN_LERP     (a-b)*c + d               (ex.: o ceu: mistura de 2 texturas)
 *   KERN_LERP_MUL ((a-b)*c + d) * c2        (ex.: terreno com 2 texturas * sombreamento)
 * As operacoes so de alfa ficam numa lista a parte e passam pelo interpretador. Mesmas contas. */
enum {
    KERN_GEN,
    KERN_T0_SH_K,   /* textura * sombreamento * constante: o mais comum (chao, paredes, personagens) */
    KERN_T0_K,      /* textura * constante (imagens 2D, HUD) */
    KERN_T0_SH,     /* textura * sombreamento */
    KERN_LERP_TT,   /* mistura de 2 texturas por um alfa constante (o ceu) */
    KERN_LERP_TT_SH,/* mistura de 2 texturas * sombreamento (terreno) */
    KERN_KK_T0_SH,  /* (constante - constante) * textura + constante, * sombreamento */
    KERN_K,         /* cor constante (o alfa e que varia) */
    KERN_KK_T0,     /* (constante - constante) * textura + constante: fogo/brilhos/efeitos de impacto (lote 123) */
    KERN_TKT_KKK    /* 2 ciclos: (T1 - K) * k + T0, depois (K - K) * isso + K (multitextura com cor prim/env) */
};
/* de onde vem cada entrada de um kernel: cor (3 canais) da textura 0/1 ou do sombreamento, constante
 * (3 canais ou um so), ou um alfa por pixel (textura 0/1, sombreamento) usado nos 3 canais */
enum { KS_T0, KS_T1, KS_SH, KS_K3, KS_K1, KS_T0A, KS_T1A, KS_SHA, KS_BAD };
enum { KA_R, KA_T0A, KA_K, KA_T0A_K, KA_TT_K }; /* KA_R: o alfa sai de R[fcc[3]] (interpretador ou constante ja em R)
                                                 * KA_T0A_K: T0.a * k; KA_TT_K: ((T1.a - k) * k + T0.a) * k */
static int kern_cls(int idx, int stride) {
    if (stride) {
        if (idx == V_T0) return KS_T0;
        if (idx == V_T1) return KS_T1;
        if (idx == V_SHADE) return KS_SH;
        if (idx == V_PRIM || idx == V_ENV) return KS_K3;
        return KS_BAD;
    }
    if (idx == V_T0 + 3) return KS_T0A;
    if (idx == V_T1 + 3) return KS_T1A;
    if (idx == V_SHADE + 3) return KS_SHA;
    if ((idx >= V_PRIM && idx < V_PRIM + 4) || (idx >= V_ENV && idx < V_ENV + 4) || (idx >= V_LODF && idx <= V_NOISE)) {
        return KS_K1;
    }
    return KS_BAD;
}
int gx_kern = 1;
/* alfa dos kernels: formas conhecidas saem direto; as outras pelo interpretador (fopsA) */
static void fused_kern_alpha(FastProg *f, const FusedOp *aop0, int na) {
    FusedOp aop[4];
    int i;
    f->nfopsA = na;
    f->kAcls = KA_R;
    f->kAi[0] = f->kAi[1] = f->kAi[2] = V_ZERO;
    memcpy(aop, aop0, (size_t)na * sizeof aop[0]);
    /* formas equivalentes: produto com a textura no 2o operando; copia de T0.a lida so pela operacao seguinte */
    for (i = 0; i < na; i++) {
        if (aop[i].kind == 1 && aop[i].c == V_T0 + 3 && aop[i].a != V_T0 + 3) {
            aop[i].c = aop[i].a;
            aop[i].a = V_T0 + 3;
        }
    }
    if (na == 2 && aop[0].kind == 0 && aop[0].d == V_T0 + 3 && aop[1].a == aop[0].dst && aop[1].kind == 1 &&
        aop[1].c != aop[0].dst && f->fcc[3] == aop[1].dst) {
        aop[1].a = V_T0 + 3;
        aop[0] = aop[1];
        na = 1;
    }
    /* alfa = copia simples (T0.a ou constante) lida pelo resultado final: sem interpretador */
    if (na == 1 && aop[0].kind == 0 && f->fcc[3] == aop[0].dst) {
        int d = aop[0].d;
        if (d == V_T0 + 3 && f->useT[0]) {
            f->kAcls = KA_T0A;
            f->nfopsA = 0;
        } else if (kern_cls(d, 0) == KS_K1) {
            f->kAcls = KA_K;
            f->kAidx = (uint8_t)d;
            f->nfopsA = 0;
        }
    } else if (na == 1 && aop[0].kind == 1 && f->fcc[3] == aop[0].dst && aop[0].a == V_T0 + 3 && f->useT[0] &&
               kern_cls(aop[0].c, 0) == KS_K1) {
        /* T0.a * k */
        f->kAcls = KA_T0A_K;
        f->kAi[0] = aop[0].c;
        f->nfopsA = 0;
    } else if (na == 2 && aop[0].kind == 2 && aop[1].kind == 1 && f->fcc[3] == aop[1].dst && aop[1].a == aop[0].dst &&
               aop[1].c != aop[0].dst && aop[0].a == V_T1 + 3 && aop[0].d == V_T0 + 3 && f->useT[0] && f->useT[1] &&
               kern_cls(aop[0].b, 0) == KS_K1 && kern_cls(aop[0].c, 0) == KS_K1 && kern_cls(aop[1].c, 0) == KS_K1) {
        /* ((T1.a - k) * k + T0.a) * k */
        f->kAcls = KA_TT_K;
        f->kAi[0] = aop[0].b;
        f->kAi[1] = aop[0].c;
        f->kAi[2] = aop[1].c;
        f->nfopsA = 0;
    }
}

static void fused_kern(FastProg *f) {
    const FusedOp *ops = (const FusedOp *)f->fops;
    FusedOp *aop = (FusedOp *)f->fopsA;
    const FusedOp *rgb[4];
    int i, nr = 0, na = 0, k;
    f->kern = KERN_GEN;
    f->nfopsA = 0;
    f->kAcls = KA_R;
    if (!gx_kern) {
        return;
    }
    for (i = 0; i < f->nfops; i++) {
        const FusedOp *o = &ops[i];
        if (o->kind & FOP_RGB) {
            if ((o->kind & 0xF) == 0 || nr == 2) {
                return;
            }
            rgb[nr++] = o;
        } else {
            if ((o->dst != V_COMB + 3 && o->dst != V_N + 3) || na == 4) {
                return;
            }
            aop[na++] = *o;
        }
    }
    if (nr == 0) {
        /* cor constante: os 3 canais do resultado sao uma cor primitiva/de ambiente */
        int base = f->fcc[0];
        if ((base == V_PRIM || base == V_ENV) && f->fcc[1] == base + 1 && f->fcc[2] == base + 2) {
            f->kern = KERN_K;
            f->ka0 = (uint8_t)base;
            for (k = 0; k < 8; k++) {
                f->kcls[k] = KS_BAD;
            }
            f->kcls[0] = KS_K3;
            fused_kern_alpha(f, aop, na);
        }
        return;
    }
    /* as operacoes de alfa nao podem ler resultados de cor, nem as de cor ler alfa calculado */
    for (k = 0; k < na; k++) {
        for (i = 0; i < nr; i++) {
            int lo = rgb[i]->dst, hi = rgb[i]->dst + 2;
            const FusedOp *o = &aop[k];
            if ((o->a >= lo && o->a <= hi) || (o->b >= lo && o->b <= hi) || (o->c >= lo && o->c <= hi) ||
                (o->d >= lo && o->d <= hi)) {
                return;
            }
            if (rgb[i]->a == o->dst || rgb[i]->b == o->dst || rgb[i]->c == o->dst || rgb[i]->d == o->dst) {
                return;
            }
        }
    }
    {
        const FusedOp *o0 = rgb[0], *last = rgb[nr - 1];
        int k0 = o0->kind & 0xF;
        for (k = 0; k < 3; k++) {
            if (f->fcc[k] != last->dst + k) {
                return;
            }
        }
        if (f->fcc[3] >= last->dst && f->fcc[3] <= last->dst + 2) {
            return;
        }
        for (k = 5; k < 8; k++) {
            f->kcls[k] = KS_BAD;
        }
        if (nr == 2 && (rgb[1]->kind & 0xF) == 2) {
            /* 2o ciclo (A - B) * resultado + D, com A, B, D constantes de 3 canais */
            const FusedOp *o1 = rgb[1];
            if (k0 != 2 || (o1->kind & FOP_C1) || o1->c != o0->dst || kern_cls(o1->a, 1) != KS_K3 ||
                kern_cls(o1->b, 1) != KS_K3 || kern_cls(o1->d, 1) != KS_K3 || kern_cls(o0->a, 1) != KS_T1 ||
                kern_cls(o0->b, 1) != KS_K3 || (o0->kind & FOP_C1) == 0 || kern_cls(o0->c, 0) != KS_K1 ||
                kern_cls(o0->d, 1) != KS_T0 || !f->useT[0] || !f->useT[1]) {
                return;
            }
            if (f->fcc[3] >= o0->dst && f->fcc[3] <= o0->dst + 2) {
                return;
            }
            f->ka0 = o0->a;
            f->kb0 = o0->b;
            f->kc0 = o0->c;
            f->kd0 = o0->d;
            f->ks0 = 0;
            f->ka1 = o1->a;
            f->kb1 = o1->b;
            f->kd1 = o1->d;
            f->kcls[0] = KS_T1;
            f->kcls[1] = KS_K3;
            f->kcls[2] = KS_K1;
            f->kcls[3] = KS_T0;
            f->kcls[4] = KS_BAD;
            f->kcls[5] = KS_K3;
            f->kcls[6] = KS_K3;
            f->kcls[7] = KS_K3;
            f->kern = KERN_TKT_KKK;
            fused_kern_alpha(f, aop, na);
            return;
        }
        if (nr == 2) {
            const FusedOp *o1 = rgb[1];
            if ((o1->kind & 0xF) != 1) {
                return;
            }
            if (f->fcc[3] >= o0->dst && f->fcc[3] <= o0->dst + 2) {
                return;
            }
            if (o1->a == o0->dst && !(o1->c >= o0->dst && o1->c <= o0->dst + 2)) {
                f->kc1 = o1->c;
                f->ks1 = (o1->kind & FOP_C1) ? 0 : 1;
            } else if (o1->c == o0->dst && !(o1->kind & FOP_C1) && !(o1->a >= o0->dst && o1->a <= o0->dst + 2)) {
                /* K * resultado: a multiplicacao e a mesma nos dois sentidos */
                f->kc1 = o1->a;
                f->ks1 = 1;
            } else {
                return;
            }
        }
        /* a operacao nao pode ler o proprio destino em outro canal (fused_merge ja garante) */
        f->ka0 = o0->a;
        f->kb0 = o0->b;
        f->kc0 = o0->c;
        f->kd0 = o0->d;
        f->ks0 = (o0->kind & FOP_C1) ? 0 : 1;
        f->kcls[0] = (uint8_t)kern_cls(o0->a, 1);
        f->kcls[1] = (uint8_t)(k0 == 1 ? KS_K3 : kern_cls(o0->b, 1));
        f->kcls[2] = (uint8_t)kern_cls(o0->c, f->ks0);
        f->kcls[3] = (uint8_t)(k0 == 1 ? KS_K3 : kern_cls(o0->d, 1));
        f->kcls[4] = (uint8_t)(nr == 2 ? kern_cls(f->kc1, f->ks1) : KS_K3);
        for (k = 0; k < 5; k++) {
            if (f->kcls[k] == KS_BAD) {
                return;
            }
        }
        {
            const uint8_t *c = f->kcls;
            int kk = KERN_GEN;
            if (k0 == 1 && nr == 2 && c[0] == KS_T0 && c[2] == KS_SH && c[4] == KS_K3) {
                kk = KERN_T0_SH_K;
            } else if (k0 == 1 && nr == 1 && c[0] == KS_T0 && c[2] == KS_K3) {
                kk = KERN_T0_K;
            } else if (k0 == 1 && nr == 1 && c[0] == KS_T0 && c[2] == KS_SH) {
                kk = KERN_T0_SH;
            } else if (k0 == 2 && c[0] == KS_T1 && c[1] == KS_T0 && c[2] == KS_K1 && c[3] == KS_T0) {
                kk = nr == 1 ? KERN_LERP_TT : (c[4] == KS_SH ? KERN_LERP_TT_SH : KERN_GEN);
            } else if (k0 == 2 && nr == 2 && c[0] == KS_K3 && c[1] == KS_K3 && c[2] == KS_T0 && c[3] == KS_K3 &&
                       c[4] == KS_SH) {
                kk = KERN_KK_T0_SH;
            } else if (k0 == 2 && nr == 1 && c[0] == KS_K3 && c[1] == KS_K3 && c[2] == KS_T0 && c[3] == KS_K3) {
                kk = KERN_KK_T0;
            }
            if (kk == KERN_GEN) {
                return;
            }
            /* textura que o combinador le mas nao e amostrada (a referencia le 0): fica com o interpretador */
            for (k = 0; k < 5; k++) {
                if (((c[k] == KS_T0 || c[k] == KS_T0A) && !f->useT[0]) || ((c[k] == KS_T1 || c[k] == KS_T1A) && !f->useT[1])) {
                    return;
                }
            }
            f->kern = kk;
        }
        fused_kern_alpha(f, aop, na);
    }
}

/* Memoria da montagem do caminho fundido: o jogo troca muito de estado (cor primitiva, textura...) sem
 * trocar a forma do combinador e do blender; a montagem das operacoes so depende destas entradas. */
typedef struct {
    uint8_t c0[4][4], c1[4][4], k0[4], k1[4], need0[4], need1[4];
    int cyc2, bl[2][4], forceBl, useT[2], needShade[4];
    uint32_t needV;
} FusedKey;
typedef struct {
    FusedKey key;
    int valid;
    uint8_t fops[8][6], fcc[4], fshk[4], kcls[8], kAcls, kAidx, kAi[3], fopsA[4][6];
    uint8_t ka0, kb0, kc0, kd0, ks0, kc1, ks1, ka1, kb1, kd1;
    int nfops, fnsh, kern, nfopsA, fblm;
    uint32_t fzero;
} FusedMemo;
#define FUSED_MEMO_N 32
static FusedMemo g_fmemo[FUSED_MEMO_N];
static int g_fmemoNext;
int gx_fmemo = 1;
static void fused_key(const FastProg *f, FusedKey *k) {
    memset(k, 0, sizeof *k);
    memcpy(k->c0, f->c0, sizeof k->c0);
    memcpy(k->c1, f->c1, sizeof k->c1);
    memcpy(k->k0, f->k0, 4);
    memcpy(k->k1, f->k1, 4);
    memcpy(k->need0, f->need0, 4);
    memcpy(k->need1, f->need1, 4);
    k->cyc2 = f->cyc2;
    memcpy(k->bl, f->bl, sizeof k->bl);
    k->forceBl = f->forceBl;
    k->useT[0] = f->useT[0];
    k->useT[1] = f->useT[1];
    memcpy(k->needShade, f->needShade, sizeof k->needShade);
    k->needV = f->needV;
}
static int fused_memo_get(FastProg *f) {
    FusedKey k;
    int i;
    if (!gx_fmemo) {
        return 0;
    }
    fused_key(f, &k);
    for (i = 0; i < FUSED_MEMO_N; i++) {
        const FusedMemo *m = &g_fmemo[i];
        if (m->valid && !memcmp(&m->key, &k, sizeof k)) {
            memcpy(f->fops, m->fops, sizeof f->fops);
            memcpy(f->fcc, m->fcc, sizeof f->fcc);
            memcpy(f->fshk, m->fshk, sizeof f->fshk);
            memcpy(f->kcls, m->kcls, sizeof f->kcls);
            memcpy(f->kAi, m->kAi, sizeof f->kAi);
            memcpy(f->fopsA, m->fopsA, sizeof f->fopsA);
            f->kAcls = m->kAcls;
            f->kAidx = m->kAidx;
            f->ka0 = m->ka0; f->kb0 = m->kb0; f->kc0 = m->kc0; f->kd0 = m->kd0; f->ks0 = m->ks0;
            f->kc1 = m->kc1; f->ks1 = m->ks1; f->ka1 = m->ka1; f->kb1 = m->kb1; f->kd1 = m->kd1;
            f->nfops = m->nfops;
            f->fnsh = m->fnsh;
            f->kern = m->kern;
            f->nfopsA = m->nfopsA;
            f->fblm = m->fblm;
            f->fzero = m->fzero;
            return 1;
        }
    }
    return 0;
}
static void fused_memo_put(const FastProg *f) {
    FusedMemo *m;
    if (!gx_fmemo) {
        return;
    }
    m = &g_fmemo[g_fmemoNext];
    g_fmemoNext = (g_fmemoNext + 1) % FUSED_MEMO_N;
    fused_key(f, &m->key);
    memcpy(m->fops, f->fops, sizeof m->fops);
    memcpy(m->fcc, f->fcc, sizeof m->fcc);
    memcpy(m->fshk, f->fshk, sizeof m->fshk);
    memcpy(m->kcls, f->kcls, sizeof m->kcls);
    memcpy(m->kAi, f->kAi, sizeof m->kAi);
    memcpy(m->fopsA, f->fopsA, sizeof m->fopsA);
    m->kAcls = f->kAcls;
    m->kAidx = f->kAidx;
    m->ka0 = f->ka0; m->kb0 = f->kb0; m->kc0 = f->kc0; m->kd0 = f->kd0; m->ks0 = f->ks0;
    m->kc1 = f->kc1; m->ks1 = f->ks1; m->ka1 = f->ka1; m->kb1 = f->kb1; m->kd1 = f->kd1;
    m->nfops = f->nfops;
    m->fnsh = f->fnsh;
    m->kern = f->kern;
    m->nfopsA = f->nfopsA;
    m->fblm = f->fblm;
    m->fzero = f->fzero;
    m->valid = 1;
}

static void fused_check(FastProg *f) {
    int ok = 0;
    f->kern = KERN_GEN;
    f->kAcls = KA_R;
    f->nfopsA = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    int ch, i, c, k;
    ok = gx_fused;
    /* no 2o ciclo, o 1o ciclo lendo COMBINED (raro) fica com o caminho antigo */
    if (f->cyc2) {
        for (ch = 0; ch < 4; ch++) {
            if (!f->need0[ch]) {
                continue;
            }
            for (i = f->k0[ch] ? 3 : 0; i < 4; i++) {
                if (f->c0[ch][i] >= V_COMB && f->c0[ch][i] < V_COMB + 4) {
                    ok = 0;
                }
            }
        }
    }
    /* blender: a divisao pela soma (B = alfa da memoria ou 1) fica com o caminho antigo */
    for (c = 0; c < (f->cyc2 ? 2 : 1); c++) {
        int blends = (f->cyc2 && c == 0) || f->forceBl;
        if (blends && f->bl[c][1] != 3 && (f->bl[c][3] == 1 || f->bl[c][3] == 2)) {
            ok = 0;
        }
    }
    if (f->ac != 0 && f->ac != 1 && f->ac != 3) {
        ok = 0;
    }
    for (k = 0; k < 2; k++) {
        f->sampn[k] = NULL;
        f->sampnF[k] = NULL;
        f->fExact[k] = 0;
        if (!f->useT[k] || f->t[k].px == NULL) {
            continue;
        }
        if (f->t[k].modeS > 2 || f->t[k].modeT > 2) {
            ok = 0;
        } else if (ok) {
            tex_pad(&f->t[k]);
            if (f->t[k].pp == NULL) {
                ok = 0;
            } else {
                f->sampn[k] = g_sampn[f->bilerp && !f->t[k].hdPoint ? 1 : 0][f->t[k].modeS == 2][f->t[k].modeT == 2];
                if (gx_tex_prefilter && f->bilerp && !f->t[k].hdPoint && tex_prefilter(&f->t[k])) {
                    f->sampn[k] = g_sampf[f->t[k].pfs >= 1 && f->t[k].pfs <= 3 ? f->t[k].pfs : 1][f->t[k].modeS == 2][f->t[k].modeT == 2];
                    if (gx_filtro_exato_perto > 0) {
                        /* de perto: o filtro exato da mesma textura (hd_close decide por triangulo) */
                        f->tF[k] = f->t[k];
                        f->sampnF[k] = g_sampn[1][f->t[k].modeS == 2][f->t[k].modeT == 2];
                        f->fExact[k] = 1;
                    }
                }
                if (f->t[k].hdPoint && gx_hd_perto > 0) {
                    /* versao com filtro (sem o arredondamento do texel mais proximo); a tabela filtrada so e
                     * feita quando um triangulo precisar (hd_close) */
                    f->tF[k] = f->t[k];
                    f->tF[k].hdPoint = 0;
                    f->tF[k].offS += 16.0f;
                    f->tF[k].offT += 16.0f;
                    f->tF[k].pf = NULL;
                    f->sampnF[k] = g_sampn[1][f->t[k].modeS == 2][f->t[k].modeT == 2];
                }
            }
        }
    }
    if (ok && fused_memo_get(f)) {
        /* (mesmas entradas de um programa recente: resultado guardado) */
    } else if (ok) {
        uint32_t perPix = 0;
        FusedOp *ops = (FusedOp *)f->fops;
        int n = 0;
        f->fnsh = 0;
        for (k = 0; k < 4; k++) {
            if (f->needShade[k]) {
                f->fshk[f->fnsh++] = (uint8_t)k;
                perPix |= 1u << (V_SHADE + k);
            }
        }
        if (f->useT[0]) perPix |= 0xFu << V_T0;
        if (f->useT[1]) perPix |= 0xFu << V_T1;
        /* Lote 190: canal do 1o ciclo que so copia uma entrada (k0: (A - A) * C + D ou C = 0) nao vira operacao: o 2o
         * ciclo le direto a entrada (a copia e o "COMBINED" deixam de existir). Era o caso do ceu e das texturas
         * pre-misturadas (agua, chao com reflexo): sem isso a copia levava o pixel inteiro para o interpretador
         * (KERN_GEN) em vez de um kernel pronto. Mesmas contas. */
        uint8_t alias[V_N + 4];
        for (k = 0; k < V_N + 4; k++) {
            alias[k] = (uint8_t)k;
        }
        if (f->cyc2) {
            for (k = 0; k < 4; k++) {
                const uint8_t *c = f->c0[k];
                if (f->need0[k]) {
                    if (f->k0[k] && gx_cc_alias && !(c[3] >= V_COMB && c[3] < V_COMB + 4)) {
                        alias[V_COMB + k] = c[3];
                    } else if (f->k0[k]) {
                        perPix |= 1u << (V_COMB + k);
                        ops[n].kind = 0;
                        ops[n].dst = (uint8_t)(V_COMB + k);
                        ops[n].a = ops[n].b = ops[n].c = V_ZERO;
                        ops[n].d = c[3];
                        n++;
                    } else {
                        perPix |= 1u << (V_COMB + k);
                        fused_op(&ops[n++], V_COMB + k, c);
                    }
                }
            }
        }
        for (k = 0; k < 4; k++) {
            uint8_t c[4];
            int i2;
            for (i2 = 0; i2 < 4; i2++) {
                c[i2] = alias[f->c1[k][i2]];
            }
            if (!f->need1[k]) {
                f->fcc[k] = V_ZERO;
            } else if (f->k1[k]) {
                f->fcc[k] = c[3];
            } else {
                fused_op(&ops[n++], V_N + k, c);
                f->fcc[k] = (uint8_t)(V_N + k);
            }
        }
        f->nfops = fused_merge(ops, n);
        /* zeradas: tudo o que pode ser lido e nao e calculado por pixel nem constante (as constantes sao
         * escritas depois, por cima) + o alfa do sombreamento (o blender pode le-lo) + COMBINED nao usado */
        f->fzero = (f->needV | (1u << V_ZERO) | (0xFu << V_COMB) | (1u << (V_SHADE + 3))) & ~perPix;
        fused_kern(f);
        if (f->kern != KERN_GEN && f->nfopsA == 0 && f->kAcls == KA_R && f->need1[3]) {
            /* alfa final copiado direto de uma entrada por pixel (textura ou COMBINED), sem operacao: com um kernel
             * de cor os registradores das texturas e o COMBINED nao sao preenchidos e o alfa saia 0 (as asas da Navi
             * sumiam com a simplificacao do combinador, lote 120). T0.a tem kernel proprio; o resto vai pelo
             * interpretador. */
            const int a = f->fcc[3];
            if (a == V_T0 + 3 && f->useT[0]) {
                f->kAcls = KA_T0A;
            } else if ((a >= V_T0 && a < V_T0 + 4) || (a >= V_T1 && a < V_T1 + 4) || (a >= V_COMB && a < V_COMB + 4)) {
                f->kern = KERN_GEN;
            }
        }
        f->fblm = BLM_GERAL;
        if (bl_pass(f)) {
            f->fblm = BLM_COR;
        } else if (f->cyc2 && f->bl[0][0] == 3 && f->bl[0][1] == 2 && f->bl[0][2] == 0 && f->bl[0][3] == 0) {
            if (!f->forceBl && f->bl[1][0] == 0) {
                f->fblm = BLM_NEBLINA;
            } else if (f->forceBl && f->bl[1][0] == 0 && f->bl[1][1] == 0 && f->bl[1][2] == 1 && f->bl[1][3] == 0) {
                f->fblm = BLM_NEB_ALFA;
            }
        }
        fused_memo_put(f);
    }
    {
        const int last = f->cyc2 ? 1 : 0;
        f->a0skip = gx_a0_skip && f->needMem && f->forceBl && f->bl[last][0] == 0 && f->bl[last][1] == 0 &&
                    f->bl[last][2] == 1 && f->bl[last][3] == 0 && !(f->alphaCvgSel && !f->cvgXAlpha) && !f->cdith &&
                    !f->zupd;
    }
#endif
    f->fused = ok;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static const uint8_t k_lane8[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* (a - b) * c / 256 arredondado + d, preso em 0..255 (como cc_loop) */
static inline uint8x8_t cc8(uint8x8_t a, uint8x8_t b, uint8x8_t c, uint8x8_t d) {
    uint16x8_t ac = vmull_u8(a, c), bc = vmull_u8(b, c);
    int32x4_t lo = vreinterpretq_s32_u32(vsubl_u16(vget_low_u16(ac), vget_low_u16(bc)));
    int32x4_t hi = vreinterpretq_s32_u32(vsubl_u16(vget_high_u16(ac), vget_high_u16(bc)));
    int16x8_t m = vcombine_s16(vrshrn_n_s32(lo, 8), vrshrn_n_s32(hi, 8));
    return vqmovun_s16(vaddq_s16(m, vreinterpretq_s16_u16(vmovl_u8(d))));
}

/* um canal do blender (como bl_chan; o caso da divisao pela soma nao chega aqui) */
static inline uint8x8_t bl8(int bA, int bB, uint8x8_t P, uint8x8_t M, uint8x8_t A) {
    uint16x8_t x;
    if (bA == 3) {
        return bB == 3 ? P : M;
    }
    if (bB == 3) {
        return P;
    }
    x = vmull_u8(P, A);
    x = vmlal_u8(x, M, vsub_u8(vdup_n_u8(255), A));
    x = vaddq_u16(vsraq_n_u16(x, x, 8), vdupq_n_u16(1));
    return vshrn_n_u16(x, 8);
}


/* Um trecho de linha em blocos de ate 64 pixels (8 grupos de 8):
 *   1. profundidade de todos os grupos do bloco: mascara e z novo de cada um; lista dos grupos com algum
 *      pixel vivo
 *   2. coordenadas de textura e amostragem dos grupos vivos, uma chamada por textura para o bloco inteiro
 *      (as constantes do amostrador ficam em registradores)
 *   3. por grupo vivo: sombreamento, combinador, alfa, blender e escrita
 * Cada etapa tem versoes para cada combinacao das opcoes que mudam o laco (com/sem z, tipo do blender...),
 * escolhidas uma vez por trecho: dentro dos lacos nao sobram testes dessas opcoes. */
#define FCH 8 /* grupos por bloco */

typedef struct {
    const GxPrim *p;
    const FastProg *f;
    FastCtx *ctx;
    int xs, xe, y;
    uint32_t base[5];
    float rowF[3];
    uint8_t *crow, *zrow;
    int zw, qOne;
    uint8x8_t thr;
    uint8x8_t dth;       /* pontilhado da cor: o que soma em cada um dos 8 pixels do grupo (0 = nenhum) */
    uint8_t *covW;       /* linha da mascara de cobertura a marcar (fundo adiado) */
    const uint8_t *covR; /* linha da mascara: desenha so onde ainda nao ha nada */
} SpanCtx;

/* etapa 1: mascara e z novo de cada grupo; devolve quantos grupos vivos (indices em live) */
static inline __attribute__((always_inline)) int zph_tpl(const SpanCtx *sc, int xc, int ng, uint8x8_t *mk, uint16x8_t *zn,
                                                         uint8_t *live, const int USEZ, const int ZCMP, const int ZDEC,
                                                         const int COVR) {
    const int xe = sc->xe;
    const uint32_t dz = (uint32_t)sc->p->dx[GXP_Z];
    const uint32x4_t lanes = vld1q_u32((const uint32_t *)k_ident);
    const uint32x4_t vdz = vmulq_n_u32(lanes, dz), dz4 = vdupq_n_u32(dz * 4u), dz8 = vdupq_n_u32(dz * 8u);
    const int32x4_t zero = vdupq_n_s32(0), zmax = vdupq_n_s32(0x3FFFF), six = vdupq_n_s32(6);
    const uint32x4_t seven = vdupq_n_u32(7), m7ff = vdupq_n_u32(0x7FF);
    const uint32x4_t tol = vdupq_n_u32((uint32_t)sc->p->zTol), top = vdupq_n_u32(0x40000);
    const uint8_t *zrow = sc->zrow;
    /* z (ponto fixo, + 128 para o arredondamento) do 1o pixel do bloco */
    uint32x4_t vb = vaddq_u32(vdupq_n_u32(sc->base[GXP_Z] + dz * (uint32_t)(xc - sc->xs) + 128u), vdz);
    int g, nl = 0;
    for (g = 0; g < ng; g++, vb = vaddq_u32(vb, dz8)) {
        const int xb = xc + 8 * g;
        const int n = xe - xb + 1 < 8 ? xe - xb + 1 : 8;
        uint8x8_t mask = vdup_n_u8(0xFF);
        if (n < 8) {
            mask = vclt_u8(vld1_u8(k_lane8), vdup_n_u8((uint8_t)n));
        }
        if (COVR) {
            mask = vbic_u8(mask, vld1_u8(sc->covR + xb));
        }
        if (USEZ) {
            const uint8_t *zp = zrow + (size_t)xb * 2u;
            uint8_t zbuf[16];
            uint16x8_t zraw;
            uint32x4_t zv0, zv1, e0, e1, zz0, zz1, p0, p1;
            if (n < 8 && xb + 8 > (int)sc->f->cwidth) {
                /* so no fim da linha: no meio, os 8 pixels sao lidos direto (os de depois do trecho sao da
                 * mesma linha, desta thread, e a mascara os ignora) */
                memcpy(zbuf, zp, (size_t)n * 2u);
                zp = zbuf;
            }
            zraw = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(zp)));
            zv0 = vreinterpretq_u32_s32(vminq_s32(vmaxq_s32(vshrq_n_s32(vreinterpretq_s32_u32(vb), 8), zero), zmax));
            zv1 = vreinterpretq_u32_s32(vminq_s32(vmaxq_s32(vshrq_n_s32(vreinterpretq_s32_u32(vaddq_u32(vb, dz4)), 8), zero), zmax));
            /* comprime para 14 bits (3 de expoente, 11 de mantissa), como gx_zcomp, ja << 2 (como fica na
             * memoria), em 16 bits (8 pixels por instrucao): zh = z >> 2 (16 bits), zl = z & 3.
             * e = 1s no topo dos 18 bits (ate 7); mantissa << 2 = (z >> (6 - min(e, 6))) << 2, montada como
             * (zh << (es - 2)) | (zl << (es - 4)) e cortada nos bits 12..2; o expoente vai nos bits 15..13 */
            (void)e0; (void)e1; (void)zz0; (void)zz1; (void)six; (void)seven; (void)m7ff;
            {
                const uint16x8_t zh = vcombine_u16(vshrn_n_u32(zv0, 2), vshrn_n_u32(zv1, 2));
                const uint16x8_t zl = vandq_u16(vcombine_u16(vmovn_u32(zv0), vmovn_u32(zv1)), vdupq_n_u16(3));
                const uint16x8_t ez = vminq_u16(vclzq_u16(vmvnq_u16(zh)), vdupq_n_u16(7));
                const int16x8_t es = vreinterpretq_s16_u16(vminq_u16(ez, vdupq_n_u16(6)));
                const uint16x8_t m2 = vandq_u16(vorrq_u16(vshlq_u16(zh, vsubq_s16(es, vdupq_n_s16(2))),
                                                          vshlq_u16(zl, vsubq_s16(es, vdupq_n_s16(4)))),
                                                vdupq_n_u16(0x1FFC));
                const uint16x8_t znew = vsliq_n_u16(m2, ez, 13);
                zn[g] = znew;
                if (ZCMP && !ZDEC) {
                    /* zz <= mem >> 2  <=>  zz << 2 <= mem (os 2 bits de baixo de zz << 2 sao 0): em 16 bits */
                    mask = vand_u8(mask, vmovn_u16(vcleq_u16(znew, zraw)));
                }
            }
            if (ZCMP && ZDEC) {
                uint32x4_t zm0 = vshrq_n_u32(vmovl_u16(vget_low_u16(zraw)), 2), zm1 = vshrq_n_u32(vmovl_u16(vget_high_u16(zraw)), 2);
                {
                    uint32x4_t em0 = vandq_u32(vshrq_n_u32(zm0, 11), seven), em1 = vandq_u32(vshrq_n_u32(zm1, 11), seven);
                    uint32x4_t d0 = vaddq_u32(vshlq_u32(vandq_u32(zm0, m7ff), vmaxq_s32(vsubq_s32(six, vreinterpretq_s32_u32(em0)), zero)),
                                              vsubq_u32(top, vshlq_u32(top, vnegq_s32(vreinterpretq_s32_u32(em0)))));
                    uint32x4_t d1 = vaddq_u32(vshlq_u32(vandq_u32(zm1, m7ff), vmaxq_s32(vsubq_s32(six, vreinterpretq_s32_u32(em1)), zero)),
                                              vsubq_u32(top, vshlq_u32(top, vnegq_s32(vreinterpretq_s32_u32(em1)))));
                    p0 = vcleq_u32(zv0, vaddq_u32(d0, tol));
                    p1 = vcleq_u32(zv1, vaddq_u32(d1, tol));
                }
                mask = vand_u8(mask, vmovn_u16(vcombine_u16(vmovn_u32(p0), vmovn_u32(p1))));
            }
        }
        if (vget_lane_u64(vreinterpret_u64_u8(mask), 0) == 0) {
            continue;
        }
        mk[g] = mask;
        live[nl++] = (uint8_t)g;
    }
    return nl;
}

/* etapa 2: S e T dos grupos vivos (8 por grupo, na ordem de live) */
static inline __attribute__((always_inline)) void tcph_tpl(const SpanCtx *sc, int xc, const uint8_t *live, int nl, float *S,
                                                           float *T, const int PERSP) {
    const GxPrim *p = sc->p;
    const float32x4_t r0 = vdupq_n_f32(sc->rowF[0]), r1 = vdupq_n_f32(sc->rowF[1]), r2 = vdupq_n_f32(sc->rowF[2]);
    const float32x4_t d0 = vdupq_n_f32(p->fdx[0]), d1 = vdupq_n_f32(p->fdx[1]), d2 = vdupq_n_f32(p->fdx[2]);
    const float32x4_t tiny = vdupq_n_f32(1e-12f), four = vdupq_n_f32(4.0f);
    /* fx = (x - fx0) do 1o pixel do bloco, nas 4 pistas (contas iguais as da referencia: inteiro -> float - fx0) */
    const int32x4_t lanes = vld1q_s32(k_ident);
    const float32x4_t vfx0 = vdupq_n_f32(p->fx0);
    int li;
    (void)four;
    for (li = 0; li < nl; li++) {
        const int xb = xc + 8 * live[li];
        float32x4_t fx0 = vsubq_f32(vcvtq_f32_s32(vaddq_s32(vdupq_n_s32(xb), lanes)), vfx0);
        float32x4_t fx1 = vsubq_f32(vcvtq_f32_s32(vaddq_s32(vdupq_n_s32(xb + 4), lanes)), vfx0);
        float32x4_t s0 = vaddq_f32(r0, vmulq_f32(d0, fx0)), t0 = vaddq_f32(r1, vmulq_f32(d1, fx0));
        float32x4_t s1 = vaddq_f32(r0, vmulq_f32(d0, fx1)), t1 = vaddq_f32(r1, vmulq_f32(d1, fx1));
        if (PERSP) {
            float32x4_t q0 = vaddq_f32(r2, vmulq_f32(d2, fx0)), q1 = vaddq_f32(r2, vmulq_f32(d2, fx1)), rq0, rq1;
            q0 = vbslq_f32(vcltq_f32(vabsq_f32(q0), tiny), tiny, q0);
            q1 = vbslq_f32(vcltq_f32(vabsq_f32(q1), tiny), tiny, q1);
            rq0 = vrecpeq_f32(q0);
            rq1 = vrecpeq_f32(q1);
            rq0 = vmulq_f32(rq0, vrecpsq_f32(q0, rq0));
            rq1 = vmulq_f32(rq1, vrecpsq_f32(q1, rq1));
            rq0 = vmulq_f32(rq0, vrecpsq_f32(q0, rq0));
            rq1 = vmulq_f32(rq1, vrecpsq_f32(q1, rq1));
            s0 = vmulq_f32(s0, rq0);
            t0 = vmulq_f32(t0, rq0);
            s1 = vmulq_f32(s1, rq1);
            t1 = vmulq_f32(t1, rq1);
        }
        vst1q_f32(S + 8 * li, s0);
        vst1q_f32(S + 8 * li + 4, s1);
        vst1q_f32(T + 8 * li, t0);
        vst1q_f32(T + 8 * li + 4, t1);
    }
}

/* (a - b) * c / 256 arredondado + d nos 3 canais (combinador) */
static inline __attribute__((always_inline)) void fused_ops_run(uint8x8_t *R, const FusedOp *ops, int nops) {
    int k;
    for (k = 0; k < nops; k++) {
        const FusedOp *o = &ops[k];
        const int a = o->a, b = o->b, c = o->c, d = o->d, dst = o->dst;
        switch (o->kind) {
            case 0:
                R[dst] = R[d];
                break;
            case 1:
                R[dst] = vrshrn_n_u16(vmull_u8(R[a], R[c]), 8); /* (a*c + 0x80) >> 8: igual a cc8 */
                break;
            case 2:
                R[dst] = cc8(R[a], R[b], R[c], R[d]);
                break;
            case 0 | FOP_RGB: {
                uint8x8_t x0 = R[d], x1 = R[d + 1], x2 = R[d + 2];
                R[dst] = x0;
                R[dst + 1] = x1;
                R[dst + 2] = x2;
                break;
            }
            case 1 | FOP_RGB: {
                uint8x8_t a0 = R[a], a1 = R[a + 1], a2 = R[a + 2], c0 = R[c], c1 = R[c + 1], c2 = R[c + 2];
                R[dst] = vrshrn_n_u16(vmull_u8(a0, c0), 8);
                R[dst + 1] = vrshrn_n_u16(vmull_u8(a1, c1), 8);
                R[dst + 2] = vrshrn_n_u16(vmull_u8(a2, c2), 8);
                break;
            }
            case 1 | FOP_RGB | FOP_C1: {
                uint8x8_t a0 = R[a], a1 = R[a + 1], a2 = R[a + 2], c0 = R[c];
                R[dst] = vrshrn_n_u16(vmull_u8(a0, c0), 8);
                R[dst + 1] = vrshrn_n_u16(vmull_u8(a1, c0), 8);
                R[dst + 2] = vrshrn_n_u16(vmull_u8(a2, c0), 8);
                break;
            }
            case 2 | FOP_RGB: {
                uint8x8_t a0 = R[a], a1 = R[a + 1], a2 = R[a + 2], b0 = R[b], b1 = R[b + 1], b2 = R[b + 2];
                uint8x8_t c0 = R[c], c1 = R[c + 1], c2 = R[c + 2], d0 = R[d], d1 = R[d + 1], d2 = R[d + 2];
                R[dst] = cc8(a0, b0, c0, d0);
                R[dst + 1] = cc8(a1, b1, c1, d1);
                R[dst + 2] = cc8(a2, b2, c2, d2);
                break;
            }
            default: { /* 2 | FOP_RGB | FOP_C1 */
                uint8x8_t a0 = R[a], a1 = R[a + 1], a2 = R[a + 2], b0 = R[b], b1 = R[b + 1], b2 = R[b + 2];
                uint8x8_t c0 = R[c], d0 = R[d], d1 = R[d + 1], d2 = R[d + 2];
                R[dst] = cc8(a0, b0, c0, d0);
                R[dst + 1] = cc8(a1, b1, c0, d1);
                R[dst + 2] = cc8(a2, b2, c0, d2);
                break;
            }
        }
    }
}

/* etapa 3: um grupo vivo. FBLM = forma do blender, MEM = le a cor da memoria */
static inline __attribute__((always_inline)) void grp_tpl(const SpanCtx *sc, int xb, int li, uint8x8_t mask, uint16x8_t znew,
                                                          const uint32_t *tx0, const uint32_t *tx1, const int FBLM, const int MEM,
                                                          const int COVW) {
    const FastProg *f = sc->f;
    FastCtx *ctx = sc->ctx;
    uint8x8_t *const R = ctx->R;
    const GxPrim *p = sc->p;
    const int n = sc->xe - xb + 1 < 8 ? sc->xe - xb + 1 : 8;
    const uint32_t j0 = (uint32_t)(xb - sc->xs);
    const uint8x8_t v255 = vdup_n_u8(255);
    uint8x8_t cc[4], out[3], mem[3], aIn;
    uint8_t cbuf[16], zbuf[16];
    uint8_t *cp = sc->crow + (size_t)xb * 2u, *zp = sc->zw ? sc->zrow + (size_t)xb * 2u : NULL;
    const uint8_t *cin = cp, *zin = zp;
    int k;
    if (n < 8 && xb + 8 > (int)f->cwidth) {
        /* fim do trecho no fim da linha: le/escreve por uma copia (nao passa do fim da linha). No meio da
         * linha os 8 pixels sao lidos e escritos direto: os que ficam depois do trecho sao desta mesma
         * linha (so esta thread mexe nela) e voltam com o mesmo valor (a mascara os deixa como estavam) */
        memcpy(cbuf, cp, (size_t)n * 2u);
        cin = cbuf;
        if (zp != NULL) {
            memcpy(zbuf, zp, (size_t)n * 2u);
            zin = zbuf;
        }
    }
    uint8x8_t sh[4], t0[4], t1[4];
    /* --- sombreamento (canal a canal, sem indice variavel: os 4 ficam em registradores) --- */
    if (f->fnsh) {
        /* valor = (base + d*j + 0x8000) >> 16, saturado em 0..255 (vqshrun = deslocamento com saturacao) */
        const uint32x4_t lanes = vld1q_u32((const uint32_t *)k_ident);
#define SHADE_CH(ch)                                                                                      \
        if (f->needShade[ch]) {                                                                           \
            const uint32_t d = (uint32_t)p->dx[ch];                                                       \
            uint32x4_t v0 = vmlaq_n_u32(vdupq_n_u32(sc->base[ch] + d * j0 + 0x8000u), lanes, d);          \
            uint32x4_t v1 = vaddq_u32(v0, vdupq_n_u32(d * 4u));                                           \
            sh[ch] = vqmovn_u16(vcombine_u16(vqshrun_n_s32(vreinterpretq_s32_u32(v0), 16),                \
                                             vqshrun_n_s32(vreinterpretq_s32_u32(v1), 16)));              \
            R[V_SHADE + ch] = sh[ch];                                                                     \
        }
        SHADE_CH(0)
        SHADE_CH(1)
        SHADE_CH(2)
        SHADE_CH(3)
#undef SHADE_CH
    }
    /* --- texels --- */
    if (tx0 != NULL) {
        uint8x8x4_t cv = vld4_u8((const uint8_t *)(tx0 + 8 * li));
        t0[0] = cv.val[0];
        t0[1] = cv.val[1];
        t0[2] = cv.val[2];
        t0[3] = cv.val[3];
        if (f->kern == KERN_GEN || f->nfopsA) {
            R[V_T0 + 0] = cv.val[0];
            R[V_T0 + 1] = cv.val[1];
            R[V_T0 + 2] = cv.val[2];
            R[V_T0 + 3] = cv.val[3];
        }
    }
    if (tx1 != NULL) {
        uint8x8x4_t cv = vld4_u8((const uint8_t *)(tx1 + 8 * li));
        t1[0] = cv.val[0];
        t1[1] = cv.val[1];
        t1[2] = cv.val[2];
        t1[3] = cv.val[3];
        if (f->kern == KERN_GEN || f->nfopsA) {
            R[V_T1 + 0] = cv.val[0];
            R[V_T1 + 1] = cv.val[1];
            R[V_T1 + 2] = cv.val[2];
            R[V_T1 + 3] = cv.val[3];
        }
    }
    /* --- grupo todo transparente (alfa 0 que nao depende do combinador de cor): nada muda na tela --- */
    if (MEM && f->a0skip && (f->kAcls == KA_K || ((f->kAcls == KA_T0A || f->kAcls == KA_T0A_K) && tx0 != NULL))) {
        const uint8x8_t a = f->kAcls == KA_K ? ctx->kA
                          : f->kAcls == KA_T0A ? t0[3] : vrshrn_n_u16(vmull_u8(t0[3], ctx->kAv[0]), 8);
        mask = vand_u8(mask, vtst_u8(a, a));
        if (vget_lane_u64(vreinterpret_u64_u8(mask), 0) == 0) {
            return;
        }
    }
    /* --- combinador --- */
    if (f->kern == KERN_GEN) {
        fused_ops_run(R, (const FusedOp *)f->fops, f->nfops);
        cc[0] = R[f->fcc[0]];
        cc[1] = R[f->fcc[1]];
        cc[2] = R[f->fcc[2]];
    } else {
        const uint8x8_t(*const K)[3] = ctx->kk;
        if (f->nfopsA) {
            fused_ops_run(R, (const FusedOp *)f->fopsA, f->nfopsA);
        }
        switch (f->kern) {
            case KERN_T0_SH_K:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = vrshrn_n_u16(vmull_u8(vrshrn_n_u16(vmull_u8(t0[k], sh[k]), 8), K[4][k]), 8);
                }
                break;
            case KERN_T0_K:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = vrshrn_n_u16(vmull_u8(t0[k], K[2][k]), 8);
                }
                break;
            case KERN_T0_SH:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = vrshrn_n_u16(vmull_u8(t0[k], sh[k]), 8);
                }
                break;
            case KERN_LERP_TT:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = cc8(t1[k], t0[k], K[2][0], t0[k]);
                }
                break;
            case KERN_LERP_TT_SH:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = vrshrn_n_u16(vmull_u8(cc8(t1[k], t0[k], K[2][0], t0[k]), sh[k]), 8);
                }
                break;
            case KERN_KK_T0_SH:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = vrshrn_n_u16(vmull_u8(cc8(K[0][k], K[1][k], t0[k], K[3][k]), sh[k]), 8);
                }
                break;
            case KERN_KK_T0:
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = cc8(K[0][k], K[1][k], t0[k], K[3][k]);
                }
                break;
            case KERN_K:
                cc[0] = K[0][0];
                cc[1] = K[0][1];
                cc[2] = K[0][2];
                break;
            default: /* KERN_TKT_KKK */
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    cc[k] = cc8(K[5][k], K[6][k], cc8(t1[k], K[1][k], K[2][0], t0[k]), K[7][k]);
                }
                break;
        }
    }
    switch (f->kAcls) {
        case KA_T0A: cc[3] = t0[3]; break;
        case KA_K: cc[3] = ctx->kA; break;
        case KA_T0A_K: cc[3] = vrshrn_n_u16(vmull_u8(t0[3], ctx->kAv[0]), 8); break;
        case KA_TT_K: cc[3] = vrshrn_n_u16(vmull_u8(cc8(t1[3], ctx->kAv[0], ctx->kAv[1], t0[3]), ctx->kAv[2]), 8); break;
        default: cc[3] = R[f->fcc[3]]; break;
    }
    /* --- alfa / cobertura --- */
    if (f->cvgXAlpha) {
        mask = vand_u8(mask, vtst_u8(cc[3], vdup_n_u8(0xE0)));
    }
    if (f->ac == 1) {
        mask = vand_u8(mask, vcge_u8(cc[3], ctx->blc[3]));
    } else if (f->ac == 3) {
        mask = vand_u8(mask, vcge_u8(cc[3], sc->thr));
    }
    if (MEM && f->a0skip) {
        mask = vand_u8(mask, vtst_u8(cc[3], cc[3])); /* alfa 0: o pixel fica como esta */
    }
    if (f->cvgXAlpha || f->ac || (MEM && f->a0skip)) {
        if (vget_lane_u64(vreinterpret_u64_u8(mask), 0) == 0) {
            return;
        }
    }
    aIn = (f->alphaCvgSel && !f->cvgXAlpha) ? v255 : cc[3];
    /* --- blender --- */
    if (MEM) {
        /* x5 -> (x5 << 3) | (x5 >> 2): o canal ja deslocado para os bits 7..3 de um byte (vshrn/vmovn) e
         * somado a ele mesmo >> 5 (os bits nao se cruzam: soma = ou) */
        uint16x8_t c = vreinterpretq_u16_u8(crev(vld1q_u8(cin), f->cle));
        const uint8x8_t f8 = vdup_n_u8(0xF8);
        uint8x8_t r8 = vand_u8(vshrn_n_u16(c, 8), f8);
        uint8x8_t g8 = vand_u8(vshrn_n_u16(c, 3), f8);
        uint8x8_t b8 = vand_u8(vmovn_u16(vshlq_n_u16(c, 2)), f8);
        if (f->cdith) {
            /* com o pontilhado: a memoria lida como o RDP (5 bits << 3, sem repetir os de cima): um pixel misturado
             * sem mudar (alfa 0) volta igual; repetindo os bits ele subia 1 degrau aos poucos (linhas, retangulos) */
            mem[0] = r8;
            mem[1] = g8;
            mem[2] = b8;
        } else {
            mem[0] = vsra_n_u8(r8, r8, 5);
            mem[1] = vsra_n_u8(g8, g8, 5);
            mem[2] = vsra_n_u8(b8, b8, 5);
        }
    } else {
        mem[0] = mem[1] = mem[2] = vdup_n_u8(0);
    }
    if (FBLM == BLM_COR) {
        out[0] = cc[0];
        out[1] = cc[1];
        out[2] = cc[2];
    } else if (FBLM == BLM_NEBLINA || FBLM == BLM_NEB_ALFA) {
        /* (neblina * aS + cor * (255 - aS)) / 255, aS = alfa do sombreamento */
        const uint8x8_t aS = R[V_SHADE + 3], naS = vsub_u8(v255, aS);
        const uint16x8_t one = vdupq_n_u16(1);
        #pragma GCC unroll 4
        for (k = 0; k < 3; k++) {
            uint16x8_t x = vmlal_u8(vmull_u8(ctx->fogc[k], aS), cc[k], naS);
            x = vaddq_u16(vsraq_n_u16(x, x, 8), one);
            out[k] = vshrn_n_u16(x, 8);
        }
        if (FBLM == BLM_NEB_ALFA) {
            /* (resultado * a + memoria * (255 - a)) / 255 */
            const uint8x8_t na = vsub_u8(v255, aIn);
            #pragma GCC unroll 4
            for (k = 0; k < 3; k++) {
                uint16x8_t x = vmlal_u8(vmull_u8(out[k], aIn), mem[k], na);
                x = vaddq_u16(vsraq_n_u16(x, x, 8), one);
                out[k] = vshrn_n_u16(x, 8);
            }
        }
    } else {
        const uint8x8_t *const fogc = ctx->fogc, *const blc = ctx->blc;
        const uint8x8_t zero8 = vdup_n_u8(0);
        const int (*bl)[4] = f->bl;
#define FSEL(sel, ch, in) ((sel) == 0 ? (in)[ch] : (sel) == 1 ? mem[ch] : (sel) == 2 ? blc[ch] : fogc[ch])
#define FSELA(sel) ((sel) == 0 ? aIn : (sel) == 1 ? fogc[3] : (sel) == 2 ? R[V_SHADE + 3] : zero8)
        if (f->cyc2) {
            uint8x8_t mid[3];
            const uint8x8_t A0 = FSELA(bl[0][1]);
            #pragma GCC unroll 4
            for (k = 0; k < 3; k++) {
                mid[k] = bl8(bl[0][1], bl[0][3], FSEL(bl[0][0], k, cc), FSEL(bl[0][2], k, cc), A0);
            }
            if (f->forceBl) {
                const uint8x8_t A1 = FSELA(bl[1][1]);
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    out[k] = bl8(bl[1][1], bl[1][3], FSEL(bl[1][0], k, mid), FSEL(bl[1][2], k, mid), A1);
                }
            } else {
                #pragma GCC unroll 4
                for (k = 0; k < 3; k++) {
                    out[k] = FSEL(bl[1][0], k, mid);
                }
            }
        } else if (f->forceBl) {
            const uint8x8_t A0 = FSELA(bl[0][1]);
            #pragma GCC unroll 4
            for (k = 0; k < 3; k++) {
                out[k] = bl8(bl[0][1], bl[0][3], FSEL(bl[0][0], k, cc), FSEL(bl[0][2], k, cc), A0);
            }
        } else {
            #pragma GCC unroll 4
            for (k = 0; k < 3; k++) {
                out[k] = FSEL(bl[0][0], k, cc);
            }
        }
#undef FSEL
#undef FSELA
    }
    /* --- escrita (so os pixels da mascara) --- */
    if (COVW) {
        uint8_t *cv = sc->covW + xb;
        vst1_u8(cv, vorr_u8(vld1_u8(cv), mask));
    }
    {
        /* 5551: cada canal no byte alto (vshll #8) e os de baixo inseridos por vsri: R nos bits 15..11, G em
         * 10..6, B em 5..1 (os bits que sobram abaixo sao trocados pelo alfa 1) - o mesmo que (x >> 3) << n */
        uint16x8_t c;
        out[0] = vqadd_u8(out[0], sc->dth); /* (pontilhado da cor; 0 quando desligado) */
        out[1] = vqadd_u8(out[1], sc->dth);
        out[2] = vqadd_u8(out[2], sc->dth);
        c = vsriq_n_u16(vshll_n_u8(out[0], 8), vshll_n_u8(out[1], 8), 5);
        const uint16x8_t m16 = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(mask)));
        uint8x16_t res;
        c = vsriq_n_u16(c, vshll_n_u8(out[2], 8), 10);
        c = vorrq_u16(c, vdupq_n_u16(1));
        res = vbslq_u8(vreinterpretq_u8_u16(m16), crev(vreinterpretq_u8_u16(c), f->cle), vld1q_u8(cin));
        if (cin == cp) {
            vst1q_u8(cp, res);
        } else {
            vst1q_u8(cbuf, res);
            memcpy(cp, cbuf, (size_t)n * 2u);
        }
        if (zp != NULL) {
            uint8x16_t zr = vbslq_u8(vreinterpretq_u8_u16(m16), vrev16q_u8(vreinterpretq_u8_u16(znew)), vld1q_u8(zin));
            if (zin == zp) {
                vst1q_u8(zp, zr);
            } else {
                vst1q_u8(zbuf, zr);
                memcpy(zp, zbuf, (size_t)n * 2u);
            }
        }
    }
}

#define GRP_LOOP(FB_, MEM_, COV_)                                                                                \
    for (li = 0; li < nl; li++) {                                                                                \
        const int g = live[li];                                                                                  \
        grp_tpl(&sc, xc + 8 * g, li, mk[g], zn[g], useT0 ? txb[0] : NULL, useT1 ? txb[1] : NULL, FB_, MEM_, COV_);\
    }

/* Textura ampliada (cada texel cobre 2 pixels ou mais na horizontal), com filtro: amostra so os pixels pares
 * (0, 2, 4, 6 e o 8, logo depois do grupo) e os impares saem da media dos vizinhos. Dentro de um texel o filtro e
 * uma rampa (bilinear/3 pontos): a media de 2 pontos e o ponto do meio, a menos do arredondamento e de quando os 2
 * caem em texels diferentes (diferenca pequena; a textura ja e borrada por estar ampliada). 5 amostras por grupo de
 * 8 em vez de 8. Opcao gx_tex_meia (replay --sem-meia). Devolve 0 se a textura nao esta ampliada o bastante. */
static inline int tex_meia(SampNFn fn, const FastTex *u, const float *S, const float *T, uint32_t *out, int nl) {
    float S2[8 * FCH], T2[8 * FCH];
    uint32_t tmp[8 * FCH];
    int li, n2, j;
    {
        /* passo em texels por pixel (S * escala = 1/32 de texel), no 1o e no ultimo grupo do bloco */
        const float kS = fabsf(u->scaleS) * (1.0f / 32.0f / 7.0f), kT = fabsf(u->scaleT) * (1.0f / 32.0f / 7.0f);
        const float *s0 = S, *t0 = T, *s1 = S + 8 * (nl - 1), *t1 = T + 8 * (nl - 1);
        if (fabsf(s0[7] - s0[0]) * kS > 0.5f || fabsf(t0[7] - t0[0]) * kT > 0.5f ||
            fabsf(s1[7] - s1[0]) * kS > 0.5f || fabsf(t1[7] - t1[0]) * kT > 0.5f) {
            return 0;
        }
    }
    for (li = 0; li < nl; li++) {
        const float *s = S + 8 * li, *t = T + 8 * li;
        float *ds = S2 + 5 * li, *dt = T2 + 5 * li;
        ds[0] = s[0]; ds[1] = s[2]; ds[2] = s[4]; ds[3] = s[6]; ds[4] = s[6] + (s[6] - s[4]);
        dt[0] = t[0]; dt[1] = t[2]; dt[2] = t[4]; dt[3] = t[6]; dt[4] = t[6] + (t[6] - t[4]);
    }
    n2 = 5 * nl;
    for (j = n2; j < ((n2 + 7) & ~7); j++) {
        S2[j] = S2[n2 - 1];
        T2[j] = T2[n2 - 1];
    }
    fn(u, S2, T2, tmp, (n2 + 7) >> 3);
    for (li = 0; li < nl; li++) {
        const uint32_t *a = tmp + 5 * li;
        const uint8x16_t A = vreinterpretq_u8_u32(vld1q_u32(a)), B = vreinterpretq_u8_u32(vld1q_u32(a + 1));
        const uint32x4x2_t z = vzipq_u32(vreinterpretq_u32_u8(A), vreinterpretq_u32_u8(vrhaddq_u8(A, B)));
        vst1q_u32(out + 8 * li, z.val[0]);
        vst1q_u32(out + 8 * li + 4, z.val[1]);
    }
    return 1;
}

/* uma versao por forma do blender (gsel = BLM * 2 + le a memoria): funcoes menores, o compilador guarda
 * mais coisas em registradores e cada trecho comeca mais barato */
static inline __attribute__((always_inline)) void span_fused_tpl(const GxPrim *p, const FastProg *f, FastCtx *ctx, int y,
                                                                 int xs, int xe, const int GS, const int NCH) {
    uint8x8_t *const R = ctx->R;
    const int useT0 = f->useT[0], useT1 = f->useT[1];
    const int zsel = !(p->useZ && f->zimg != NULL) ? 0 : !f->zcmp ? 1 : !f->zdec ? 2 : 3;
    SpanCtx sc;
    int k, xc;

    sc.p = p;
    sc.f = f;
    sc.ctx = ctx;
    sc.xs = xs;
    sc.xe = xe;
    sc.y = y;
    #pragma GCC unroll 4
    for (k = 0; k < 3; k++) {
        sc.rowF[k] = p->f0[k] + p->fdy[k] * ((float)y - p->fy0);
    }
    sc.qOne = p->fdx[2] == 0.0f && p->fdy[2] == 0.0f && p->f0[2] == 1.0f;
    for (k = 0; k < 5; k++) {
        sc.base[k] = (uint32_t)(int32_t)(p->c[k] + (int64_t)p->dy[k] * y + (int64_t)p->dx[k] * xs);
    }
    sc.crow = f->cimg + (size_t)y * (size_t)f->cwidth * 2u;
    sc.zrow = f->zimg ? f->zimg + (size_t)y * (size_t)f->cwidth * 2u : NULL;
    sc.zw = zsel != 0 && f->zupd;
    sc.covW = ctx->covW && y < COV_H ? g_cov[y] : NULL;
    sc.covR = ctx->covR && y < COV_H ? g_cov[y] : NULL;
    /* constantes do combinador e do blender: so quando o programa muda (trechos seguidos costumam ser
     * do mesmo triangulo) */
    if (ctx->rProg != (const void *)f || ctx->rGen != f->gen) {
        uint32_t z = f->fzero;
        while (z) {
            R[__builtin_ctz(z)] = vdup_n_u8(0);
            z &= z - 1;
        }
        for (k = 0; k < f->nConst; k++) {
            R[f->cIdx[k]] = vdup_n_u8((uint8_t)f->cVal[k]);
        }
        #pragma GCC unroll 4
        for (k = 0; k < 4; k++) {
            ctx->fogc[k] = vdup_n_u8(f->fogc[k]);
            ctx->blc[k] = vdup_n_u8(f->blendc[k]);
        }
        if (f->kern != KERN_GEN) {
            const uint8_t idx[8] = { f->ka0, f->kb0, f->kc0, f->kd0, f->kc1, f->ka1, f->kb1, f->kd1 };
            const uint8_t str[8] = { 1, 1, f->ks0, 1, f->ks1, 1, 1, 1 };
            int q;
            for (q = 0; q < 8; q++) {
                if (f->kcls[q] == KS_K3) {
                    #pragma GCC unroll 4
                    for (k = 0; k < 3; k++) {
                        ctx->kk[q][k] = R[idx[q] + k * str[q]];
                    }
                } else if (f->kcls[q] == KS_K1) {
                    ctx->kk[q][0] = ctx->kk[q][1] = ctx->kk[q][2] = R[idx[q]];
                }
            }
            if (f->kAcls == KA_K) {
                ctx->kA = R[f->kAidx];
            } else if (f->kAcls >= KA_T0A_K) {
                ctx->kAv[0] = R[f->kAi[0]];
                ctx->kAv[1] = R[f->kAi[1]];
                ctx->kAv[2] = R[f->kAi[2]];
            }
        }
        ctx->rProg = f;
        ctx->rGen = f->gen;
    }
    sc.dth = vdup_n_u8(0);
    if (f->cdith) {
        extern const uint8_t gx_dith[3][4][4];
        uint8_t t[8];
        for (k = 0; k < 8; k++) {
            t[k] = gx_dith[f->cdith][y & 3][(xs + k) & 3];
        }
        sc.dth = vld1_u8(t);
    }
    sc.thr = vdup_n_u8(0);
    if (f->ac == 3) {
        uint8_t t[8];
        for (k = 0; k < 8; k++) {
            t[k] = (uint8_t)(bayer4[y & 3][(xs + k) & 3] * 16 + 8);
        }
        sc.thr = vld1_u8(t);
    }

    for (xc = xs; xc <= xe; xc += 8 * NCH) {
        uint8x8_t mk[NCH];
        uint16x8_t zn[NCH];
        uint8_t live[NCH];
        uint32_t txb[2][8 * NCH];
        int nl, li;
        const int ng = (xe - xc + 8) >> 3 < NCH ? (xe - xc + 8) >> 3 : NCH;
        if (gx_pld && xc + 8 * NCH <= xe) {
            /* o bloco seguinte (64 pixels = 2 linhas de cache em cada buffer) */
            const size_t o = (size_t)(xc + 8 * NCH) * 2u;
            __builtin_prefetch(sc.crow + o);
            __builtin_prefetch(sc.crow + o + 64);
            if (sc.zrow != NULL) {
                __builtin_prefetch(sc.zrow + o);
                __builtin_prefetch(sc.zrow + o + 64);
            }
        }
        /* --- 1. profundidade --- */
        switch (zsel) {
            case 0: nl = sc.covR ? zph_tpl(&sc, xc, ng, mk, zn, live, 0, 0, 0, 1) : zph_tpl(&sc, xc, ng, mk, zn, live, 0, 0, 0, 0); break;
            case 1: nl = zph_tpl(&sc, xc, ng, mk, zn, live, 1, 0, 0, 0); break;
            case 2: nl = zph_tpl(&sc, xc, ng, mk, zn, live, 1, 1, 0, 0); break;
            default: nl = zph_tpl(&sc, xc, ng, mk, zn, live, 1, 1, 1, 0); break;
        }
        if (nl == 0) {
            continue;
        }
        /* --- 2. texturas dos grupos vivos --- */
        if (useT0 || useT1) {
            float S[8 * NCH], T[8 * NCH];
            int t;
            /* amostrar so os pares (tex_meia): com filtro, sem teste de alfa/cobertura (as bordas recortadas ficam
             * exatas) e com textura (coordenadas de verdade) */
            const int meia = gx_tex_meia && f->bilerp && p->hasTex && !f->ac && !f->cvgXAlpha;
            if (!p->hasTex) {
                memset(S, 0, (size_t)nl * 8u * sizeof(float));
                memset(T, 0, (size_t)nl * 8u * sizeof(float));
            } else if (sc.qOne) {
                tcph_tpl(&sc, xc, live, nl, S, T, 0);
            } else if (!gx_fast_exact) {
                tcph_tpl(&sc, xc, live, nl, S, T, 1);
            } else {
                for (li = 0; li < nl; li++) {
                    const int xb = xc + 8 * live[li];
                    for (t = 0; t < 8; t++) {
                        float fx = (float)(xb + t) - p->fx0;
                        float q = sc.rowF[2] + p->fdx[2] * fx, rq;
                        if (fabsf(q) < 1e-12f) {
                            q = 1e-12f;
                        }
                        rq = 1.0f / q;
                        S[8 * li + t] = (sc.rowF[0] + p->fdx[0] * fx) * rq;
                        T[8 * li + t] = (sc.rowF[1] + p->fdx[1] * fx) * rq;
                    }
                }
            }
            for (t = 0; t < 2; t++) {
                if (!(t ? useT1 : useT0)) {
                    continue;
                }
                if (f->sampn[t] == NULL) {
                    memset(txb[t], 0, (size_t)nl * 8u * 4u);
                } else if (meia && !(ctx->hdF && f->sampnF[t] != NULL) && !f->t[t].hdPoint &&
                           tex_meia(f->sampn[t], &f->t[t], S, T, txb[t], nl)) {
                    /* (amostras pares + medias) */
                } else if (ctx->hdF && f->sampnF[t] != NULL) {
                    f->sampnF[t](&f->tF[t], S, T, txb[t], nl);
                } else {
                    f->sampn[t](&f->t[t], S, T, txb[t], nl);
                }
            }
        }
        /* --- 3. o resto, grupo a grupo --- */
        switch (GS) {
            case BLM_COR * 2: if (sc.covW) { GRP_LOOP(BLM_COR, 0, 1) } else { GRP_LOOP(BLM_COR, 0, 0) } break;
            case BLM_COR * 2 + 1: GRP_LOOP(BLM_COR, 1, 0) break;
            case BLM_NEBLINA * 2: if (sc.covW) { GRP_LOOP(BLM_NEBLINA, 0, 1) } else { GRP_LOOP(BLM_NEBLINA, 0, 0) } break;
            case BLM_NEBLINA * 2 + 1: GRP_LOOP(BLM_NEBLINA, 1, 0) break;
            case BLM_NEB_ALFA * 2: if (sc.covW) { GRP_LOOP(BLM_NEB_ALFA, 0, 1) } else { GRP_LOOP(BLM_NEB_ALFA, 0, 0) } break;
            case BLM_NEB_ALFA * 2 + 1: GRP_LOOP(BLM_NEB_ALFA, 1, 0) break;
            case BLM_GERAL * 2: if (sc.covW) { GRP_LOOP(BLM_GERAL, 0, 1) } else { GRP_LOOP(BLM_GERAL, 0, 0) } break;
            default: GRP_LOOP(BLM_GERAL, 1, 0) break;
        }
    }
}
#undef GRP_LOOP
#define SPFN(g)                                                                                           \
    static void span_fused_##g(const GxPrim *p, const FastProg *f, FastCtx *ctx, int y, int xs, int xe) { \
        span_fused_tpl(p, f, ctx, y, xs, xe, g, FCH);                                                     \
    }
SPFN(0) SPFN(1) SPFN(2) SPFN(3) SPFN(4) SPFN(5) SPFN(6) SPFN(7)
#undef SPFN
static void (*const g_spanFused[8])(const GxPrim *, const FastProg *, FastCtx *, int, int, int) = {
    span_fused_0, span_fused_1, span_fused_2, span_fused_3, span_fused_4, span_fused_5, span_fused_6, span_fused_7
};
/* a versao unica de antes (gsel decidido a cada trecho), para comparar no aparelho (--sem-spec) */
static void span_fused_gen(const GxPrim *p, const FastProg *f, FastCtx *ctx, int y, int xs, int xe) {
    span_fused_tpl(p, f, ctx, y, xs, xe, f->fblm * 2 + (f->needMem ? 1 : 0), FCH);
}
static void gx_span_fused(const GxPrim *p, const FastProg *f, FastCtx *ctx, int y, int xs, int xe) {
    int g;
    if (!gx_spanspec) {
        span_fused_gen(p, f, ctx, y, xs, xe);
        return;
    }
    g = f->fblm * 2 + (f->needMem ? 1 : 0);
    if (f->fblm == BLM_NEBLINA && gx_fog_skip) {
        /* alfa do sombreamento (a neblina) no 1o e no ultimo pixel, antes do arredondamento: (v + 0x8000) >> 16 com
         * saturacao e 0 para v < 0x8000; em linha reta entre os dois, entao zero no trecho todo */
        const int64_t a0 = p->c[GXP_A] + (int64_t)p->dy[GXP_A] * y + (int64_t)p->dx[GXP_A] * xs;
        const int64_t a1 = a0 + (int64_t)p->dx[GXP_A] * (xe - xs);
        if (a0 < 0x8000 && a1 < 0x8000 && a0 > -0x40000000LL && a1 > -0x40000000LL) {
            g = BLM_COR * 2 + (f->needMem ? 1 : 0);
        }
    }
    g_spanFused[g & 7](p, f, ctx, y, xs, xe);
}
#endif

/* encurta o trecho [xs, xe] da linha y para os pixels cuja coordenada de textura cai dentro do retangulo visivel
 * (FastProg crop). S = (S*q)/q com S*q e q lineares em x: cada limite vira uma desigualdade linear em x. Devolve 0 se
 * nada sobra. So recorta se a linha inteira cai dentro de um periodo da textura (sem repetir) e q > 0. */
static int span_crop(const GxPrim *p, const FastProg *f, int y, int *pxs, int *pxe) {
    /* (contas em float, sem chamadas da libm: no lote 125 a versao em double custava mais do que poupava no aparelho) */
    const FastTex *u = &f->t[0];
    const float fy = (float)y - p->fy0;
    const float a0 = p->f0[0] + p->fdy[0] * fy, a1 = p->f0[1] + p->fdy[1] * fy, a2 = p->f0[2] + p->fdy[2] * fy;
    const float b0 = p->fdx[0], b1 = p->fdx[1], b2 = p->fdx[2];
    const float sS = u->scaleS, oS = u->offS, sT = u->scaleT, oT = u->offT;
    float lo = (float)*pxs, hi = (float)*pxe;
    const float xs = (float)*pxs - p->fx0, xe = (float)*pxe - p->fx0;
    int k;
    if (!p->hasTex) {
        return 1;
    }
    {
        /* as pontas: q > 0 e dentro de um periodo (S monotono na linha: as pontas bastam) */
        const float q0 = a2 + b2 * xs, q1 = a2 + b2 * xe;
        float r0, r1, s0, s1, t0, t1;
        if (!(q0 > 1e-6f && q1 > 1e-6f)) {
            return 1;
        }
        r0 = 1.0f / q0;
        r1 = 1.0f / q1;
        s0 = (a0 + b0 * xs) * r0 * sS - oS;
        s1 = (a0 + b0 * xe) * r1 * sS - oS;
        t0 = (a1 + b1 * xs) * r0 * sT - oT;
        t1 = (a1 + b1 * xe) * r1 * sT - oT;
        if (s0 < 0.0f || s1 < 0.0f || t0 < 0.0f || t1 < 0.0f || s0 >= f->cropW || s1 >= f->cropW || t0 >= f->cropH ||
            t1 >= f->cropH) {
            return 1;
        }
    }
    for (k = 0; k < 4; k++) {
        /* limite L: S*escala - desl >= L  <=>  (a + b*u)*escala - (L + desl)*(a2 + b2*u) >= 0 (u = x - fx0); o de cima
         * com o sinal trocado */
        const int isS = k < 2, isLo = (k & 1) == 0;
        const float sc = isS ? sS : sT, of = isS ? oS : oT;
        const float L = isS ? (isLo ? f->cropS0 : f->cropS1) : (isLo ? f->cropT0 : f->cropT1);
        const float na = isS ? a0 : a1, nb = isS ? b0 : b1;
        float A = na * sc - (L + of) * a2, B = nb * sc - (L + of) * b2;
        if (!isLo) {
            A = -A;
            B = -B;
        }
        if (B > 1e-9f) {
            const float x = p->fx0 - A / B; /* x >= */
            if (x > lo) lo = x;
        } else if (B < -1e-9f) {
            const float x = p->fx0 - A / B; /* x <= */
            if (x < hi) hi = x;
        } else if (A < 0.0f) {
            return 0;
        }
    }
    {
        /* 2 pixels de folga de cada lado (arredondamentos do float) */
        const int nxs = gx_floori(lo) - 2, nxe = gx_floori(hi) + 3;
        if (nxs > *pxs) *pxs = nxs;
        if (nxe < *pxe) *pxe = nxe;
    }
    return *pxs <= *pxe;
}

static void gx_span_fast(const GxPrim *p, const FastProg *f, FastCtx *ctx, int y, int xs, int xe) {
    float rowF[3];
    uint32_t base[5];
    int k, xb, useZ, zw, qOne;
    uint8_t *crow, *zrow;
    /* constantes do combinador e do blender (preenchidas so quando mudam) */
    uint8_t (*cV)[FB] = ctx->cV;
    Chan4 *cBl = ctx->cBl;
    uint8_t *c255 = ctx->c255;
    /* vetores por pixel (16 bytes de folga: os lacos NEON trabalham em grupos de 8/16) */
    int32_t idx[FB], z14[FB];
    uint8_t sh[4][FB], tc[2][4][FB], comb[4][FB], cc1[4][FB], mem[3][FB], mid[3][FB], rgb[3][FB];
    uint8_t pass[FB];
    float S[FB], T[FB];
    uint32_t tp[FB];
    const uint8_t *V[V_N];

    if (f->dead) {
        return;
    }
    if (f->crop && !ctx->covW && !ctx->covR && !span_crop(p, f, y, &xs, &xe)) {
        return; /* (so a parte vazia da textura) */
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (f->fused) {
        gx_span_fused(p, f, ctx, y, xs, xe);
        return;
    }
#endif
    for (k = 0; k < 3; k++) {
        rowF[k] = p->f0[k] + p->fdy[k] * ((float)y - p->fy0);
    }
    qOne = p->fdx[2] == 0.0f && p->fdy[2] == 0.0f && p->f0[2] == 1.0f;
    for (k = 0; k < 5; k++) {
        base[k] = (uint32_t)(int32_t)(p->c[k] + (int64_t)p->dy[k] * y + (int64_t)p->dx[k] * xs);
    }
    crow = f->cimg + (size_t)y * (size_t)f->cwidth * 2u;
    zrow = f->zimg ? f->zimg + (size_t)y * (size_t)f->cwidth * 2u : NULL;
    useZ = p->useZ && zrow != NULL;
    zw = useZ && f->zupd;

    /* entradas do combinador: constantes (so as usadas; o vetor so e preenchido de novo se o valor mudou) */
    for (k = 0; k < V_N; k++) {
        V[k] = g_zero; /* V_ZERO, V_LODF, as nao usadas e COMBINED no 1o ciclo sao 0 */
    }
    for (k = 0; k < f->nConst; k++) {
        const int vi = f->cIdx[k];
        const int32_t val = f->cVal[k];
        if (ctx->cHave[vi] != val + 1) {
            fill_u8(cV[vi], val, FB);
            ctx->cHave[vi] = val + 1;
        }
        V[vi] = cV[vi];
    }
    for (k = 0; k < 4; k++) {
        V[V_SHADE + k] = sh[k];
        V[V_T0 + k] = tc[0][k];
        V[V_T1 + k] = tc[1][k];
    }
    if (f->cyc2) {
        for (k = 0; k < 4; k++) {
            V[V_COMB + k] = comb[k]; /* trocado pelo resultado do 1o ciclo */
        }
    }
    /* constantes do blender */
    {
        int c, nc = f->cyc2 ? 2 : 1;
        for (c = 0; c < nc; c++) {
            if (f->bl[c][0] >= 2 || f->bl[c][2] >= 2) {
                for (k = 0; k < 3; k++) {
                    if (ctx->blHave[0][k] != f->blendc[k] + 1) {
                        fill_u8(cBl[0].v[k], f->blendc[k], FB);
                        ctx->blHave[0][k] = f->blendc[k] + 1;
                    }
                    if (ctx->blHave[1][k] != f->fogc[k] + 1) {
                        fill_u8(cBl[1].v[k], f->fogc[k], FB);
                        ctx->blHave[1][k] = f->fogc[k] + 1;
                    }
                }
            }
            if (f->bl[c][1] == 1 && ctx->blHave[1][3] != f->fogc[3] + 1) {
                fill_u8(cBl[1].v[3], f->fogc[3], FB);
                ctx->blHave[1][3] = f->fogc[3] + 1;
            }
        }
        if (f->alphaCvgSel && !f->cvgXAlpha && !ctx->c255Have) {
            fill_u8(c255, 255, FB);
            ctx->c255Have = 1;
        }
    }

    for (xb = xs; xb <= xe; xb += FB) {
        int n = xe - xb + 1 < FB ? xe - xb + 1 : FB;
        int cnt, j, dense, allPass = 0;
        const uint8_t *ccp[4], *aIn, *mp[3], *out[3];
        /* --- 1. profundidade: lista dos pixels vivos --- */
        if (useZ) {
            const uint32_t bz = base[GXP_Z], dz = (uint32_t)p->dx[GXP_Z];
            const int tol = f->zdec ? p->zTol : 0;
            const int zcmp = f->zcmp;
            const uint8_t *zp = zrow + (size_t)xb * 2u;
            cnt = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            {
                uint32_t ok[FB];
                if (z_test_neon(bz, dz, zp, n, zcmp, f->zdec, (uint32_t)tol, z14, ok)) {
                    memcpy(idx, k_ident, sizeof idx);
                    cnt = n;
                } else {
                    /* compacta no lugar (cnt <= j sempre) */
                    for (j = 0; j < n; j++) {
                        idx[cnt] = j;
                        z14[cnt] = z14[j];
                        cnt += (int)ok[j];
                    }
                }
            }
#else
            if (f->zdec) {
                for (j = 0; j < n; j++) {
                    int32_t zv = ((int32_t)(bz + dz * (uint32_t)j) + 128) >> 8;
                    uint32_t z18 = (uint32_t)(zv < 0 ? 0 : (zv > 0x3FFFF ? 0x3FFFF : zv));
                    uint32_t zm = rd_be16(zp + 2 * j) >> 2;
                    idx[cnt] = j;
                    z14[cnt] = (int32_t)gx_zcomp(z18);
                    cnt += !zcmp || z18 <= gx_zdecomp(zm) + (uint32_t)tol;
                }
            } else {
                for (j = 0; j < n; j++) {
                    int32_t zv = ((int32_t)(bz + dz * (uint32_t)j) + 128) >> 8;
                    uint32_t zz = gx_zcomp((uint32_t)(zv < 0 ? 0 : (zv > 0x3FFFF ? 0x3FFFF : zv)));
                    uint32_t zm = rd_be16(zp + 2 * j) >> 2;
                    idx[cnt] = j;
                    z14[cnt] = (int32_t)zz;
                    cnt += !zcmp || zz <= zm;
                }
            }
#endif
        } else {
            memcpy(idx, k_ident, sizeof idx);
            cnt = n;
        }
        if (cnt == 0) {
            goto next;
        }
        dense = cnt == n; /* todos os pixels do bloco passaram: idx[j] == j (lacos contiguos, NEON) */
        /* --- 2. sombreamento --- */
        for (k = 0; k < 4; k++) {
            if (f->needShade[k]) {
                const uint32_t b = base[k], d = (uint32_t)p->dx[k];
                uint8_t *restrict o = sh[k];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                {
                    /* b + d*idx de 8 em 8 (idx tem folga ate FB), arredonda, prende em 0..255 */
                    const uint32x4_t vb = vdupq_n_u32(b);
                    const int32x4_t h = vdupq_n_s32(0x8000);
                    for (j = 0; j < cnt; j += 8) {
                        uint32x4_t v0 = vmlaq_n_u32(vb, vreinterpretq_u32_s32(vld1q_s32(idx + j)), d);
                        uint32x4_t v1 = vmlaq_n_u32(vb, vreinterpretq_u32_s32(vld1q_s32(idx + j + 4)), d);
                        int32x4_t x0 = vshrq_n_s32(vaddq_s32(vreinterpretq_s32_u32(v0), h), 16);
                        int32x4_t x1 = vshrq_n_s32(vaddq_s32(vreinterpretq_s32_u32(v1), h), 16);
                        vst1_u8(o + j, vqmovn_u16(vcombine_u16(vqmovun_s32(x0), vqmovun_s32(x1))));
                    }
                    continue;
                }
#endif
                for (j = 0; j < cnt; j++) {
                    o[j] = (uint8_t)clamp255i(((int32_t)(b + d * (uint32_t)idx[j]) + 0x8000) >> 16);
                }
            }
        }
        /* --- 3. texturas --- */
        if (f->useT[0] || f->useT[1]) {
            if (p->hasTex) {
                const float fx0 = p->fx0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                /* 4 pixels por vez (idx tem folga ate FB: as pistas a mais sao descartadas). Com perspectiva,
                 * 1/q vem da estimativa do NEON refinada duas vezes (Newton): erro de ~1 ulp em vez da divisao
                 * exata, que no Cortex-A7 custa ~15 ciclos por pixel. gx_fast_exact = 1 volta a divisao. */
                if (qOne || !gx_fast_exact) {
                    const int32x4_t vxb = vdupq_n_s32(xb);
                    const float32x4_t vfx0 = vdupq_n_f32(fx0), r0 = vdupq_n_f32(rowF[0]), r1 = vdupq_n_f32(rowF[1]);
                    const float32x4_t d0 = vdupq_n_f32(p->fdx[0]), d1 = vdupq_n_f32(p->fdx[1]);
                    const float32x4_t r2 = vdupq_n_f32(rowF[2]), d2 = vdupq_n_f32(p->fdx[2]), tiny = vdupq_n_f32(1e-12f);
                    for (j = 0; j < cnt; j += 4) {
                        float32x4_t fx = vsubq_f32(vcvtq_f32_s32(vaddq_s32(vxb, vld1q_s32(idx + j))), vfx0);
                        float32x4_t s = vaddq_f32(r0, vmulq_f32(d0, fx)), t = vaddq_f32(r1, vmulq_f32(d1, fx));
                        if (!qOne) {
                            float32x4_t q = vaddq_f32(r2, vmulq_f32(d2, fx)), rq;
                            q = vbslq_f32(vcltq_f32(vabsq_f32(q), tiny), tiny, q);
                            rq = vrecpeq_f32(q);
                            rq = vmulq_f32(rq, vrecpsq_f32(q, rq));
                            rq = vmulq_f32(rq, vrecpsq_f32(q, rq));
                            s = vmulq_f32(s, rq);
                            t = vmulq_f32(t, rq);
                        }
                        vst1q_f32(S + j, s);
                        vst1q_f32(T + j, t);
                    }
                } else
#endif
                if (qOne) {
                    for (j = 0; j < cnt; j++) {
                        float fx = (float)(xb + idx[j]) - fx0;
                        S[j] = rowF[0] + p->fdx[0] * fx;
                        T[j] = rowF[1] + p->fdx[1] * fx;
                    }
                } else {
                    for (j = 0; j < cnt; j++) {
                        float fx = (float)(xb + idx[j]) - fx0;
                        float q = rowF[2] + p->fdx[2] * fx, rq;
                        if (fabsf(q) < 1e-12f) {
                            q = 1e-12f;
                        }
                        rq = 1.0f / q;
                        S[j] = (rowF[0] + p->fdx[0] * fx) * rq;
                        T[j] = (rowF[1] + p->fdx[1] * fx) * rq;
                    }
                }
            } else {
                for (j = 0; j < cnt; j++) {
                    S[j] = 0.0f;
                    T[j] = 0.0f;
                }
            }
            for (k = 0; k < 2; k++) {
                if (!f->useT[k]) {
                    continue;
                }
                f->samp[k](&f->t[k], S, T, tp, cnt);
                tex_unpack(tp, tc[k], f->needTC[k], cnt);
            }
        }
        /* --- 4. combinador --- */
        if (f->cyc2) {
            for (k = 0; k < 4; k++) {
                const uint8_t *c = f->c0[k];
                if (!f->need0[k]) {
                    V[V_COMB + k] = g_zero;
                } else if (f->k0[k]) {
                    V[V_COMB + k] = V[c[3]];
                } else {
                    cc_loop(comb[k], V[c[0]], V[c[1]], V[c[2]], V[c[3]], cnt);
                    V[V_COMB + k] = comb[k];
                }
            }
        }
        for (k = 0; k < 4; k++) {
            const uint8_t *c = f->c1[k];
            if (!f->need1[k]) {
                ccp[k] = g_zero;
            } else if (f->k1[k]) {
                ccp[k] = V[c[3]];
            } else {
                cc_loop(cc1[k], V[c[0]], V[c[1]], V[c[2]], V[c[3]], cnt);
                ccp[k] = cc1[k];
            }
        }
        /* --- 5. alfa / cobertura --- */
        if (f->cvgXAlpha || f->ac == 1 || f->ac == 3) {
            const uint8_t *restrict a = ccp[3];
            const int cvg = f->cvgXAlpha, ac = f->ac;
            const int blA = f->blendc[3];
            int any = 0;
            for (j = 0; j < cnt; j++) {
                int ok = 1;
                if (cvg) {
                    ok &= (a[j] >> 5) != 0;
                }
                if (ac == 1) {
                    ok &= a[j] >= blA;
                } else if (ac == 3) {
                    ok &= a[j] >= bayer4[y & 3][(xb + idx[j]) & 3] * 16 + 8;
                }
                pass[j] = (uint8_t)ok;
                any |= ok;
            }
            if (!any) {
                goto next;
            }
        } else {
            memset(pass, 1, (size_t)cnt);
            allPass = 1;
        }
        aIn = (f->alphaCvgSel && !f->cvgXAlpha) ? c255 : ccp[3];
        /* --- 6. blender --- */
        if (f->needMem) {
            for (j = 0; j < cnt; j++) {
                int32_t c = rd_c16(crow + (size_t)(xb + idx[j]) * 2u, f->cle);
                int32_t r5 = (c >> 11) & 31, g5 = (c >> 6) & 31, b5 = (c >> 1) & 31;
                const int rb = f->cdith ? 0 : 2; /* (com pontilhado: sem repetir os bits, como o RDP) */
                mem[0][j] = (uint8_t)((r5 << 3) | (rb ? r5 >> 2 : 0));
                mem[1][j] = (uint8_t)((g5 << 3) | (rb ? g5 >> 2 : 0));
                mem[2][j] = (uint8_t)((b5 << 3) | (rb ? b5 >> 2 : 0));
            }
            mp[0] = mem[0];
            mp[1] = mem[1];
            mp[2] = mem[2];
        } else {
            mp[0] = mp[1] = mp[2] = g_zero;
        }
        {
            const uint8_t *shA = sh[3];
            if (f->cyc2) {
                const uint8_t *midp[3];
                const uint8_t *A0 = f->bl[0][1] == 0 ? aIn : f->bl[0][1] == 1 ? cBl[1].v[3] : f->bl[0][1] == 2 ? shA : g_zero;
                for (k = 0; k < 3; k++) {
                    midp[k] = bl_chan(mid[k], f->bl[0][1], f->bl[0][3], bl_sel(f->bl[0][0], k, ccp, mp, cBl),
                                      bl_sel(f->bl[0][2], k, ccp, mp, cBl), A0, cnt);
                }
                if (f->forceBl) {
                    const uint8_t *A1 = f->bl[1][1] == 0 ? aIn : f->bl[1][1] == 1 ? cBl[1].v[3] : f->bl[1][1] == 2 ? shA : g_zero;
                    for (k = 0; k < 3; k++) {
                        out[k] = bl_chan(rgb[k], f->bl[1][1], f->bl[1][3], bl_sel(f->bl[1][0], k, midp, mp, cBl),
                                         bl_sel(f->bl[1][2], k, midp, mp, cBl), A1, cnt);
                    }
                } else {
                    for (k = 0; k < 3; k++) {
                        out[k] = bl_sel(f->bl[1][0], k, midp, mp, cBl);
                    }
                }
            } else if (f->forceBl) {
                const uint8_t *A0 = f->bl[0][1] == 0 ? aIn : f->bl[0][1] == 1 ? cBl[1].v[3] : f->bl[0][1] == 2 ? shA : g_zero;
                for (k = 0; k < 3; k++) {
                    out[k] = bl_chan(rgb[k], f->bl[0][1], f->bl[0][3], bl_sel(f->bl[0][0], k, ccp, mp, cBl),
                                     bl_sel(f->bl[0][2], k, ccp, mp, cBl), A0, cnt);
                }
            } else {
                /* sem mistura: a 1a cor do blender (MEM le a memoria) */
                for (k = 0; k < 3; k++) {
                    out[k] = bl_sel(f->bl[0][0], k, ccp, mp, cBl);
                }
            }
        }
        /* --- 7. escrita --- */
        {
            const uint8_t *restrict r = out[0], *restrict g = out[1], *restrict b = out[2];
            uint16_t c16[FB];
            uint8_t dr[FB + 16], dg[FB + 16], db[FB + 16];
            if (f->cdith) {
                /* pontilhado da cor (gfx_tri.c: gx_dith) antes de cortar para 5 bits */
                extern const uint8_t gx_dith[3][4][4];
                const uint8_t *drow = gx_dith[f->cdith][y & 3];
                for (j = 0; j < cnt; j++) {
                    const int d = drow[(xb + idx[j]) & 3];
                    dr[j] = (uint8_t)(r[j] + d > 255 ? 255 : r[j] + d);
                    dg[j] = (uint8_t)(g[j] + d > 255 ? 255 : g[j] + d);
                    db[j] = (uint8_t)(b[j] + d > 255 ? 255 : b[j] + d);
                }
                r = dr;
                g = dg;
                b = db;
            }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (dense && allPass) {
                /* bloco inteiro: cor (e profundidade) gravadas de 8 em 8, ja em big-endian */
                uint8_t *cp = crow + (size_t)xb * 2u, *zq = zrow + (size_t)xb * 2u;
                const uint16x8_t one16 = vdupq_n_u16(1);
                for (j = 0; j + 8 <= cnt; j += 8) {
                    uint16x8_t c = vshlq_n_u16(vmovl_u8(vshr_n_u8(vld1_u8(r + j), 3)), 11);
                    c = vorrq_u16(c, vshlq_n_u16(vmovl_u8(vshr_n_u8(vld1_u8(g + j), 3)), 6));
                    c = vorrq_u16(c, vshlq_n_u16(vmovl_u8(vshr_n_u8(vld1_u8(b + j), 3)), 1));
                    c = vorrq_u16(c, one16);
                    vst1q_u8(cp + 2 * j, crev(vreinterpretq_u8_u16(c), f->cle));
                    if (zw) {
                        uint16x8_t z = vcombine_u16(vmovn_u32(vshlq_n_u32(vreinterpretq_u32_s32(vld1q_s32(z14 + j)), 2)),
                                                    vmovn_u32(vshlq_n_u32(vreinterpretq_u32_s32(vld1q_s32(z14 + j + 4)), 2)));
                        vst1q_u8(zq + 2 * j, vrev16q_u8(vreinterpretq_u8_u16(z)));
                    }
                }
                for (; j < cnt; j++) {
                    wr_c16(cp + 2 * j, (uint32_t)(((r[j] >> 3) << 11) | ((g[j] >> 3) << 6) | ((b[j] >> 3) << 1) | 1), f->cle);
                    if (zw) {
                        wr_be16(zq + 2 * j, (uint32_t)z14[j] << 2);
                    }
                }
                goto next;
            }
#endif
            for (j = 0; j < cnt; j++) {
                c16[j] = (uint16_t)(((r[j] >> 3) << 11) | ((g[j] >> 3) << 6) | ((b[j] >> 3) << 1) | 1);
            }
            if (zw) {
                for (j = 0; j < cnt; j++) {
                    if (pass[j]) {
                        size_t o = (size_t)(xb + idx[j]) * 2u;
                        wr_c16(crow + o, c16[j], f->cle);
                        wr_be16(zrow + o, (uint32_t)z14[j] << 2);
                    }
                }
            } else {
                for (j = 0; j < cnt; j++) {
                    if (pass[j]) {
                        wr_c16(crow + (size_t)(xb + idx[j]) * 2u, c16[j], f->cle);
                    }
                }
            }
        }
    next:
        for (k = 0; k < 5; k++) {
            base[k] += (uint32_t)p->dx[k] * (uint32_t)FB;
        }
    }
}

/* ------------------------------------------------------------------ fila de primitivas e threads
 *
 * Com o caminho rapido ligado, triangulos e retangulos nao sao desenhados na hora: entram numa fila com
 * uma copia do estado (FastProg). A fila e desenhada no fim da tarefa ou antes de qualquer coisa que
 * dependa do resultado (carga de TMEM lendo um buffer desenhado, textura saindo da cache, modo copia,
 * caminho de referencia). Com gx_threads = 2 cada thread desenha metade das linhas (faixas de 2 linhas
 * alternadas); como cada linha e desenhada na mesma ordem da fila, o resultado e identico ao de 1 thread.
 */
#include <pthread.h>
#include <time.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "../perf_trace.h"

int gx_threads = 1;
/* testes de agendamento (oot_replay / opcoes): nucleo fixo da thread auxiliar (-1 = livre) e quanto
 * tempo (microssegundos) ela espera acordada por mais trabalho antes de dormir (0 = dorme na hora) */
int gx_helper_cpu = -1;
int gx_helper_spin_us = 0;
/* 1 = a thread auxiliar desenha a fila enquanto ela ainda e montada; 0 = so no fim (como antes) */
int gx_stream = 1;

enum { JOB_TRI, JOB_RECT, JOB_FILL, JOB_BGEND };
#define JOBF_DEFER 1 /* fundo adiado: so desenhado no JOB_BGEND */
#define JOBF_COVW 2  /* marca na mascara os pixels que escreve */
#define JOBF_HDF 4   /* textura nova ampliada (perto da camera): filtro leve (ver hd_close) */
#define JOBF_LAZY 8  /* triangulo ainda nao montado: raw tem os vertices; quem desenhar primeiro monta (tri_ready) */

/* opcao lazy_triangle_setup: a montagem dos triangulos (gradientes, arestas) sai da thread do desenho e vai para a
 * fila: quem desenha o triangulo primeiro monta (gfx_tri.c tri_setup). A thread do desenho so le os comandos e
 * transforma os vertices; a auxiliar, que esperava por ela, faz a maior parte. Imagem igual (as mesmas contas). */
int gx_tri_lazy = 0; /* (medido no aparelho: sem ganho - os triangulos com o filtro exato de perto montam na hora) */

typedef struct {
    int kind;
    int flags;          /* JOBF_* */
    int prog;           /* indice em g_progs */
    GxPrim P;
    TriEdges g;         /* triangulo */
    TriRaw raw;         /* (JOBF_LAZY) o triangulo como chegou */
    int lys, lye;       /* (JOBF_LAZY) linhas, ja sabidas na fila */
    int setup;          /* (JOBF_LAZY) 0 por montar, 1 montando, 2 pronto, 3 vazio */
    uint32_t zr;        /* (JOBF_LAZY) profundidade mais proxima (ordenacao dos opacos) */
    int x0, y0, x1, y1; /* retangulo / preenchimento */
    uint32_t fill;      /* cor de preenchimento */
    uint8_t *cimg;      /* preenchimento */
    int cwidth;
} GxJob;

/* Capacidade fixa: com 2 threads a auxiliar desenha a fila ENQUANTO a thread do desenho ainda acrescenta
 * primitivas (nada pode mudar de lugar na memoria). Cheia -> desenha a fila e recomeca. */
#define JOB_CAP 4096
#define PROG_CAP 1024
static GxJob g_jobs[JOB_CAP];
static int g_nJobs;
static FastProg g_progs[PROG_CAP];
static int g_nProgs;
static int g_pub;             /* quantos trabalhos ja estao completos (a auxiliar pode desenhar) */
static uint32_t g_progGen; /* g_pipeGen do ultimo programa da fila */
static struct {
    uintptr_t a, b;
} g_tgt[8]; /* buffers escritos pela fila */
static int g_nTgt;
static FastCtx g_ctx[2];

static int fast_ok(void) {
    return gx_fast && !gx.nProbe && gx.cimg.siz == 2 && gx.cimg.addr != NULL;
}

static void batch_target(const uint8_t *p, size_t n) {
    uintptr_t a = (uintptr_t)p, b = a + n;
    int i;
    if (p == NULL) {
        return;
    }
    for (i = 0; i < g_nTgt; i++) {
        if (g_tgt[i].a == a) {
            if (b > g_tgt[i].b) {
                g_tgt[i].b = b;
            }
            return;
        }
    }
    if (g_nTgt == 8) {
        /* muitos buffers: junta tudo num intervalo so (fica so mais conservador) */
        for (i = 1; i < g_nTgt; i++) {
            if (g_tgt[i].a < g_tgt[0].a) g_tgt[0].a = g_tgt[i].a;
            if (g_tgt[i].b > g_tgt[0].b) g_tgt[0].b = g_tgt[i].b;
        }
        g_nTgt = 1;
        if (a < g_tgt[0].a) g_tgt[0].a = a;
        if (b > g_tgt[0].b) g_tgt[0].b = b;
        return;
    }
    g_tgt[g_nTgt].a = a;
    g_tgt[g_nTgt].b = b;
    g_nTgt++;
}

static int cur_prog(void) {
    if (g_fp.gen != g_pipeGen) {
        fast_build();
    }
    if (g_nProgs == 0 || g_progGen != g_fp.gen) {
        g_progs[g_nProgs++] = g_fp; /* batch_reserve garantiu espaco */
        g_progGen = g_fp.gen;
    }
    return g_nProgs - 1;
}

static void batch_start(void);
static void batch_publish(void);

/* ---- opacos do mais perto para o mais longe (opcao ordenar_opacos; ideia do port do Mario Kart 64) ----
 * Triangulos seguidos que so escrevem cor nova (sem ler a tela) e gravam profundidade podem ser desenhados
 * em qualquer ordem: o teste de profundidade da o mesmo resultado (salvo pixels com profundidade
 * exatamente igual). Desenhando os mais proximos primeiro, os de tras falham no teste antes de ler
 * textura e pintar. A sequencia fica guardada (sem publicar para a auxiliar) ate aparecer outro tipo de
 * trabalho; ai e ordenada pela profundidade mais proxima de cada triangulo e publicada. */
int gx_sort_opaque = 1;
int gx_sort_chunk = 160; /* >0: a sequencia ordenada e publicada a cada N triangulos (a auxiliar comeca antes) */
static int g_runFrom = -1;
/* ordem de desenho: run_jobs desenha g_jobs[g_ord[i]] (os trabalhos nao mudam de lugar; so a ordem e trocada) */
static uint16_t g_ord[JOB_CAP];
static uint64_t g_sortKey[JOB_CAP];

static void wake_helper(void);

/* profundidade mais proxima do triangulo (z dos 3 cantos), presa em 0..0xFFFFFFFF */
static uint32_t job_zmin(const GxJob *j) {
    float best = 4.0e9f;
    int k;
    if (j->flags & JOBF_LAZY) {
        return j->zr;
    }
    for (k = 0; k < 3; k++) {
        const float z = (float)j->P.c[GXP_Z] + (float)j->P.dx[GXP_Z] * (float)j->g.px[k] + (float)j->P.dy[GXP_Z] * (float)j->g.py[k];
        if (z < best) {
            best = z;
        }
    }
    return best <= 0.0f ? 0u : best >= 4.0e9f ? 0xFFFFFFFFu : (uint32_t)best;
}

static int sort_cmp(const void *a, const void *b) {
    const uint64_t ka = *(const uint64_t *)a, kb = *(const uint64_t *)b;
    return ka < kb ? -1 : ka > kb;
}

static void run_close(void) {
    const int from = g_runFrom;
    int n, i;
    if (from < 0) {
        return;
    }
    g_runFrom = -1;
    n = g_nJobs - from;
    if (n > 1) {
        /* chave = profundidade << 16 | posicao: mesma profundidade fica na ordem do jogo */
        for (i = 0; i < n; i++) {
            g_sortKey[i] = (uint64_t)job_zmin(&g_jobs[from + i]) << 16 | (uint64_t)(from + i);
        }
        qsort(g_sortKey, (size_t)n, sizeof(uint64_t), sort_cmp);
        for (i = 0; i < n; i++) {
            g_ord[from + i] = (uint16_t)(g_sortKey[i] & 0xFFFFu);
        }
    }
    batch_publish();
    if (n > 1) {
        wake_helper(); /* o lote inteiro ficou pronto de uma vez: acorda a auxiliar ja */
    }
}

/* garante lugar para 2 trabalhos (o 2o para um eventual JOB_BGEND) e 1 programa */
static void batch_reserve(void) {
    if (g_nJobs >= JOB_CAP - 2 || g_nProgs >= PROG_CAP) {
        gx_batch_flush();
    }
}

static GxJob *batch_push(int kind) {
    GxJob *j;
    if (kind != JOB_TRI) {
        run_close();
    }
    if (g_nJobs == 0 && gx_stream) {
        batch_start();
    }
    g_ord[g_nJobs] = (uint16_t)g_nJobs;
    j = &g_jobs[g_nJobs++];
    j->kind = kind;
    j->flags = 0;
    return j;
}

/* ---- fundo adiado (como o ceu do Tomb Raider 2 no OpenLara) ----
 * O jogo desenha primeiro o fundo (ceu, cor de fundo: sem profundidade e sem ler a tela) e depois a cena
 * por cima, pintando muitos pixels duas vezes. Aqui os trabalhos do fundo ficam guardados (JOBF_DEFER); os
 * seguintes, enquanto nenhum le a cor da tela, desenham normalmente e marcam numa mascara os pixels que
 * escreveram (JOBF_COVW). No fim da janela (JOB_BGEND) o fundo e desenhado so onde nada foi escrito. Como
 * nada na janela le a cor e o fundo nao mexe na profundidade, a imagem final e identica.
 * Estados: 0 sem fundo; 1 juntando trabalhos do fundo; 2 janela aberta (fundo guardado). */
int gx_bgdefer = 1;
int gx_bgmix = 1; /* 1 = camadas misturadas do fundo tambem adiadas */
static int g_bgState, g_bgFirst, g_bgLast, g_bgY0, g_bgY1;
static const uint8_t *g_bgCimg;

static void bg_end(void) {
    GxJob *j;
    run_close();
    if (g_bgState == 0) {
        return;
    }
#ifdef GX_REPLAY_DIAG
    if (getenv("BGDBG")) {
        fprintf(stderr, "[fundo] fecha: estado %d, trabalhos adiados %d..%d, linhas %d..%d | quem fechou: tex %08x omL %08x comb %08x %08x\n",
                g_bgState, g_bgFirst, g_bgLast, g_bgY0, g_bgY1, gx.timgRaw, gx.omL, gx.combW0, gx.combW1);
    }
#endif
    g_bgState = 0;
    g_ord[g_nJobs] = (uint16_t)g_nJobs;
    j = &g_jobs[g_nJobs++]; /* batch_reserve deixou lugar */
    j->kind = JOB_BGEND;
    j->flags = 0;
    j->prog = -1;
    j->x0 = g_bgFirst;
    j->x1 = g_bgLast;
    j->y0 = g_bgY0; /* linhas que a janela marcou (y0 > y1: nenhuma) */
    j->y1 = g_bgY1;
    batch_publish();
}

/* decide, antes de enfileirar um trabalho de desenho, como ele entra no fundo adiado (devolve JOBF_*) */
static int bg_pre(const FastProg *f, int useZ, int ys, int ye) {
    int cand, compat;
    if (f->dead) {
        return 0; /* nao escreve nada */
    }
#ifdef GX_REPLAY_DIAG
    if (getenv("BGDBG")) {
        static int nn;
        if (nn++ < 4) {
            fprintf(stderr, "[fundo] trabalho %d: fused %d needMem %d useZ %d tex %08x omL %08x\n", g_nJobs, f->fused,
                    f->needMem, useZ && f->zimg != NULL, gx.timgRaw, gx.omL);
        }
    }
#endif
    compat = gx_bgdefer && f->fused && !f->needMem && f->cimg == g_bgCimg && f->cwidth <= COV_W - 16 && ye < COV_H;
    /* camadas do fundo que misturam com o que ja esta na tela (nuvens por cima do ceu) tambem ficam
     * guardadas, desde que venham logo depois do primeiro trabalho do fundo: no fim da janela elas sao
     * desenhadas na ordem, so onde nada cobriu, e leem so o fundo */
    cand = gx_bgdefer && f->fused && (!f->needMem || (gx_bgmix && g_bgState == 1 && f->cimg == g_bgCimg)) &&
           !(useZ && f->zimg != NULL) && f->cwidth <= COV_W - 16 && ye < COV_H && f->cimg != NULL;
    if (g_bgState == 1) {
        if (cand && f->cimg == g_bgCimg) {
            g_bgLast = g_nJobs;
            return JOBF_DEFER;
        }
        if (compat) {
            g_bgState = 2;
            g_bgY0 = ys < 0 ? 0 : ys;
            g_bgY1 = ye;
            return JOBF_COVW;
        }
        bg_end();
    } else if (g_bgState == 2) {
        if (compat) {
            if (ys < g_bgY0) {
                g_bgY0 = ys < 0 ? 0 : ys;
            }
            if (ye > g_bgY1) {
                g_bgY1 = ye;
            }
            return JOBF_COVW;
        }
        bg_end();
    }
    if (cand) {
#ifdef GX_REPLAY_DIAG
        if (getenv("BGDBG")) {
            fprintf(stderr, "[fundo] abre no trabalho %d: tex %08x omL %08x\n", g_nJobs, gx.timgRaw, gx.omL);
        }
#endif
        g_bgState = 1;
        g_bgFirst = g_bgLast = g_nJobs;
        g_bgCimg = f->cimg;
        g_bgY0 = 1;
        g_bgY1 = 0;
        return JOBF_DEFER;
    }
    return 0;
}

/* textura nova (2x, lida pelo texel mais proximo) ampliada neste triangulo? Em cada vertice: texels da nova
 * por pixel na tela (derivadas de S/q e T/q). Se em algum vertice um texel ocupa gx_hd_perto pixels ou mais,
 * o triangulo usa a versao com filtro leve (e a tabela filtrada da textura e feita agora, na thread que
 * monta a fila: as que desenham so leem). */
static int hd_close(const GxPrim *p, FastProg *f, const TriEdges *g) {
    const float lim = gx_hd_perto > 0 ? 32.0f / (float)gx_hd_perto : 0.0f; /* em 1/32 de texel por pixel */
    const float limX = gx_filtro_exato_perto > 0 ? 32.0f / (float)gx_filtro_exato_perto : 0.0f; /* (filtro exato) */
    int v, k, close = 0;
    for (v = 0; v < 3 && !close; v++) {
        const float fx = g->px[v] - p->fx0, fy = g->py[v] - p->fy0;
        const float q = p->f0[2] + p->fdx[2] * fx + p->fdy[2] * fy;
        float s, t, ds, dt;
        if (fabsf(q) < 1e-12f) {
            continue;
        }
        s = (p->f0[0] + p->fdx[0] * fx + p->fdy[0] * fy) / q;
        t = (p->f0[1] + p->fdx[1] * fx + p->fdy[1] * fy) / q;
        ds = fmaxf(fabsf((p->fdx[0] - s * p->fdx[2]) / q), fabsf((p->fdy[0] - s * p->fdy[2]) / q));
        dt = fmaxf(fabsf((p->fdx[1] - t * p->fdx[2]) / q), fabsf((p->fdy[1] - t * p->fdy[2]) / q));
        for (k = 0; k < 2; k++) {
            if (f->sampnF[k] != NULL && fmaxf(ds * f->tF[k].scaleS, dt * f->tF[k].scaleT) < (f->fExact[k] ? limX : lim)) {
                close = 1;
            }
        }
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (close && gx_tex_prefilter) {
        for (k = 0; k < 2; k++) {
            if (f->sampnF[k] != NULL && f->fExact[k] == 1 && gx_filtro_perto_fino && tex_prefilter_x(&f->tF[k], 1)) {
                /* de perto: a tabela 4x4 da textura (passos de 1/4 de texel; com um texel em 4+ pixels quase nao se
                 * ve degrau) - tao rapida quanto a principal; o filtro exato fica so se nao houver memoria */
                f->sampnF[k] = g_sampf[2][f->tF[k].modeS == 2][f->tF[k].modeT == 2];
                f->fExact[k] = 2;
            }
            if (f->sampnF[k] != NULL && !f->fExact[k] && f->tF[k].pf == NULL && tex_prefilter(&f->tF[k])) {
                f->sampnF[k] = g_sampf[f->tF[k].pfs >= 1 && f->tF[k].pfs <= 3 ? f->tF[k].pfs : 1][f->tF[k].modeS == 2][f->tF[k].modeT == 2];
            }
        }
    }
#endif
    return close;
}

/* gx_draw_tri monta o triangulo direto no proximo lugar da fila (ainda nao publicado: a auxiliar nao le) */
static void fast_slot(GxPrim **p, TriEdges **g) {
    batch_reserve(); /* (batch_tri reserva de novo: com o lugar garantido, nao esvazia a fila no meio) */
    *p = &g_jobs[g_nJobs].P;
    *g = &g_jobs[g_nJobs].g;
}

static void batch_tri(const GxPrim *p, const TriEdges *g) {
    GxJob *j;
    int prog;
    int fl;
    batch_reserve();
    prog = cur_prog();
    fl = bg_pre(&g_progs[prog], p->useZ, g->ys, g->ye);
    {
        const FastProg *f = &g_progs[prog];
        const int sortable = gx_sort_opaque && !(fl & JOBF_DEFER) && p->useZ && f->zimg != NULL && f->zcmp &&
                             f->zupd && !f->zdec && !f->needMem && !f->dead;
        if (!sortable || (gx_sort_chunk > 0 && g_runFrom >= 0 && g_nJobs - g_runFrom >= gx_sort_chunk)) {
            run_close();
        }
        if ((f->sampnF[0] != NULL || f->sampnF[1] != NULL) && hd_close(p, &g_progs[prog], g)) {
            fl |= JOBF_HDF;
        }
        j = batch_push(JOB_TRI);
        j->flags = fl;
        j->prog = prog;
        if (&j->P != p) {
            j->P = *p; /* (fast_slot: ja montado no lugar) */
        }
        if (&j->g != g) {
            j->g = *g;
        }
        if (sortable) {
            if (g_runFrom < 0) {
                g_runFrom = g_nJobs - 1;
            }
        } else {
            batch_publish();
        }
    }
    batch_target(gx.cimg.addr, (size_t)(g->ye + 1) * gx.cimg.width * 2u);
    if (p->useZ && gx.zimg != NULL) {
        batch_target(gx.zimg, (size_t)(g->ye + 1) * gx.cimg.width * 2u);
    }
}

static void batch_rect(const GxPrim *p, int x0, int y0, int x1, int y1) {
    GxJob *j;
    int prog;
    int fl;
    batch_reserve();
    prog = cur_prog();
    fl = bg_pre(&g_progs[prog], p->useZ, y0, y1);
    j = batch_push(JOB_RECT);
    j->flags = fl;
    j->prog = prog;
    j->P = *p;
    j->x0 = x0;
    j->y0 = y0;
    j->x1 = x1;
    j->y1 = y1;
    batch_publish();
    batch_target(gx.cimg.addr, (size_t)(y1 + 1) * gx.cimg.width * 2u);
}

int gx_fill_fast(int x0, int y0, int x1, int y1) {
    GxJob *j;
    if (!fast_ok()) {
        return 0;
    }
    batch_reserve();
    if (g_bgState != 0 && gx.cimg.addr == g_bgCimg) {
        bg_end(); /* o preenchimento escreve na tela sem marcar a mascara */
    }
    j = batch_push(JOB_FILL);
    j->prog = -1;
    j->x0 = x0;
    j->y0 = y0;
    j->x1 = x1;
    j->y1 = y1;
    j->fill = gx.cle ? ((gx.fillColor & 0x00FF00FFu) << 8) | ((gx.fillColor >> 8) & 0x00FF00FFu) : gx.fillColor;
    j->cimg = gx.cimg.addr;
    j->cwidth = gx.cimg.width;
    batch_publish();
    batch_target(gx.cimg.addr, (size_t)(y1 + 1) * gx.cimg.width * 2u);
    return 1;
}

/* algo vai ler [p, p+n) da RAM: se a fila escreve ali, desenha a fila antes */
void gx_batch_reading(const void *p, size_t n) {
    uintptr_t a = (uintptr_t)p, b = a + n;
    int i;
    if (g_nJobs == 0 || p == NULL) {
        return;
    }
    for (i = 0; i < g_nTgt; i++) {
        if (a < g_tgt[i].b && g_tgt[i].a < b) {
            gx_batch_flush();
            return;
        }
    }
}

/* Dono de cada par de linhas: g_own[(y >> 1) & 63] (0 = thread do desenho, 1 = auxiliar). A auxiliar
 * desenha enquanto a thread do desenho ainda le os comandos, entao ela recebe mais linhas; a divisao se
 * ajusta sozinha (gx_batch_flush) para as duas terminarem juntas. 64 partes: cada passo muda pouco (com
 * 16 partes a divisao ficava pulando entre duas posicoes e um quadro em cada 2 perdia ~1,4 ms). */
#define OWN_N 64
static uint8_t g_own[OWN_N];
static int g_helperShare = -1; /* pares de linhas (de OWN_N) da auxiliar */

static void set_share(int k) {
    int i, acc = 0;
    g_helperShare = k;
    /* espalha os k pares da auxiliar entre os OWN_N (Bresenham), para as duas pegarem partes parecidas da tela */
    for (i = 0; i < OWN_N; i++) {
        acc += k;
        g_own[i] = acc >= OWN_N ? 1 : 0;
        if (acc >= OWN_N) {
            acc -= OWN_N;
        }
    }
}

static inline __attribute__((always_inline)) int row_mine_t(const uint8_t *own, int y, int band, int nb) {
    return nb == 1 || own[(y >> 1) & (OWN_N - 1)] == band;
}

/* A principal pega trabalho da auxiliar (ideia do port do Hexen II): quando a principal acaba as linhas
 * dela no fim do lote e a auxiliar ainda nao acabou as suas, a principal pede metade das partes de linhas
 * que faltam. A auxiliar responde entre um trabalho e outro (g_stealH = o 1o trabalho que ela nao fez
 * nessas linhas): dali em diante essas linhas (dono 2 em g_ownS) sao da principal. Cada linha continua
 * sendo desenhada por uma thread so e na ordem dos trabalhos, entao a imagem e a mesma.
 * g_stealReq: 0 nada, 1 pedido, 2 aceito. */
int gx_steal = 1;
/* A troca se repete enquanto a auxiliar tiver 2 ou mais partes: a cada rodada r a principal pega metade
 * das partes que ainda sao da auxiliar (dono 2 + r). As tabelas se alternam: a auxiliar so passa para a
 * nova entre dois trabalhos (nunca no meio de um), entao nenhuma linha fica sem dono. */
static uint8_t g_ownS[2][OWN_N];
static const uint8_t *volatile g_ownNext;
static int g_stealReq, g_stealH;
uint64_t gx_steal_ns; /* estatistica: tempo da principal desenhando linhas pegas da auxiliar */
static inline void steal_check(FastCtx *ctx, int band, int nb, int i) {
    if (band == 1 && nb == 2 && __atomic_load_n(&g_stealReq, __ATOMIC_ACQUIRE) == 1) {
        g_stealH = i;
        ctx->own = g_ownNext;
        __atomic_store_n(&g_stealReq, 2, __ATOMIC_SEQ_CST);
    }
}

/* (JOBF_LAZY) monta o triangulo se ninguem montou; espera se a outra thread esta montando. 0 = nada a desenhar */
static int tri_ready(const GxJob *jc) {
    GxJob *j = (GxJob *)jc;
    int st;
    if (!(j->flags & JOBF_LAZY)) {
        return 1;
    }
    st = __atomic_load_n(&j->setup, __ATOMIC_ACQUIRE);
    if (st == 0) {
        int zero = 0;
        if (__atomic_compare_exchange_n(&j->setup, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            const int ok = tri_setup(&j->raw, &j->P, &j->g);
            __atomic_store_n(&j->setup, ok ? 2 : 3, __ATOMIC_RELEASE);
            return ok;
        }
        st = zero;
    }
    while (st == 1) {
        __asm__ __volatile__("" ::: "memory");
        st = __atomic_load_n(&j->setup, __ATOMIC_ACQUIRE);
    }
    return st == 2;
}

/* esta thread tem alguma linha em ys..ye? (sem isso nem monta: o triangulo e so da outra) */
static int rows_any(const uint8_t *own, int ys, int ye, int band, int nb) {
    int y;
    if (nb == 1) {
        return 1;
    }
    if (ye - ys >= 2 * OWN_N) {
        ye = ys + 2 * OWN_N - 1; /* (a tabela da a volta a cada 2*OWN_N linhas) */
    }
    for (y = ys & ~1; y <= ye; y += 2) {
        if (own[(y >> 1) & (OWN_N - 1)] == band) {
            return 1;
        }
    }
    return 0;
}

/* gx_draw_tri: o triangulo vai para a fila sem montar. 0 = monte agora (como antes) */
static int fast_tri_lazy(const TriRaw *R) {
    GxJob *j;
    int prog, fl, ys, ye, k;
    const FastProg *f;
    if (!gx_tri_lazy || gx_threads < 2) {
        return 0;
    }
    if (!tri_rows(R, &ys, &ye)) {
        return 1; /* (nada a desenhar: o mesmo que tri_setup daria) */
    }
    batch_reserve();
    prog = cur_prog();
    f = &g_progs[prog];
    if (f->sampnF[0] != NULL || f->sampnF[1] != NULL) {
        return 0; /* (hd_close precisa do triangulo montado) */
    }
    fl = bg_pre(&g_progs[prog], R->useZ, ys, ye);
    {
        const int sortable = gx_sort_opaque && !(fl & JOBF_DEFER) && R->useZ && f->zimg != NULL && f->zcmp && f->zupd &&
                             !f->zdec && !f->needMem && !f->dead;
        if (!sortable || (gx_sort_chunk > 0 && g_runFrom >= 0 && g_nJobs - g_runFrom >= gx_sort_chunk)) {
            run_close();
        }
        j = batch_push(JOB_TRI);
        j->flags = fl | JOBF_LAZY;
        j->prog = prog;
        j->raw = *R;
        j->lys = ys;
        j->lye = ye;
        j->setup = 0;
        {
            /* (a mesma escala que tri_setup da ao z no vertice) */
            float zb = R->sz[0];
            for (k = 1; k < 3; k++) {
                if (R->sz[k] < zb) {
                    zb = R->sz[k];
                }
            }
            zb *= GX_Z18_SCALE * 256.0f;
            j->zr = zb <= 0.0f ? 0u : zb >= 4.0e9f ? 0xFFFFFFFFu : (uint32_t)zb;
        }
        if (sortable) {
            if (g_runFrom < 0) {
                g_runFrom = g_nJobs - 1;
            }
        } else {
            batch_publish();
        }
    }
    batch_target(gx.cimg.addr, (size_t)(ye + 1) * gx.cimg.width * 2u);
    if (R->useZ && gx.zimg != NULL) {
        batch_target(gx.zimg, (size_t)(ye + 1) * gx.cimg.width * 2u);
    }
    return 1;
}

static void run_jobs(FastCtx *ctx, int band, int nb, int i0, int i1);
/* desenha os trabalhos do fundo guardados so onde a mascara esta livre; depois limpa as linhas desta thread */
/* Lote 195: o fundo adiado era desenhado em cada linha inteira dos triangulos do ceu, com a mascara descartando os
 * pixels cobertos grupo a grupo - com o chao e as casas cobrindo quase tudo, quase todo o trabalho era descartado (o ceu
 * custava ~2 ms em Kakariko com pouco ceu na tela). Agora cada linha da janela vira uma lista de trechos livres (feita
 * uma vez, para todos os trabalhos do fundo): linhas cobertas inteiras sao puladas e cada trabalho so desenha os
 * trechos livres. Mesma imagem (a mascara continua ligada). Opcao gx_bg_trechos (replay --sem-fundo-trechos). */
int gx_bg_trechos = 1;
#define BG_RUNS 24 /* trechos livres guardados por linha; mais que isso: a linha inteira com a mascara (como antes) */
static int16_t g_bgRun[COV_H][BG_RUNS][2];
static int8_t g_bgNRun[COV_H]; /* -1 = muitos trechos (linha inteira pela mascara) */

static void bg_span(const GxPrim *P, const FastProg *f, FastCtx *ctx, int y, int xs, int xe, int useRuns) {
    int r, n;
    if (!useRuns) {
        gx_span_fast(P, f, ctx, y, xs, xe);
        return;
    }
    n = g_bgNRun[y];
    if (n < 0) {
        gx_span_fast(P, f, ctx, y, xs, xe);
        return;
    }
    for (r = 0; r < n; r++) {
        const int a = g_bgRun[y][r][0] > xs ? g_bgRun[y][r][0] : xs;
        const int b = g_bgRun[y][r][1] < xe ? g_bgRun[y][r][1] : xe;
        if (a <= b) {
            gx_span_fast(P, f, ctx, y, a, b);
        }
    }
}

static void run_bg(FastCtx *ctx, int band, int nb, const GxJob *e) {
    int i, y;
    const uint8_t *own = ctx->own ? ctx->own : g_own;
    const int useRuns = gx_bg_trechos && e->y0 <= e->y1;
    ctx->covR = e->y0 <= e->y1; /* janela sem nada marcado: desenha o fundo inteiro, sem a mascara */
    if (useRuns) {
        /* trechos livres das linhas desta thread (fora de y0..y1 nada foi marcado: a linha inteira) */
        int w = COV_W - 16;
        for (i = e->x0; i <= e->x1; i++) {
            if (g_jobs[i].flags & JOBF_DEFER) {
                w = g_progs[g_jobs[i].prog].cwidth;
                break;
            }
        }
        for (y = 0; y < COV_H; y++) {
            if (!row_mine_t(own, y, band, nb)) {
                continue;
            }
            if (y < e->y0 || y > e->y1) {
                g_bgNRun[y] = 1;
                g_bgRun[y][0][0] = 0;
                g_bgRun[y][0][1] = (int16_t)(w - 1);
                continue;
            }
            {
                const uint8_t *c = g_cov[y];
                int x = 0, n = 0;
                while (x < w) {
                    const uint8_t *z = (const uint8_t *)memchr(c + x, 0, (size_t)(w - x));
                    int x1;
                    if (z == NULL) {
                        break;
                    }
                    x = (int)(z - c);
                    x1 = x;
                    while (x1 + 1 < w && c[x1 + 1] == 0) {
                        x1++;
                    }
                    if (n == BG_RUNS) {
                        n = -1;
                        break;
                    }
                    g_bgRun[y][n][0] = (int16_t)x;
                    g_bgRun[y][n][1] = (int16_t)x1;
                    n++;
                    x = x1 + 1;
                }
                g_bgNRun[y] = (int8_t)n;
            }
        }
    }
    for (i = e->x0; i <= e->x1; i++) {
        const GxJob *t = &g_jobs[i];
        const FastProg *f;
        if (!(t->flags & JOBF_DEFER)) {
            continue;
        }
        f = &g_progs[t->prog];
        ctx->hdF = (t->flags & JOBF_HDF) != 0;
        if (t->kind == JOB_TRI) {
            if ((t->flags & JOBF_LAZY) && (!rows_any(own, t->lys, t->lye, band, nb) || !tri_ready(t))) {
                continue;
            }
            for (y = t->g.ys; y <= t->g.ye; y++) {
                int xs, xe;
                if (row_mine_t(own, y, band, nb) && (!useRuns || g_bgNRun[y] != 0) && tri_span(&t->g, y, &xs, &xe)) {
                    bg_span(&t->P, f, ctx, y, xs, xe, useRuns);
                }
            }
        } else {
            for (y = t->y0; y <= t->y1; y++) {
                if (row_mine_t(own, y, band, nb) && (!useRuns || g_bgNRun[y] != 0)) {
                    bg_span(&t->P, f, ctx, y, t->x0, t->x1, useRuns);
                }
            }
        }
    }
    ctx->covR = 0;
    for (y = e->y0; y <= e->y1 && y < COV_H; y++) {
        if (row_mine_t(own, y, band, nb)) {
            memset(g_cov[y], 0, COV_W);
        }
    }
}

static void run_jobs(FastCtx *ctx, int band, int nb, int i0, int i1) {
    int i, y;
    for (i = i0; i < i1; i++) {
        const GxJob *j = &g_jobs[g_ord[i]];
        const uint8_t *own;
        steal_check(ctx, band, nb, i);
        own = ctx->own ? ctx->own : g_own;
        if (j->flags & JOBF_DEFER) {
            continue;
        }
        ctx->covW = (j->flags & JOBF_COVW) != 0;
        ctx->hdF = (j->flags & JOBF_HDF) != 0;
        switch (j->kind) {
            case JOB_TRI: {
                const FastProg *f = &g_progs[j->prog];
                if ((j->flags & JOBF_LAZY) && (!rows_any(own, j->lys, j->lye, band, nb) || !tri_ready(j))) {
                    break;
                }
                for (y = j->g.ys; y <= j->g.ye; y++) {
                    int xs, xe;
                    if (row_mine_t(own, y, band, nb) && tri_span(&j->g, y, &xs, &xe)) {
                        if (gx_pld && f->cimg != NULL) {
                            /* o comeco da proxima linha desta thread: sem isso, cada linha espera a memoria
                             * no inicio (em triangulos pequenos e quase todo o custo da linha) */
                            int yn = y + 1;
                            while (yn <= j->g.ye && !row_mine_t(own, yn, band, nb)) {
                                yn++;
                            }
                            if (yn <= j->g.ye) {
                                const size_t o = ((size_t)yn * (size_t)f->cwidth + (size_t)xs) * 2u;
                                __builtin_prefetch(f->cimg + o);
                                __builtin_prefetch(f->cimg + o + 64);
                                if (f->zimg != NULL) {
                                    __builtin_prefetch(f->zimg + o);
                                    __builtin_prefetch(f->zimg + o + 64);
                                }
                            }
                        }
                        gx_span_fast(&j->P, f, ctx, y, xs, xe);
                    }
                }
                break;
            }
            case JOB_RECT: {
                const FastProg *f = &g_progs[j->prog];
                for (y = j->y0; y <= j->y1; y++) {
                    if (row_mine_t(own, y, band, nb)) {
                        gx_span_fast(&j->P, f, ctx, y, j->x0, j->x1);
                    }
                }
                break;
            }
            case JOB_BGEND:
                run_bg(ctx, band, nb, j);
                break;
            default: {
                /* preenchimento de 16 bits: pixel par = metade alta da cor, impar = baixa (como gx_fill_px) */
                uint8_t c[4];
                int x;
                c[0] = (uint8_t)(j->fill >> 24);
                c[1] = (uint8_t)(j->fill >> 16);
                c[2] = (uint8_t)(j->fill >> 8);
                c[3] = (uint8_t)j->fill;
                for (y = j->y0; y <= j->y1; y++) {
                    uint8_t *p;
                    uint32_t pat;
                    if (!row_mine_t(own, y, band, nb)) {
                        continue;
                    }
                    p = j->cimg + ((size_t)y * (size_t)j->cwidth + (size_t)j->x0) * 2u;
                    x = j->x0;
                    if (x & 1) {
                        p[0] = c[2];
                        p[1] = c[3];
                        p += 2;
                        x++;
                    }
                    memcpy(&pat, c, 4); /* par + impar de uma vez */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
                    {
                        /* 8 pixels por escrita (a limpeza do z-buffer e das telas passa por aqui) */
                        const uint8x16_t v = vreinterpretq_u8_u32(vdupq_n_u32(pat));
                        for (; x + 7 <= j->x1; x += 8, p += 16) {
                            vst1q_u8(p, v);
                        }
                    }
#endif
                    for (; x + 1 <= j->x1; x += 2, p += 4) {
                        memcpy(p, &pat, 4);
                    }
                    if (x <= j->x1) {
                        p[0] = c[0];
                        p[1] = c[1];
                    }
                }
                break;
            }
        }
    }
    ctx->covW = 0;
}

/* thread auxiliar: desenha as linhas da faixa 1 */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cvGo = PTHREAD_COND_INITIALIZER, g_cvDone = PTHREAD_COND_INITIALIZER;
static unsigned g_goSeq, g_doneSeq;
static int g_helperUp;

/* estatistica: tempo desenhando a fila (thread do desenho), na thread auxiliar e esperando por ela */
uint64_t gx_raster_ns, gx_helper_ns, gx_wait_ns;

static uint64_t fast_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Estado do lote entre as threads (tudo com o trinco g_mx ou atomicos seq_cst):
 *   g_active  ha um lote aberto (a auxiliar trabalha nele)
 *   g_pub     trabalhos completos (a thread do desenho publica depois de escrever cada um)
 *   g_closed  o lote acabou: a auxiliar termina o que falta e avisa (g_doneSeq) */
static int g_active, g_closed, g_helperSleeping;
static uint64_t g_helperDoneNs;
static float g_shareAcc;

/* classe da thread do desenho (batch_start copia a de quem desenha; a auxiliar usa a mesma: com
 * prioridade menor que a do desenho ela ficava para tras e o quadro atrasava) */
static int g_drawPol = SCHED_OTHER, g_drawPrio = 0;

static void helper_follow_class(void) {
#ifdef __linux__
    static int pol = -1, prio = -1;
    const int np = __atomic_load_n(&g_drawPol, __ATOMIC_RELAXED), npr = __atomic_load_n(&g_drawPrio, __ATOMIC_RELAXED);
    if (np != pol || npr != prio) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = np == SCHED_OTHER ? 0 : npr;
        pthread_setschedparam(pthread_self(), np, &sp);
        if (np == SCHED_OTHER) {
            setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 0);
        }
        pol = np;
        prio = npr;
    }
#endif
}

static void *helper_main(void *arg) {
    unsigned seen = 0;
    (void)arg;
#ifdef __linux__
    {
        if (gx_helper_cpu >= 0) {
            unsigned long cs = 1ul << gx_helper_cpu; /* (sem _GNU_SOURCE: chamada direta) */
            syscall(SYS_sched_setaffinity, 0, sizeof cs, &cs);
        }
    }
#endif
    PT_NAME("auxiliar");
    for (;;) {
        int i = 0;
        uint64_t busy = 0;
        pthread_mutex_lock(&g_mx);
        PT_EV(PT_HELP_SLEEP, 0);
        while (g_goSeq == seen) {
            pthread_cond_wait(&g_cvGo, &g_mx);
        }
        PT_EV(PT_HELP_SLEEP + 1, 0);
        seen = g_goSeq;
        pthread_mutex_unlock(&g_mx);
        helper_follow_class();
        for (;;) {
            int pub = __atomic_load_n(&g_pub, __ATOMIC_SEQ_CST);
            if (i < pub) {
                uint64_t t0 = fast_now_ns();
                PT_EV(PT_HELP_RUN, pub - i);
                run_jobs(&g_ctx[1], 1, 2, i, pub);
                PT_EV(PT_HELP_RUN + 1, 0);
                busy += fast_now_ns() - t0;
                i = pub;
                continue;
            }
            if (__atomic_load_n(&g_closed, __ATOMIC_SEQ_CST)) {
                if (i < __atomic_load_n(&g_pub, __ATOMIC_SEQ_CST)) {
                    continue;
                }
                break;
            }
            /* nada novo: espera acordada um pouco (se pedido), depois dorme ate a thread do desenho
             * publicar mais ou fechar o lote */
            if (gx_helper_spin_us > 0) {
                uint64_t lim = fast_now_ns() + (uint64_t)gx_helper_spin_us * 1000u;
                while (__atomic_load_n(&g_pub, __ATOMIC_SEQ_CST) == i && !__atomic_load_n(&g_closed, __ATOMIC_SEQ_CST) &&
                       fast_now_ns() < lim) {
                    int k;
                    for (k = 0; k < 64; k++) {
                        __asm__ __volatile__("" ::: "memory");
                    }
                }
                if (__atomic_load_n(&g_pub, __ATOMIC_SEQ_CST) != i || __atomic_load_n(&g_closed, __ATOMIC_SEQ_CST)) {
                    continue;
                }
            }
            pthread_mutex_lock(&g_mx);
            PT_EV(PT_HELP_SLEEP, 1);
            __atomic_store_n(&g_helperSleeping, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&g_pub, __ATOMIC_SEQ_CST) == i && !__atomic_load_n(&g_closed, __ATOMIC_SEQ_CST)) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 2000000; /* rede de seguranca contra aviso perdido */
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                pthread_cond_timedwait(&g_cvGo, &g_mx, &ts);
            }
            __atomic_store_n(&g_helperSleeping, 0, __ATOMIC_SEQ_CST);
            PT_EV(PT_HELP_SLEEP + 1, 1);
            pthread_mutex_unlock(&g_mx);
        }
        gx_helper_ns += busy;
        pthread_mutex_lock(&g_mx);
        g_helperDoneNs = fast_now_ns();
        g_doneSeq = seen;
        pthread_cond_signal(&g_cvDone);
        pthread_mutex_unlock(&g_mx);
    }
    return NULL;
}

static void wake_helper(void) {
    if (__atomic_load_n(&g_helperSleeping, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&g_mx);
        pthread_cond_broadcast(&g_cvGo);
        pthread_mutex_unlock(&g_mx);
    }
}

/* primeiro trabalho de um lote: acorda a auxiliar para ela ir desenhando junto */
static void batch_start(void) {
#ifdef __linux__
    if (gx_threads >= 2) {
        /* a auxiliar segue a classe de quem desenha (tempo real ou normal) */
        static int n;
        if ((n++ & 63) == 0) {
            struct sched_param sp;
            int pol;
            if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0) {
                __atomic_store_n(&g_drawPol, pol, __ATOMIC_RELAXED);
                __atomic_store_n(&g_drawPrio, sp.sched_priority, __ATOMIC_RELAXED);
            }
        }
    }
#endif
    if (g_helperShare < 0) {
        set_share(OWN_N / 2);
    }
    if (gx_threads >= 2 && !g_helperUp) {
        pthread_t th;
        if (pthread_create(&th, NULL, helper_main, NULL) == 0) {
            pthread_detach(th);
            g_helperUp = 1;
        } else {
            gx_threads = 1;
        }
    }
    if (gx_threads >= 2) {
        __atomic_store_n(&g_pub, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_closed, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_mx);
        g_active = 1;
        g_goSeq++;
        pthread_cond_broadcast(&g_cvGo);
        pthread_mutex_unlock(&g_mx);
    }
}

static void batch_publish(void) {
    if (g_active) {
        __atomic_store_n(&g_pub, g_nJobs, __ATOMIC_SEQ_CST);
        if ((g_nJobs & 7) == 0) {
            wake_helper();
        }
    }
}

void gx_batch_flush(void) {
    if (g_nJobs == 0) {
        return;
    }
    g_flushGen++;
    gx_stN[3]++;
    run_close();
    bg_end(); /* o fundo guardado entra antes de a fila acabar */
    if (!g_active && gx_threads >= 2) {
        batch_start(); /* sem desenho simultaneo: a auxiliar so comeca agora */
    }
    if (g_active) {
        uint64_t t0, t1;
        const uint64_t h0 = __atomic_load_n(&gx_helper_ns, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_pub, g_nJobs, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_closed, 1, __ATOMIC_SEQ_CST);
        wake_helper();
        t0 = fast_now_ns();
        PT_EV(PT_DRAW_RASTER, g_nJobs);
        run_jobs(&g_ctx[0], 0, 2, 0, g_nJobs);
        PT_EV(PT_DRAW_RASTER + 1, 0);
        t1 = fast_now_ns();
        gx_raster_ns += t1 - t0;
        if (gx_steal) {
            const uint8_t *cur = g_own; /* a tabela que a auxiliar usa agora */
            int r;
            for (r = 0; r < 6 && __atomic_load_n(&g_doneSeq, __ATOMIC_SEQ_CST) != g_goSeq; r++) {
                /* rodada r: metade das partes que ainda sao da auxiliar (uma sim, uma nao) */
                uint8_t *nt = g_ownS[r & 1];
                int k, alt = 0, left = 0;
                for (k = 0; k < OWN_N; k++) {
                    nt[k] = cur[k];
                    if (cur[k] == 1) {
                        left++;
                        if (alt) {
                            nt[k] = (uint8_t)(2 + r);
                        }
                        alt ^= 1;
                    }
                }
                if (left < 2) {
                    break;
                }
                g_ownNext = nt;
                __atomic_store_n(&g_stealReq, 1, __ATOMIC_SEQ_CST);
                /* espera a resposta (entre dois trabalhos da auxiliar) ou o fim dela */
                while (__atomic_load_n(&g_stealReq, __ATOMIC_SEQ_CST) == 1 &&
                       __atomic_load_n(&g_doneSeq, __ATOMIC_SEQ_CST) != g_goSeq) {
                    sched_yield();
                }
                if (__atomic_load_n(&g_stealReq, __ATOMIC_SEQ_CST) != 2) {
                    break; /* a auxiliar acabou antes de responder */
                }
                __atomic_store_n(&g_stealReq, 0, __ATOMIC_SEQ_CST);
                {
                    const uint64_t s0 = fast_now_ns();
                    g_ctx[0].own = nt;
                    PT_EV(PT_DRAW_STEAL, r);
                    run_jobs(&g_ctx[0], 2 + r, 2, g_stealH, g_nJobs);
                    PT_EV(PT_DRAW_STEAL + 1, 0);
                    g_ctx[0].own = NULL;
                    gx_steal_ns += fast_now_ns() - s0;
                }
                cur = nt;
            }
            __atomic_store_n(&g_stealReq, 0, __ATOMIC_SEQ_CST);
        }
        PT_EV(PT_DRAW_WAIT, 0);
        pthread_mutex_lock(&g_mx);
        while (g_doneSeq != g_goSeq) {
            pthread_cond_wait(&g_cvDone, &g_mx);
        }
        g_active = 0;
        __atomic_store_n(&g_stealReq, 0, __ATOMIC_SEQ_CST);
        g_ctx[1].own = NULL;
        pthread_mutex_unlock(&g_mx);
        PT_EV(PT_DRAW_WAIT + 1, 0);
        gx_wait_ns += fast_now_ns() - t1;
        /* ajusta a divisao: se esperamos pela auxiliar, ela tinha linhas demais; se ela acabou bem antes
         * de nos, tinha de menos. So lotes grandes contam (os pequenos sao ruido). */
        if (g_nJobs >= 64) {
            /* d > 0: a auxiliar terminou depois. Passar 1 parte de uma para a outra muda a diferenca em
             * 2 * (trabalho total / OWN_N); corrige metade do que falta por lote, guardando a fracao */
            const int64_t d = (int64_t)g_helperDoneNs - (int64_t)t1;
            const double w = (double)(t1 - t0) + (double)(gx_helper_ns - h0);
            if (w > 100000.0) {
                int m;
                g_shareAcc -= (float)(0.25 * OWN_N * (double)d / w);
                m = (int)g_shareAcc;
                if (m > 4) {
                    m = 4;
                } else if (m < -4) {
                    m = -4;
                }
                if (m != 0) {
                    int k = g_helperShare + m;
                    k = k < OWN_N / 4 ? OWN_N / 4 : k > OWN_N - OWN_N / 8 ? OWN_N - OWN_N / 8 : k;
                    set_share(k);
                    g_shareAcc -= (float)m;
                }
                if (g_shareAcc > 4.0f || g_shareAcc < -4.0f) {
                    g_shareAcc = 0.0f;
                }
            }
        }
    } else {
        uint64_t t0 = fast_now_ns();
        PT_EV(PT_DRAW_RASTER, g_nJobs);
        run_jobs(&g_ctx[0], 0, 1, 0, g_nJobs);
        PT_EV(PT_DRAW_RASTER + 1, 0);
        gx_raster_ns += fast_now_ns() - t0;
    }
    g_nJobs = 0;
    g_nProgs = 0;
    g_nTgt = 0;
}
