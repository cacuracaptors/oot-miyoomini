/*
 * video_miyoo.c -- a tela do Miyoo Mini Plus para o Ocarina of Time.
 *
 * O jogo desenha (pelo renderizador do port) no framebuffer do N64, na RAM: 320x240,
 * RGBA5551 com os bytes na ordem do N64. Quando o VI troca de quadro (gancho
 * vi_present), o endereco do quadro vai para a thread de tela, que:
 *   1. converte para RGB565 (NEON) numa memoria que o 2D do chip enxerga (MMA);
 *   2. manda o 2D do chip (MI_GFX) escalar para 640x480, girar 180 graus (o painel e
 *      montado de cabeca para baixo) e converter para XRGB8888 direto na pagina
 *      escondida de /dev/fb0;
 *   3. mostra essa pagina no proximo vsync (FBIOPAN_DISPLAY espera por ele).
 * Sem MI_GFX (ou com o teste dele falhando), a CPU faz os passos 1 e 2.
 *
 * A converter logo que o VI troca e seguro: o OoT usa dois framebuffers e so volta a
 * desenhar neste depois da proxima troca.
 *
 * Relogio: como no Mario Kart 64, cada volta da thread termina num vsync de verdade do
 * painel, e ela avisa o VI do jogo (os_hw_vi_pulse). Assim o jogo anda no ritmo exato
 * do painel (~59,7 Hz no Miyoo) e cada quadro fica o mesmo tempo na tela. Se a troca de
 * pagina nao esperar o vsync, volta para o relogio interno de 60 Hz.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if __has_include(<mi_gfx.h>)
#include <mi_gfx.h>
#include <mi_sys.h>
#define HAVE_MI_GFX 1
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VID_NEON 1
#endif

#include "miyoo.h"
#include "../perf_trace.h"
#include "os_hw.h"
#include "font8x16.h"

/* Tamanho da tela: 640x480 no Mini Plus (e no Mini antigo); 752x560 no Mini v4 e no Mini Flip. Antes o jogo punha a
 * tela destes em 640x480 e o aparelho ampliava a imagem para 752x560 (1,175 vez): as linhas do filtro retro e os
 * pixels ficavam desiguais (relato de 11/2026). Agora a tela fica no tamanho dela e o jogo desenha para ele
 * (gfx_hires.c, gx_hr_set_panel). Decidido em panel_detect, antes de mexer no modo da tela. */
#define PANEL_MAX_W 752
#define PANEL_MAX_H 560
int miyoo_panel_w = 640, miyoo_panel_h = 480;
#define PANEL_W miyoo_panel_w
#define PANEL_H miyoo_panel_h
#define SRC_MAX_W PANEL_W
#define SRC_MAX_H PANEL_H
/* o menu de opcoes e montado sempre em 640x480 (menu_miyoo.c); na tela de 752x560 ele e ampliado */
#define MENU_W 640
#define MENU_H 480

int miyoo_opt_perf;
int miyoo_opt_nogfx;

double miyoo_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- /dev/fb0 */

static int fb_fd = -1;
static struct fb_var_screeninfo fb_var;
static uint8_t* fb_mem;
static size_t fb_len;
static unsigned long fb_phys;
static int fb_pages; /* 2 = troca de pagina com vsync; 1 = desenha na pagina visivel */
static int fb_page;

/* A tela pode chegar ao jogo em outro modo, deixado pelo programa anterior (o menu do RedOnion fecha o SDL
 * antes de cada programa e o SDL devolve o modo que achou; o Mini v4/Flip usa 752x560). Ja aconteceu de
 * chegar com 2 paginas e mesmo assim a troca de pagina nao esperar o vsync (20 quadros por segundo, ate
 * reiniciar o aparelho). Aqui o modo e sempre posto de novo em 640x480, 32 bits, 2 paginas, como o OnionOS
 * faz (fbset -g 640 480 640 960 32), e o batmon e o keymon sao avisados. */
static int fb_set_mode(const char *why) {
    struct fb_var_screeninfo v, n;
    int r;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) != 0) {
        return -1;
    }
    n = v;
    n.xres = n.xres_virtual = PANEL_W;
    n.yres = PANEL_H;
    n.yres_virtual = PANEL_H * 2;
    n.xoffset = n.yoffset = 0;
    n.bits_per_pixel = 32;
    n.red.offset = 16;
    n.red.length = 8;
    n.green.offset = 8;
    n.green.length = 8;
    n.blue.offset = 0;
    n.blue.length = 8;
    n.transp.offset = 24;
    n.transp.length = 8;
    n.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (v.xres != PANEL_W || v.yres != PANEL_H || v.bits_per_pixel != 32 || !strcmp(why, "abertura")) {
        /* a memoria da tela ainda tem a imagem do programa anterior em outro passo de linha (o Game Switcher do
         * Mini v4/Flip: 752 pixels por linha), que o modo novo mostraria torta ate o 1o quadro: preto antes,
         * como o fbmode --preclear do RedOnion (port do Mario Kart 64). Na abertura, sempre: a pagina 0 pode ter
         * ainda o ultimo quadro da vez anterior do jogo (o menu do OnionOS desenha na outra pagina), que aparecia
         * por 1 ou 2 segundos ate o 1o quadro novo. */
        struct fb_fix_screeninfo of;
        if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &of) == 0 && of.smem_len) {
            void* m = mmap(NULL, of.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
            if (m != MAP_FAILED) {
                memset(m, 0, of.smem_len);
                munmap(m, of.smem_len);
            }
        }
    }
    r = ioctl(fb_fd, FBIOPUT_VSCREENINFO, &n);
    fprintf(stderr, "video: %s: tela estava %ux%u %ubpp (virtual %ux%u, pagina em y=%u); posta em %dx%d 32bpp 2 paginas: %s\n",
            why, v.xres, v.yres, v.bits_per_pixel, v.xres_virtual, v.yres_virtual, v.yoffset, PANEL_W, PANEL_H,
            r == 0 ? "ok" : "falhou");
    if (r == 0) {
        if (system("killall -SIGUSR1 batmon >/dev/null 2>&1; killall -SIGUSR1 keymon >/dev/null 2>&1") != 0) {
            /* (sem eles rodando: nada a avisar) */
        }
    }
    return r;
}

/* tela de 560 linhas (Mini v4, Mini Flip)? O menu do Onion/RedOnion deixa a tela no tamanho dela quando abre o jogo;
 * numa reabertura pelo proprio jogo (reiniciar, Game Switcher) ela ja esta como o jogo deixou (o tamanho dela tambem).
 * O OnionOS grava o tamanho em /tmp/screen_resolution em algumas versoes. OOT_TELA=640 ou 752 (launch.sh) manda. */
static void panel_detect(void) {
    const char* e = getenv("OOT_TELA");
    struct fb_var_screeninfo v;
    int big = 0;
    const char* why = "tela em 640x480";
    if (e != NULL && (atoi(e) == 640 || atoi(e) == 752)) {
        big = atoi(e) == 752;
        why = "OOT_TELA";
    } else if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) == 0 && v.yres == 560 && (v.xres == 752 || v.xres == 750)) {
        big = 1;
        why = "modo em que a tela chegou";
    } else {
        FILE* f = fopen("/tmp/screen_resolution", "r");
        if (f != NULL) {
            int w = 0, h = 0;
            if (fscanf(f, "%dx%d", &w, &h) == 2 && h == 560) {
                big = 1;
                why = "/tmp/screen_resolution";
            }
            fclose(f);
        }
    }
    miyoo_panel_w = big ? 752 : 640;
    miyoo_panel_h = big ? 560 : 480;
    fprintf(stderr, "video: tela %dx%d (%s)\n", PANEL_W, PANEL_H, why);
}

static void fb_init(void) {
    struct fb_fix_screeninfo fix;
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) {
        fprintf(stderr, "video: sem /dev/fb0 (nada sera mostrado)\n");
        return;
    }
    panel_detect();
    fb_set_mode("abertura");
    if (PANEL_W != 640 && (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) ||
                           fb_var.xres != (unsigned)PANEL_W || fb_var.yres != (unsigned)PANEL_H ||
                           fb_var.bits_per_pixel != 32 || fix.line_length != (unsigned)PANEL_W * 4)) {
        /* a tela nao aceitou o tamanho dela: volta para 640x480 (o aparelho amplia, como antes) */
        fprintf(stderr, "video: a tela nao ficou em %dx%d (%ux%u, linha de %u bytes); voltando para 640x480\n", PANEL_W,
                PANEL_H, fb_var.xres, fb_var.yres, fix.line_length);
        miyoo_panel_w = 640;
        miyoo_panel_h = 480;
        fb_set_mode("tamanho da tela recusado");
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) ||
        fb_var.xres != (unsigned)PANEL_W || fb_var.yres != (unsigned)PANEL_H || fb_var.bits_per_pixel != 32 ||
        fix.line_length != (unsigned)PANEL_W * 4) {
        fprintf(stderr, "video: framebuffer inesperado %ux%u %ubpp (nada sera mostrado)\n", fb_var.xres, fb_var.yres,
                fb_var.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        return;
    }
    fb_len = fix.smem_len;
    fb_phys = fix.smem_start;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "video: mmap de /dev/fb0 falhou\n");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return;
    }
    if (fb_var.yres_virtual >= PANEL_H * 2 && fb_len >= (size_t)PANEL_W * PANEL_H * 4 * 2) {
        fb_pages = 2;
        fb_page = fb_var.yoffset >= (unsigned)PANEL_H ? 1 : 0;
        fprintf(stderr, "video: tela %dx%d, 2 paginas (vsync)\n", PANEL_W, PANEL_H);
    } else {
        fb_pages = 1;
        fb_page = 0;
        fprintf(stderr, "video: tela %dx%d, 1 pagina (sem vsync)\n", PANEL_W, PANEL_H);
    }
}

static uint32_t* fb_page_ptr(int page) {
    return (uint32_t*)(fb_mem + (size_t)page * PANEL_W * PANEL_H * 4);
}

/* ---------------------------------------------------------------- MI_GFX */

static int hw_ok;
#ifdef HAVE_MI_GFX
static struct {
    MI_S32 (*SysInit)(void);
    MI_S32 (*MmaAlloc)(MI_U8*, MI_U32, MI_PHY*);
    MI_S32 (*MmaFree)(MI_PHY);
    MI_S32 (*Mmap)(MI_U64, MI_U32, void**, MI_BOOL);
    MI_S32 (*Munmap)(void*, MI_U32);
    MI_S32 (*Flush)(void*, MI_U32);
    MI_S32 (*GfxOpen)(void);
    MI_S32 (*GfxClose)(void);
    MI_S32 (*Blit)(MI_GFX_Surface_t*, MI_GFX_Rect_t*, MI_GFX_Surface_t*, MI_GFX_Rect_t*, MI_GFX_Opt_t*, MI_U16*);
    MI_S32 (*Wait)(MI_BOOL, MI_U16);
} mi;
static MI_PHY hw_phys[2];
static pthread_mutex_t g_blitMx = PTHREAD_MUTEX_INITIALIZER; /* (o chip recebe pedidos da tela e do desenho) */
static int hw_rotate = E_MI_GFX_ROTATE_180, hw_mirror = E_MI_GFX_MIRROR_NONE;
#endif
static uint16_t* conv_buf[2]; /* quadros convertidos (RGB565), memoria MMA quando hw_ok */

#ifdef HAVE_MI_GFX
static void* open_lib(const char* name) {
    static const char* dirs[] = { "", "/customer/lib/", "/config/lib/", "/lib/", "/usr/lib/" };
    char path[96];
    unsigned i;
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        void* h;
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            return h;
        }
    }
    return NULL;
}

/* dstride: bytes por linha no destino (o filtro retro escreve uma linha sim, outra nao: 2 linhas da tela) */
static int hw_blit_ex(MI_PHY src, int sfmt, int sw, int sh, MI_PHY dst, int dw, int dh, int dstride, int rotate, int mirror) {
    MI_GFX_Surface_t s = { src, sfmt, (MI_U32)sw, (MI_U32)sh, (MI_U32)sw * 2 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32)dw, (MI_U32)dh, (MI_U32)dstride };
    MI_GFX_Rect_t sr = { 0, 0, (MI_U32)sw, (MI_U32)sh }, dr = { 0, 0, (MI_U32)dw, (MI_U32)dh };
    MI_GFX_Opt_t opt;
    MI_U16 fence = 0;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect = dr;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.eMirror = (MI_GFX_Mirror_e)mirror;
    opt.eRotate = (MI_GFX_Rotate_e)rotate;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
    pthread_mutex_lock(&g_blitMx);
    if (mi.Blit(&s, &sr, &d, &dr, &opt, &fence) != 0) {
        pthread_mutex_unlock(&g_blitMx);
        return -1;
    }
    pthread_mutex_unlock(&g_blitMx);
    mi.Wait(TRUE, fence);
    return 0;
}

static int hw_blit_fmt(MI_PHY src, int sfmt, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror) {
    extern int gx_retro; /* gfx_hires.c: filtro retro */
    if (gx_retro && sw * 2 == dw && sh * 2 == dh && dw == PANEL_W && dh == PANEL_H) {
        /* filtro retro: cada linha do jogo vai inteira para 2 linhas da tela (sem a suavizacao de cima para baixo, que
         * misturaria as linhas vizinhas): 2 copias de 640x240, uma nas linhas pares e outra nas impares (752x280 com o
         * jogo em 376x280 na tela de 752x560) */
        if (hw_blit_ex(src, sfmt, sw, sh, dst, dw, dh / 2, dw * 8, rotate, mirror) != 0) {
            return -1;
        }
        return hw_blit_ex(src, sfmt, sw, sh, dst + (MI_PHY)dw * 4, dw, dh / 2, dw * 8, rotate, mirror);
    }
    return hw_blit_ex(src, sfmt, sw, sh, dst, dw, dh, dw * 4, rotate, mirror);
}

static int hw_blit(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh, int rotate, int mirror) {
    return hw_blit_fmt(src, E_MI_GFX_FMT_RGB565, sw, sh, dst, dw, dh, rotate, mirror);
}

/* imagem XRGB8888 ja na orientacao do painel (o menu de opcoes) ampliada para a pagina, sem giro */
static int hw_blit32(MI_PHY src, int sw, int sh, MI_PHY dst, int dw, int dh) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_ARGB8888, (MI_U32)sw, (MI_U32)sh, (MI_U32)sw * 4 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32)dw, (MI_U32)dh, (MI_U32)dw * 4 };
    MI_GFX_Rect_t sr = { 0, 0, (MI_U32)sw, (MI_U32)sh }, dr = { 0, 0, (MI_U32)dw, (MI_U32)dh };
    MI_GFX_Opt_t opt;
    MI_U16 fence = 0;
    int r;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect = dr;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.eMirror = E_MI_GFX_MIRROR_NONE;
    opt.eRotate = E_MI_GFX_ROTATE_0;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
    pthread_mutex_lock(&g_blitMx);
    r = mi.Blit(&s, &sr, &d, &dr, &opt, &fence);
    pthread_mutex_unlock(&g_blitMx);
    if (r != 0) {
        return -1;
    }
    mi.Wait(TRUE, fence);
    return 0;
}

/* imagens da fila (gfx_hook.c) na memoria MMA: o chip 2D le direto (opcao cor_direta, cor RGBA5551 na ordem do
 * processador) - sem converter na CPU. Tabela virtual -> fisica */
#define IMG_MMA_N 8
static struct {
    void *v;
    MI_PHY p;
    size_t n;
} g_imgMma[IMG_MMA_N];
static int g_nImgMma;
static unsigned g_direct, g_directNo;

static void *img_mma_alloc(size_t bytes) {
    MI_PHY p = 0;
    void *v = NULL;
    if (g_nImgMma >= IMG_MMA_N || mi.MmaAlloc(NULL, (MI_U32)bytes, &p) != 0) {
        return NULL;
    }
    if (mi.Mmap(p, (MI_U32)bytes, &v, TRUE) != 0) {
        mi.MmaFree(p);
        return NULL;
    }
    memset(v, 0, bytes);
    g_imgMma[g_nImgMma].v = v;
    g_imgMma[g_nImgMma].p = p;
    g_imgMma[g_nImgMma].n = bytes;
    g_nImgMma++;
    return v;
}

static MI_PHY img_mma_phys(const void *v) {
    int i;
    for (i = 0; i < g_nImgMma; i++) {
        if (g_imgMma[i].v == v) {
            return g_imgMma[i].p;
        }
    }
    return 0;
}

/* (endereco dentro de uma das imagens MMA da fila: o fisico correspondente; 0 = fora delas) */
static MI_PHY img_mma_phys_in(const void *v, size_t len) {
    int i;
    for (i = 0; i < g_nImgMma; i++) {
        const uint8_t *b = (const uint8_t *)g_imgMma[i].v;
        if ((const uint8_t *)v >= b && (const uint8_t *)v + len <= b + g_imgMma[i].n) {
            return g_imgMma[i].p + (MI_PHY)((const uint8_t *)v - b);
        }
    }
    return 0;
}

/* fundos JPEG (praca do mercado, frente do Templo do Tempo...) pelo chip 2D (opcao bg_accelerator, gfx_rdp.c
 * gx_bg_rect): a imagem do jogo (RGBA5551 na ordem do N64) vai trocada de ordem para uma memoria MMA e o chip a
 * amplia (com suavizacao) direto na imagem que os nucleos estao desenhando (da fila, MMA), antes do 3D por cima.
 * Os nucleos estao parados (gx_batch_flush) enquanto isso. 1 = feito; 0 = a CPU faz como antes */
static MI_PHY g_bgPhys;
static uint16_t *g_bgVirt;
static size_t g_bgCap;
static unsigned g_bgN, g_bgNo;

static int ge_bg_blit(uint8_t *dst, int dstW, int dstH, const uint8_t *img, int iw, int ih, int sx, int sy, int sw,
                      int sh, int dx, int dy, int dw, int dh) {
    const size_t need = (size_t)iw * (size_t)ih * 2u;
    MI_PHY dp;
    MI_GFX_Surface_t s, d;
    MI_GFX_Rect_t sr, dr;
    MI_GFX_Opt_t opt;
    MI_U16 fence = 0;
    int x, y, r;
    if (!hw_ok || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || sx < 0 || sy < 0 || sx + sw > iw || sy + sh > ih ||
        dx < 0 || dy < 0 || dx + dw > dstW || dy + dh > dstH) {
        g_bgNo++;
        return 0;
    }
    dp = img_mma_phys_in(dst, (size_t)dstW * (size_t)dstH * 2u);
    if (dp == 0) {
        g_bgNo++;
        return 0;
    }
    if (g_bgCap < need) {
        void *v = NULL;
        MI_PHY p = 0;
        if (g_bgVirt != NULL) {
            mi.Munmap(g_bgVirt, (MI_U32)g_bgCap);
            mi.MmaFree(g_bgPhys);
            g_bgVirt = NULL;
            g_bgCap = 0;
        }
        if (mi.MmaAlloc(NULL, (MI_U32)need, &p) != 0) {
            g_bgNo++;
            return 0;
        }
        if (mi.Mmap(p, (MI_U32)need, &v, TRUE) != 0) {
            mi.MmaFree(p);
            g_bgNo++;
            return 0;
        }
        g_bgPhys = p;
        g_bgVirt = (uint16_t *)v;
        g_bgCap = need;
    }
    for (y = sy; y < sy + sh; y++) {
        const uint8_t *sp = img + ((size_t)y * iw + sx) * 2u;
        uint16_t *o = g_bgVirt + (size_t)y * iw + sx;
        for (x = 0; x < sw; x++) {
            o[x] = (uint16_t)(((unsigned)sp[2 * x] << 8) | sp[2 * x + 1] | 1u); /* (alfa 1, como o combinador do jogo) */
        }
    }
    mi.Flush(g_bgVirt + (size_t)sy * iw, (MI_U32)((size_t)sh * iw * 2u));
    mi.Flush(dst + (size_t)dy * dstW * 2u, (MI_U32)((size_t)dh * dstW * 2u)); /* (nada da CPU na cache por cima) */
    memset(&opt, 0, sizeof opt);
    s.phyAddr = g_bgPhys;
    s.eColorFmt = E_MI_GFX_FMT_RGBA5551;
    s.u32Width = (MI_U32)iw;
    s.u32Height = (MI_U32)ih;
    s.u32Stride = (MI_U32)iw * 2u;
    d.phyAddr = dp;
    d.eColorFmt = E_MI_GFX_FMT_RGBA5551;
    d.u32Width = (MI_U32)dstW;
    d.u32Height = (MI_U32)dstH;
    d.u32Stride = (MI_U32)dstW * 2u;
    sr.s32Xpos = sx;
    sr.s32Ypos = sy;
    sr.u32Width = (MI_U32)sw;
    sr.u32Height = (MI_U32)sh;
    dr.s32Xpos = dx;
    dr.s32Ypos = dy;
    dr.u32Width = (MI_U32)dw;
    dr.u32Height = (MI_U32)dh;
    opt.stClipRect = dr;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.eMirror = E_MI_GFX_MIRROR_NONE;
    opt.eRotate = E_MI_GFX_ROTATE_0;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
    pt_ev(PT_BG_GE, 0);
    pthread_mutex_lock(&g_blitMx);
    r = mi.Blit(&s, &sr, &d, &dr, &opt, &fence);
    pthread_mutex_unlock(&g_blitMx);
    if (r != 0) {
        pt_ev(PT_BG_GE + 1, 0);
        g_bgNo++;
        return 0;
    }
    mi.Wait(TRUE, fence);
    mi.Flush(dst + (size_t)dy * dstW * 2u, (MI_U32)((size_t)dh * dstW * 2u));
    pt_ev(PT_BG_GE + 1, 0);
    g_bgN++;
    return 1;
}

static inline uint32_t rgb565_to_x8888(uint32_t c) {
    return ((c & 0xF800u) << 8) | ((c & 0xE000u) << 3) | ((c & 0x07E0u) << 5) | ((c & 0x0600u) >> 1) |
           ((c & 0x001Fu) << 3) | ((c & 0x001Cu) >> 2);
}

/* teste (so registro no log): o chip 2D le imagens de 16 bits com 5 bits por cor + 1 de transparencia? Em que ordem?
 * Para decidir se o desenho pode gravar a imagem maior ja num formato que ele le direto (sem a conversao na CPU). */
static void fmt_probe(void) {
    static const struct {
        int f;
        const char *n;
    } fm[] = { { E_MI_GFX_FMT_RGBA5551, "RGBA5551" }, { E_MI_GFX_FMT_ARGB1555, "ARGB1555" }, { E_MI_GFX_FMT_BGRA5551, "BGRA5551" } };
    /* pixels de teste em u16 (ordem do processador): vermelho, verde, azul e branco no layout do N64 (R 15-11,
     * G 10-6, B 5-1, A 0), e os mesmos com os bytes trocados (como ficam na memoria hoje) */
    static const uint16_t px[8] = { 0xF801, 0x07C1, 0x003F, 0xFFFE, 0x01F8, 0xC107, 0x3F00, 0xFEFF };
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    unsigned k, i;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd) || mi.Mmap(ps, 4096, &vs, TRUE) ||
        mi.Mmap(pd, 4096, &vd, TRUE)) {
        fprintf(stderr, "[formato] sem memoria para o teste\n");
        goto out;
    }
    for (k = 0; k < sizeof fm / sizeof fm[0]; k++) {
        MI_GFX_Surface_t sf = { ps, fm[k].f, 8, 1, 16 };
        MI_GFX_Surface_t df = { pd, E_MI_GFX_FMT_ARGB8888, 8, 1, 32 };
        MI_GFX_Rect_t r = { 0, 0, 8, 1 };
        MI_GFX_Opt_t opt;
        MI_U16 fence = 0;
        uint32_t *d = (uint32_t *)vd;
        memcpy(vs, px, sizeof px);
        memset(vd, 0, 64);
        mi.Flush(vs, 4096);
        mi.Flush(vd, 4096);
        memset(&opt, 0, sizeof opt);
        opt.stClipRect = r;
        opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
        opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
        opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;
        if (mi.Blit(&sf, &r, &df, &r, &opt, &fence) != 0) {
            fprintf(stderr, "[formato] %s: o chip recusou\n", fm[k].n);
            continue;
        }
        mi.Wait(TRUE, fence);
        mi.Flush(vd, 4096);
        fprintf(stderr, "[formato] %s:", fm[k].n);
        for (i = 0; i < 8; i++) {
            fprintf(stderr, " %04x->%08x", px[i], d[i]);
        }
        fprintf(stderr, "\n");
    }
out:
    if (vs) {
        mi.Munmap(vs, 4096);
    }
    if (vd) {
        mi.Munmap(vd, 4096);
    }
    if (ps) {
        mi.MmaFree(ps);
    }
    if (pd) {
        mi.MmaFree(pd);
    }
}

/* blit de teste: orientacao e cores certas? */
static int hw_selftest(void) {
    /* 1:1 (so orientacao e cores): ao escalar o 2D do chip suaviza (mistura vizinhos), o que e esperado */
    const int sw = 16, sh = 8, dw = 16, dh = 8;
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    int ok = 0, t, i;
    static const int tries[2][2] = { { E_MI_GFX_ROTATE_180, E_MI_GFX_MIRROR_NONE },
                                     { E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_BOTH } };
    uint16_t* src;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd)) {
        fprintf(stderr, "video: teste do MI_GFX: MMA_Alloc falhou\n");
        goto out;
    }
    if (mi.Mmap(ps, 4096, &vs, TRUE) || mi.Mmap(pd, 4096, &vd, TRUE)) {
        fprintf(stderr, "video: teste do MI_GFX: Mmap falhou\n");
        goto out;
    }
    src = vs;
    for (i = 0; i < sw * sh; i++) {
        src[i] = (uint16_t)(((i * 7) & 31) << 11 | ((i * 13) & 63) << 5 | ((i * 3 + 5) & 31));
    }
    for (t = 0; t < 2 && !ok; t++) {
        const uint32_t* dst = vd;
        int bad = 0, x, y, k;
        memset(vd, 0, 4096);
        mi.Flush(vs, 4096);
        mi.Flush(vd, 4096);
        if (hw_blit(ps, sw, sh, pd, dw, dh, tries[t][0], tries[t][1])) {
            fprintf(stderr, "video: teste do MI_GFX %d: o blit falhou\n", t);
            continue;
        }
        mi.Flush(vd, 4096);
        for (y = 0; y < dh && !bad; y++) {
            for (x = 0; x < dw && !bad; x++) {
                int sx = sw - 1 - x * sw / dw, sy = sh - 1 - y * sh / dh;
                uint32_t want = rgb565_to_x8888(src[sy * sw + sx]), got = dst[y * dw + x];
                for (k = 0; k < 24; k += 8) {
                    int a = (int)((want >> k) & 255), b = (int)((got >> k) & 255);
                    if (a - b > 8 || b - a > 8) {
                        bad = 1;
                    }
                }
                if (bad) {
                    fprintf(stderr, "video: teste do MI_GFX %d: pixel (%d,%d) esperado %06x, veio %08x "
                                    "(cantos: %08x %08x %08x %08x)\n", t, x, y, want, got, dst[0], dst[dw - 1],
                            dst[(dh - 1) * dw], dst[dh * dw - 1]);
                }
            }
        }
        if (!bad) {
            hw_rotate = tries[t][0];
            hw_mirror = tries[t][1];
            ok = 1;
        }
    }
out:
    if (vs) {
        mi.Munmap(vs, 4096);
    }
    if (vd) {
        mi.Munmap(vd, 4096);
    }
    if (ps) {
        mi.MmaFree(ps);
    }
    if (pd) {
        mi.MmaFree(pd);
    }
    return ok;
}
#endif

/* ---------------------------------------------------------------- 2D pelo acelerador (gfx/gfx_ge.c)
 * As pecas do HUD / do logo de cada imagem (texels em ARGB com a cor ja multiplicada pelo alfa, numa memoria
 * MMA por "parte") sao desenhadas pelo MI_GFX na pagina da tela depois da imagem, em 640x480, ampliadas
 * com a suavizacao dele. Um teste no comeco confere onde o MI_GFX poe um retangulo girado e se a mistura
 * esta certa (como no port do Mario Kart 64); qualquer duvida, e os nucleos desenham tudo. */
typedef struct {
    uint32_t off;
    uint16_t pw, ph, sw, sh;
    int16_t dx, dy, dw, dh;
} GxGeItem;
#define GX_GE_PARTS 8
extern uint8_t* (*gx_ge_mem)(int part, uint32_t* cap);
extern int gx_ge_items(const void* key, const GxGeItem** items, int* part);
extern const uint8_t* gx_ge_part_mem(int part, uint32_t* lo, uint32_t* hi);
#ifdef HAVE_MI_GFX
static MI_PHY ge_phys[GX_GE_PARTS];
static uint8_t* ge_virt[GX_GE_PARTS];
static uint32_t ge_cap;
static int ge_ok, ge_place;
static unsigned g_geShown, g_geItems; /* (log) */

static int ge_blit(MI_PHY src, int pw, int ph, const MI_GFX_Rect_t* sr, MI_PHY dst, int dw, int dh, const MI_GFX_Rect_t* dr,
                   int rotate, MI_U16* fence) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_ARGB8888, (MI_U32)pw, (MI_U32)ph, (MI_U32)pw * 4 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32)dw, (MI_U32)dh, (MI_U32)dw * 4 };
    MI_GFX_Rect_t srr = *sr, drr = *dr;
    MI_GFX_Opt_t opt;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect.s32Xpos = 0;
    opt.stClipRect.s32Ypos = 0;
    opt.stClipRect.u32Width = (MI_U32)dw;
    opt.stClipRect.u32Height = (MI_U32)dh;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE; /* (cor ja multiplicada pelo alfa) */
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_INVSRCALPHA;
    opt.eMirror = E_MI_GFX_MIRROR_NONE;
    opt.eRotate = (MI_GFX_Rotate_e)rotate;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_ALPHACHANNEL;
    {
        int r;
        pthread_mutex_lock(&g_blitMx);
        r = mi.Blit(&s, &srr, &d, &drr, &opt, fence) != 0;
        pthread_mutex_unlock(&g_blitMx);
        return r;
    }
}

static int ge_near(uint32_t got, uint32_t want) {
    int k;
    for (k = 0; k < 24; k += 8) {
        const int a = (int)((got >> k) & 255), b = (int)((want >> k) & 255);
        if (a - b > 6 || b - a > 6) {
            return 0;
        }
    }
    return 1;
}

/* onde um retangulo cai com o giro de 180 graus, e a mistura esta certa? */
static int ge_selftest(void) {
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    int ok = 0, i, x, y, nz = 0, bad = 0;
    char why[160] = "sem memoria";
    uint32_t *sp, *dp;
    MI_U16 fence = 0;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd) || mi.Mmap(ps, 4096, &vs, TRUE) ||
        mi.Mmap(pd, 4096, &vd, TRUE)) {
        goto out;
    }
    sp = vs;
    dp = vd;
    for (i = 0; i < 12; i++) {
        sp[i] = 0xFF0000FFu;
    }
    sp[5] = 0xFFFF0000u;
    sp[6] = 0xFF00FF00u;
    memset(vd, 0, 4096);
    mi.Flush(vs, 4096);
    mi.Flush(vd, 4096);
    {
        MI_GFX_Rect_t sr = { 1, 1, 2, 1 }, dr = { 1, 2, 2, 1 };
        if (ge_blit(ps, 4, 3, &sr, pd, 8, 8, &dr, E_MI_GFX_ROTATE_180, &fence)) {
            snprintf(why, sizeof why, "a copia com giro falhou");
            goto out;
        }
    }
    mi.Wait(TRUE, fence);
    mi.Flush(vd, 4096);
    for (i = 0; i < 64; i++) {
        nz += (dp[i] & 0xFFFFFFu) != 0;
    }
    if (nz == 2 && (dp[2 * 8 + 1] & 0xFFFFFF) == 0x00FF00 && (dp[2 * 8 + 2] & 0xFFFFFF) == 0xFF0000) {
        ge_place = 0;
    } else if (nz == 2 && (dp[5 * 8 + 5] & 0xFFFFFF) == 0x00FF00 && (dp[5 * 8 + 6] & 0xFFFFFF) == 0xFF0000) {
        ge_place = 1;
    } else {
        snprintf(why, sizeof why, "lugar inesperado (%d pixels)", nz);
        goto out;
    }
    /* mistura, ampliada 2x e girada: meio transparente (pre-multiplicado) sobre cinza */
    for (i = 0; i < 16; i++) {
        sp[i] = 0x80804000u;
    }
    for (i = 0; i < 64; i++) {
        dp[i] = 0xFF404040u;
    }
    mi.Flush(vs, 4096);
    mi.Flush(vd, 4096);
    {
        MI_GFX_Rect_t sr2 = { 1, 1, 2, 2 }, dr2 = { 1, 2, 4, 4 };
        if (ge_place) {
            dr2.s32Xpos = 8 - 1 - 4;
            dr2.s32Ypos = 8 - 2 - 4;
        }
        if (ge_blit(ps, 4, 4, &sr2, pd, 8, 8, &dr2, E_MI_GFX_ROTATE_180, &fence)) {
            snprintf(why, sizeof why, "a mistura falhou");
            goto out;
        }
    }
    mi.Wait(TRUE, fence);
    mi.Flush(vd, 4096);
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            const int gx = ge_place ? 7 - x : x, gy = ge_place ? 7 - y : y;
            const int in = gx >= 1 && gx < 5 && gy >= 2 && gy < 6;
            if (!ge_near(dp[y * 8 + x], in ? 0xA06020u : 0x404040u)) {
                bad++;
            }
        }
    }
    if (bad) {
        snprintf(why, sizeof why, "mistura errada em %d pixels", bad);
        goto out;
    }
    ok = 1;
out:
    if (vs) {
        mi.Munmap(vs, 4096);
    }
    if (vd) {
        mi.Munmap(vd, 4096);
    }
    if (ps) {
        mi.MmaFree(ps);
    }
    if (pd) {
        mi.MmaFree(pd);
    }
    if (!ok) {
        fprintf(stderr, "video: 2D pelo MI_GFX desligado (teste: %s), os nucleos desenham\n", why);
    }
    return ok;
}

static uint8_t* ge_mem_cb(int part, uint32_t* cap) {
    if (part < 0 || part >= GX_GE_PARTS || !ge_ok) {
        return NULL;
    }
    *cap = ge_cap;
    return ge_virt[part];
}

static void ge_init(void) {
    static const uint32_t sizes[] = { 768u << 10, 512u << 10, 256u << 10 };
    unsigned k;
    if (!ge_selftest()) {
        return;
    }
    for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int i;
        for (i = 0; i < GX_GE_PARTS; i++) {
            void* v = NULL;
            if (mi.MmaAlloc(NULL, sizes[k], &ge_phys[i])) {
                break;
            }
            if (mi.Mmap(ge_phys[i], sizes[k], &v, TRUE)) {
                mi.MmaFree(ge_phys[i]);
                break;
            }
            ge_virt[i] = v;
        }
        if (i == GX_GE_PARTS) {
            ge_cap = sizes[k];
            ge_ok = 1;
            gx_ge_mem = ge_mem_cb;
            fprintf(stderr, "video: 2D (HUD, logo) pelo MI_GFX, %u KB por imagem%s\n", (unsigned)(ge_cap >> 10),
                    ge_place ? " (o MI_GFX gira o lugar)" : "");
            return;
        }
        while (i-- > 0) {
            mi.Munmap(ge_virt[i], sizes[k]);
            mi.MmaFree(ge_phys[i]);
            ge_virt[i] = NULL;
        }
    }
    fprintf(stderr, "video: 2D pelo MI_GFX desligado (sem memoria MMA)\n");
}

/* tela: as pecas da imagem 'key' por cima da pagina 'back' (depois da imagem); espera o MI_GFX acabar */
static void ge_draw(const void* key, int back, int imgW, int imgH) {
    const GxGeItem* it;
    int n, part, i, done = 0;
    uint32_t lo, hi;
    const uint8_t* mem;
    MI_U16 fence = 0;
    const MI_PHY page = (MI_PHY)fb_phys + (MI_PHY)back * PANEL_W * PANEL_H * 4;
    if (!ge_ok || !hw_ok || key == NULL) {
        return;
    }
    n = gx_ge_items(key, &it, &part);
    if (n <= 0) {
        return;
    }
    mem = gx_ge_part_mem(part, &lo, &hi);
    if (hi > lo && mem != NULL) {
        mi.Flush((void*)(mem + lo), hi - lo); /* (os texels novos estao no cache do processador) */
    }
    /* as pecas vem em 640x480 (2x a tela do N64). Na tela de 752x560: a imagem cobre os 320 pixels de largura do N64
     * (x * 752/640) e as linhas que couberem na mesma escala (y: largura da imagem / altura dela) */
    const int scaled = PANEL_W != 640 || PANEL_H != 480;
    const float kx = (float)PANEL_W / 640.0f;
    const float ky = imgW > 0 && imgH > 0 ? (float)imgW * (float)PANEL_H / (640.0f * (float)imgH) : kx;
    for (i = 0; i < n; i++) {
        MI_GFX_Rect_t sr = { 1, 1, it[i].sw, it[i].sh };
        MI_GFX_Rect_t dr = { it[i].dx, it[i].dy, (MI_U32)it[i].dw, (MI_U32)it[i].dh };
        if (scaled) {
            const int x0 = (int)((float)it[i].dx * kx + 0.5f), x1 = (int)((float)(it[i].dx + it[i].dw) * kx + 0.5f);
            const int y0 = (int)((float)it[i].dy * ky + 0.5f), y1 = (int)((float)(it[i].dy + it[i].dh) * ky + 0.5f);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            dr.s32Xpos = x0;
            dr.s32Ypos = y0;
            dr.u32Width = (MI_U32)(x1 - x0);
            dr.u32Height = (MI_U32)(y1 - y0);
        }
        if (!ge_place) { /* (a tela e de cabeca para baixo: o lugar girado aqui) */
            dr.s32Xpos = PANEL_W - dr.s32Xpos - (int)dr.u32Width;
            dr.s32Ypos = PANEL_H - dr.s32Ypos - (int)dr.u32Height;
        }
        if (ge_blit(ge_phys[part] + it[i].off, it[i].pw, it[i].ph, &sr, page, PANEL_W, PANEL_H, &dr, hw_rotate, &fence) == 0) {
            done++;
        }
    }
    if (done) {
        mi.Wait(TRUE, fence);
    }
    g_geShown++;
    g_geItems += (unsigned)done;
}

/* ---------------------------------------------------------------- filtro retro (opcao retro_filter)
 * Uma camada de 640x480 com o desenho da TV de tubo (as linhas escuras entre as linhas do jogo e, no modo 2, a mascara
 * de colunas vermelho/verde/azul), aplicada pelo MI_GFX por cima da imagem pronta (depois do HUD): o chip MULTIPLICA a
 * cor da tela pela da camada (cada canal), sem trabalho para os nucleos. Um teste confere se ele multiplica; se nao,
 * a camada so escurece pela transparencia (a mascara vira listras mais escuras, sem cor). A camada e feita na
 * orientacao de quem olha e vai com o mesmo giro da imagem do jogo. */
static MI_PHY rt_phys;
static uint32_t* rt_virt;
static int rt_mode = -1, rt_forca = -1, rt_tvb = -1;
static int rt_mul = -1; /* -1 = falta o teste; 1 = multiplica; 0 = so a transparencia; -2 = sem memoria */
static int rt_sop, rt_dop;

static int rt_blit(MI_PHY src, MI_PHY dst, int w, int h, int sop, int dop, int rotate, int mirror, MI_U16* fence) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_ARGB8888, (MI_U32)w, (MI_U32)h, (MI_U32)w * 4 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32)w, (MI_U32)h, (MI_U32)w * 4 };
    MI_GFX_Rect_t r = { 0, 0, (MI_U32)w, (MI_U32)h };
    MI_GFX_Opt_t opt;
    int e;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect = r;
    opt.eSrcDfbBldOp = (MI_GFX_DfbBldOp_e)sop;
    opt.eDstDfbBldOp = (MI_GFX_DfbBldOp_e)dop;
    opt.eMirror = (MI_GFX_Mirror_e)mirror;
    opt.eRotate = (MI_GFX_Rotate_e)rotate;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_ALPHACHANNEL;
    pthread_mutex_lock(&g_blitMx);
    e = mi.Blit(&s, &r, &d, &r, &opt, fence) != 0;
    pthread_mutex_unlock(&g_blitMx);
    return e;
}

/* o MI_GFX multiplica as cores? (camada 0x80FF40 sobre cinza 0x808080: tem de dar 0x408020) */
static int rt_test(void) {
    static const int ops[2][2] = { { E_MI_GFX_DFB_BLD_DESTCOLOR, E_MI_GFX_DFB_BLD_ZERO },
                                   { E_MI_GFX_DFB_BLD_ZERO, E_MI_GFX_DFB_BLD_SRCCOLOR } };
    MI_PHY ps = 0, pd = 0;
    void *vs = NULL, *vd = NULL;
    int ok = 0, k, i;
    if (mi.MmaAlloc(NULL, 4096, &ps) || mi.MmaAlloc(NULL, 4096, &pd) || mi.Mmap(ps, 4096, &vs, TRUE) ||
        mi.Mmap(pd, 4096, &vd, TRUE)) {
        goto out;
    }
    for (k = 0; k < 2 && !ok; k++) {
        uint32_t* sp = vs;
        uint32_t* dp = vd;
        MI_U16 fence = 0;
        for (i = 0; i < 16; i++) {
            sp[i] = 0xFF80FF40u;
            dp[i] = 0xFF808080u;
        }
        mi.Flush(vs, 4096);
        mi.Flush(vd, 4096);
        if (rt_blit(ps, pd, 16, 1, ops[k][0], ops[k][1], E_MI_GFX_ROTATE_0, E_MI_GFX_MIRROR_NONE, &fence) != 0) {
            continue;
        }
        mi.Wait(TRUE, fence);
        mi.Flush(vd, 4096);
        ok = ge_near(dp[5], 0x408020u) && ge_near(dp[10], 0x408020u);
        fprintf(stderr, "video: filtro retro, teste da multiplicacao %d: %08X (%s)\n", k, (unsigned)dp[5], ok ? "ok" : "nao");
        if (ok) {
            rt_sop = ops[k][0];
            rt_dop = ops[k][1];
        }
    }
out:
    if (vs) {
        mi.Munmap(vs, 4096);
    }
    if (vd) {
        mi.Munmap(vd, 4096);
    }
    if (ps) {
        mi.MmaFree(ps);
    }
    if (pd) {
        mi.MmaFree(pd);
    }
    return ok;
}

/* A camada inteira (640x480: 1,2 MB; 752x560: 1,7 MB) e pedida quando o filtro liga. Ligado no meio do jogo, a memoria do
 * chip 2D ja pode estar tomada pelas imagens (na tela de 752x560 elas sao maiores): "filtro retro sem memoria" e o filtro
 * nao aparecia ate reabrir o jogo (relato do Mini Flip, 11/2026). Reserva: uma faixa de RT_STRIP linhas (o desenho se
 * repete a cada 2 linhas e 3 colunas), pedida na abertura e passada varias vezes, ja na orientacao da pagina (sem giro). */
#define RT_STRIP 80
static MI_PHY rs_phys;
static uint32_t* rs_virt;
/* com a tela de TV (opcao retro_filter_tv), na tela de 752x560: cada faixa e diferente, entao a camada inteira e montada
 * ja girada (na ordem da pagina) e vai em faixas sem giro, como a de sempre */
static MI_PHY rs_fullp;
static uint32_t* rs_full;
static int rt_strip; /* 1 = a camada e a faixa */

static void rt_reserve(void) {
    void* v = NULL;
    const MI_U32 bytes = (MI_U32)(PANEL_W * RT_STRIP * 4);
    if (PANEL_H % RT_STRIP != 0 || mi.MmaAlloc(NULL, bytes, &rs_phys) != 0) {
        rs_phys = 0;
        return;
    }
    if (mi.Mmap(rs_phys, bytes, &v, TRUE) != 0) {
        mi.MmaFree(rs_phys);
        rs_phys = 0;
        return;
    }
    rs_virt = (uint32_t*)v;
}

/* tela de TV de tubo (opcao retro_filter_tv): quanto sobra do ponto (0..256), na orientacao de quem olha. A forma de
 * uma tela de tubo (ajustada pixel a pixel ao contorno de uma foto de referencia, 11/2026): as quatro bordas curvas de
 * ponta a ponta (cada lado abaulado, entrando 2,5% da largura nos cantos e 4,5% da altura em cima e embaixo), os
 * cantos arredondados em "superelipse" (raio de 8% da altura, norma 4: mais cheios que um circulo), uma borda preta
 * minima (1% da altura) e um leve escurecido so junto da borda (ate 40% na borda, numa faixa de 2,5% da altura). Feito so quando a camada e montada: no desenho de cada quadro nao custa nada (a mesma camada). */
static unsigned rt_frame(int tv, int x, int y) {
    const float W = (float)PANEL_W, H = (float)PANEL_H, m = H * 0.01f, r = H * 0.08f, band = H * 0.025f;
    const float u = fabsf(((float)x + 0.5f) / W * 2.0f - 1.0f), v = fabsf(((float)y + 0.5f) / H * 2.0f - 1.0f);
    float dx, dy, qx, qy, ox, oy, d, t, f, cov;
    if (!tv) {
        return 256u;
    }
    dx = (1.0f - 0.025f * v * v - u) * W * 0.5f - m; /* pixels ate o lado curvo */
    dy = (1.0f - 0.045f * u * u - v) * H * 0.5f - m; /* pixels ate a borda de cima/baixo curva */
    qx = r - dx;
    qy = r - dy;
    ox = qx > 0.0f ? qx : 0.0f;
    oy = qy > 0.0f ? qy : 0.0f;
    d = r - sqrtf(sqrtf(ox * ox * ox * ox + oy * oy * oy * oy)) - (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f));
    cov = d + 0.5f;
    if (cov <= 0.0f) {
        return 0u;
    }
    cov = cov > 1.0f ? 1.0f : cov;
    t = d < band ? 1.0f - d / band : 0.0f;
    f = cov * (1.0f - 0.4f * t * t);
    return (unsigned)(f * 256.0f + 0.5f);
}

/* um ponto da camada; x, y na orientacao de quem olha (a linha 2j e a do jogo, a 2j+1 a escura) */
static uint32_t rt_px(int mode, int x, int y, unsigned line, unsigned mk, unsigned fr) {
    const unsigned l = (y & 1) ? line : 255u;
    unsigned c[3] = { l, l, l };
    if (mode == 2) {
        int k;
        for (k = 0; k < 3; k++) {
            if (k != x % 3) {
                c[k] = c[k] * mk / 255u;
            }
        }
    }
    if (fr < 256u) {
        c[0] = c[0] * fr >> 8;
        c[1] = c[1] * fr >> 8;
        c[2] = c[2] * fr >> 8;
    }
    if (rt_mul == 1) {
        return 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
    }
    /* so a transparencia: preto (ja multiplicado pelo alfa) que tira a parte que falta do brilho */
    return (uint32_t)(255u - (c[0] + c[1] + c[2]) / 3u) << 24;
}

/* uma faixa da pagina que comeca na linha dy da pagina (a pagina e girada 180: linha r e coluna c da pagina = linha
 * PANEL_H-1-r e coluna PANEL_W-1-c de quem olha) */
static void rt_build_strip(uint32_t* buf, int dy, int mode, unsigned line, unsigned mk, int tv) {
    int x, y;
    for (y = 0; y < RT_STRIP; y++) {
        uint32_t* row = buf + (size_t)y * PANEL_W;
        const int vy = PANEL_H - 1 - (dy + y);
        for (x = 0; x < PANEL_W; x++) {
            const int vx = PANEL_W - 1 - x;
            row[x] = rt_px(mode, vx, vy, line, mk, rt_frame(tv, vx, vy));
        }
    }
    mi.Flush(buf, (MI_U32)(PANEL_W * RT_STRIP * 4));
}

static int rt_build(int mode, int forca, int tv) {
    const size_t n = (size_t)PANEL_W * PANEL_H;
    int x, y;
    unsigned line, mk;
    if (rt_virt == NULL && !rt_strip && rs_virt != NULL && (PANEL_W != 640 || getenv("OOT_FILTRO_FAIXA") != NULL)) {
        /* na tela de 752x560, sempre a faixa: a camada inteira (752x560 girada 180 pelo chip 2D) saia com uma faixa de
         * ~8 linhas sem o filtro perto de baixo (imagem do Game Switcher do Mini Flip, 11/2026); a faixa vai sem giro.
         * OOT_FILTRO_FAIXA: o mesmo na tela de 640x480 (teste) */
        rt_strip = 1;
        fprintf(stderr, "video: filtro retro em faixas de %d linhas\n", RT_STRIP);
    }
    if (rt_virt == NULL && !rt_strip) {
        void* v = NULL;
        if (mi.MmaAlloc(NULL, (MI_U32)(n * 4), &rt_phys) == 0 && mi.Mmap(rt_phys, (MI_U32)(n * 4), &v, TRUE) == 0) {
            rt_virt = (uint32_t*)v;
        } else {
            if (rt_phys) {
                mi.MmaFree(rt_phys);
            }
            rt_phys = 0;
            if (rs_virt == NULL) {
                return 0;
            }
            rt_strip = 1;
            fprintf(stderr, "video: filtro retro em faixas de %d linhas (sem memoria para a camada inteira)\n", RT_STRIP);
        }
    }
    forca = forca < 10 ? 10 : forca > 90 ? 90 : forca;
    line = 255u - 255u * (unsigned)forca / 100u;      /* linha escura */
    mk = 255u - 255u * (unsigned)forca / 200u;        /* mascara: os 2 canais que nao sao o da coluna */
    if (rt_strip) {
        /* com RT_STRIP par, cada faixa da pagina tem as mesmas linhas pares e impares; a do meio vale para todas as faixas
         * do meio (a borda escurecida de cima e de baixo cabe numa faixa) */
        rt_build_strip(rs_virt, RT_STRIP, mode, line, mk, 0);
        if (tv && rs_full == NULL) {
            void* v = NULL;
            const MI_U32 bytes = (MI_U32)(n * 4);
            if (mi.MmaAlloc(NULL, bytes, &rs_fullp) == 0 && mi.Mmap(rs_fullp, bytes, &v, TRUE) == 0) {
                rs_full = (uint32_t*)v;
            } else {
                if (rs_fullp) {
                    mi.MmaFree(rs_fullp);
                }
                rs_fullp = 0;
                fprintf(stderr, "video: filtro retro: sem memoria para a tela de TV (fica sem ela)\n");
            }
        }
        if (tv && rs_full != NULL) {
            for (y = 0; y < PANEL_H; y += RT_STRIP) {
                rt_build_strip(rs_full + (size_t)y * PANEL_W, y, mode, line, mk, tv);
            }
        }
    } else {
        for (y = 0; y < PANEL_H; y++) {
            uint32_t* row = rt_virt + (size_t)y * PANEL_W;
            for (x = 0; x < PANEL_W; x++) {
                row[x] = rt_px(mode, x, y, line, mk, rt_frame(tv, x, y));
            }
        }
        mi.Flush(rt_virt, (MI_U32)(n * 4));
    }
    fprintf(stderr, "video: filtro retro %s, forca %d%%%s (%s)\n", mode == 2 ? "linhas e mascara" : "linhas", forca,
            tv ? ", cantos de TV" : "", rt_mul == 1 ? "multiplicando as cores" : "so escurecendo");
    return 1;
}

/* a faixa por cima das linhas dy..dy+RT_STRIP-1 da pagina */
static int rt_blit_strip(MI_PHY src, MI_PHY dst, int dy, int sop, int dop, MI_U16* fence) {
    MI_GFX_Surface_t s = { src, E_MI_GFX_FMT_ARGB8888, (MI_U32)PANEL_W, (MI_U32)RT_STRIP, (MI_U32)PANEL_W * 4 };
    MI_GFX_Surface_t d = { dst, E_MI_GFX_FMT_ARGB8888, (MI_U32)PANEL_W, (MI_U32)PANEL_H, (MI_U32)PANEL_W * 4 };
    MI_GFX_Rect_t sr = { 0, 0, (MI_U32)PANEL_W, (MI_U32)RT_STRIP }, dr = { 0, dy, (MI_U32)PANEL_W, (MI_U32)RT_STRIP };
    MI_GFX_Opt_t opt;
    int e;
    memset(&opt, 0, sizeof opt);
    opt.stClipRect = dr;
    opt.eSrcDfbBldOp = (MI_GFX_DfbBldOp_e)sop;
    opt.eDstDfbBldOp = (MI_GFX_DfbBldOp_e)dop;
    opt.eMirror = E_MI_GFX_MIRROR_NONE;
    opt.eRotate = E_MI_GFX_ROTATE_0;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_ALPHACHANNEL;
    pthread_mutex_lock(&g_blitMx);
    e = mi.Blit(&s, &sr, &d, &dr, &opt, fence) != 0;
    pthread_mutex_unlock(&g_blitMx);
    return e;
}

static void rt_draw(int back) {
    extern int gx_retro, gx_retro_forca, gx_retro_tv; /* gfx_hires.c */
    const int mode = gx_retro, tv = gx_retro_tv != 0;
    const MI_PHY page = (MI_PHY)fb_phys + (MI_PHY)back * PANEL_W * PANEL_H * 4;
    MI_U16 fence = 0;
    int sop, dop;
    if (mode <= 0 || !hw_ok || rt_mul == -2) {
        return;
    }
    if (rt_mul == -1) {
        rt_mul = rt_test();
    }
    if (mode != rt_mode || gx_retro_forca != rt_forca || tv != rt_tvb) {
        if (!rt_build(mode, gx_retro_forca, tv)) {
            fprintf(stderr, "video: filtro retro sem memoria\n");
            rt_mul = -2;
            return;
        }
        rt_mode = mode;
        rt_forca = gx_retro_forca;
        rt_tvb = tv;
    }
    sop = rt_mul == 1 ? rt_sop : E_MI_GFX_DFB_BLD_ONE;
    dop = rt_mul == 1 ? rt_dop : E_MI_GFX_DFB_BLD_INVSRCALPHA;
    if (rt_strip) {
        int y, sent = 0;
        for (y = 0; y < PANEL_H; y += RT_STRIP) {
            const MI_PHY src = tv && rs_full != NULL ? rs_fullp + (MI_PHY)y * PANEL_W * 4 : rs_phys;
            sent += rt_blit_strip(src, page, y, sop, dop, &fence) == 0;
        }
        if (sent) {
            mi.Wait(TRUE, fence);
        }
        return;
    }
    if (rt_blit(rt_phys, page, PANEL_W, PANEL_H, sop, dop, hw_rotate, hw_mirror, &fence) == 0) {
        mi.Wait(TRUE, fence);
    }
}
#endif

static int hw_init(void) {
#ifdef HAVE_MI_GFX
    void *hs, *hg;
    const MI_U32 bytes = SRC_MAX_W * SRC_MAX_H * 2;
    int i;
    if (miyoo_opt_nogfx || !fb_phys || fb_pages != 2) {
        return 0;
    }
    hs = open_lib("libmi_sys.so");
    hg = open_lib("libmi_gfx.so");
    if (!hs || !hg) {
        fprintf(stderr, "video: sem libmi_sys/libmi_gfx, copia pela CPU\n");
        return 0;
    }
    mi.SysInit = dlsym(hs, "MI_SYS_Init");
    mi.MmaAlloc = dlsym(hs, "MI_SYS_MMA_Alloc");
    mi.MmaFree = dlsym(hs, "MI_SYS_MMA_Free");
    mi.Mmap = dlsym(hs, "MI_SYS_Mmap");
    mi.Munmap = dlsym(hs, "MI_SYS_Munmap");
    mi.Flush = dlsym(hs, "MI_SYS_FlushInvCache");
    mi.GfxOpen = dlsym(hg, "MI_GFX_Open");
    mi.GfxClose = dlsym(hg, "MI_GFX_Close");
    mi.Blit = dlsym(hg, "MI_GFX_BitBlit");
    mi.Wait = dlsym(hg, "MI_GFX_WaitAllDone");
    if (!mi.SysInit || !mi.MmaAlloc || !mi.MmaFree || !mi.Mmap || !mi.Munmap || !mi.Flush || !mi.GfxOpen ||
        !mi.GfxClose || !mi.Blit || !mi.Wait) {
        fprintf(stderr, "video: MI_GFX incompleto, copia pela CPU\n");
        return 0;
    }
    if (mi.SysInit() != 0 || mi.GfxOpen() != 0) {
        fprintf(stderr, "video: MI_GFX nao abriu, copia pela CPU\n");
        return 0;
    }
    if (!hw_selftest()) {
        fprintf(stderr, "video: teste do MI_GFX falhou, copia pela CPU\n");
        mi.GfxClose();
        return 0;
    }
    for (i = 0; i < 2; i++) {
        void* v = NULL;
        if (mi.MmaAlloc(NULL, bytes, &hw_phys[i]) || mi.Mmap(hw_phys[i], bytes, &v, TRUE)) {
            int k;
            fprintf(stderr, "video: sem memoria MMA, copia pela CPU\n");
            for (k = 0; k <= i; k++) {
                if (hw_phys[k]) {
                    mi.MmaFree(hw_phys[k]);
                }
            }
            mi.GfxClose();
            return 0;
        }
        memset(v, 0, bytes);
        conv_buf[i] = v;
    }
    fmt_probe();
    fprintf(stderr, "video: copia para a tela pelo MI_GFX (%s)\n",
            hw_rotate == E_MI_GFX_ROTATE_180 ? "giro 180" : "espelho");
    hw_ok = 1; /* (ge_init usa) */
    {
        extern int gx_hr_le;
        extern void *(*gx_img_alloc)(size_t bytes);
        extern int (*gx_ge_bg)(uint8_t *, int, int, const uint8_t *, int, int, int, int, int, int, int, int, int, int);
        if (gx_hr_le) {
            gx_img_alloc = img_mma_alloc; /* (imagens da fila na memoria do chip 2D) */
            gx_ge_bg = ge_bg_blit;        /* (fundos JPEG pelo chip 2D, gfx_rdp.c) */
        }
    }
    rt_reserve(); /* (a faixa do filtro retro, antes do resto pegar a memoria do chip 2D) */
    if (hw_rotate == E_MI_GFX_ROTATE_180 && hw_mirror == E_MI_GFX_MIRROR_NONE) {
        ge_init();
    } else {
        fprintf(stderr, "video: 2D pelo MI_GFX desligado (a tela so sai espelhada)\n");
    }
    return 1;
#else
    return 0;
#endif
}

/* ---------------------------------------------------------------- conversao */

/* N64 RGBA5551 (bytes do N64) -> RGB565: R igual, G de 5 para 6 bits, B no lugar */
static inline uint16_t n64_to_565(uint16_t be) {
    uint16_t v = (uint16_t)((be >> 8) | (be << 8));
    return (uint16_t)((v & 0xFFC0) | ((v >> 5) & 0x20) | ((v >> 1) & 0x1F));
}

/* o mesmo, com a cor ja na ordem do processador (imagens da fila com a opcao cor_direta) */
static inline uint16_t le_to_565(uint16_t v) {
    return (uint16_t)((v & 0xFFC0) | ((v >> 5) & 0x20) | ((v >> 1) & 0x1F));
}

static void convert_le(const uint16_t* src, uint16_t* dst, int npix) {
    int i = 0;
#ifdef VID_NEON
    for (; i + 8 <= npix; i += 8) {
        uint16x8_t v = vld1q_u16(src + i);
        uint16x8_t o = vbicq_u16(v, vdupq_n_u16(0x003F));
        o = vorrq_u16(o, vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x20)));
        o = vorrq_u16(o, vandq_u16(vshrq_n_u16(v, 1), vdupq_n_u16(0x1F)));
        vst1q_u16(dst + i, o);
    }
#endif
    for (; i < npix; i++) {
        dst[i] = le_to_565(src[i]);
    }
}

static void convert_n64(const uint16_t* src, uint16_t* dst, int npix) {
    int i = 0;
#ifdef VID_NEON
    for (; i + 8 <= npix; i += 8) {
        uint16x8_t v = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(vld1q_u16(src + i))));
        uint16x8_t o = vbicq_u16(v, vdupq_n_u16(0x003F)); /* v & 0xFFC0 */
        o = vorrq_u16(o, vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x20)));
        o = vorrq_u16(o, vandq_u16(vshrq_n_u16(v, 1), vdupq_n_u16(0x1F)));
        vst1q_u16(dst + i, o);
    }
#endif
    for (; i < npix; i++) {
        dst[i] = n64_to_565(src[i]);
    }
}

/* sem MI_GFX: RGB565 (w x h) -> pagina XRGB8888 640x480, escalado e girado 180.
 * 320x240 (o normal): cada linha e montada espelhada e dobrada num buffer (NEON) e escrita
 * em duas linhas da tela; a memoria da tela e rapida para escrever e muito lenta para ler. */
static uint32_t line_buf[PANEL_MAX_W] __attribute__((aligned(16)));

static inline uint32_t x8888(uint32_t c) {
    return ((c & 0xF800u) << 8) | ((c & 0xE000u) << 3) | ((c & 0x07E0u) << 5) | ((c & 0x0600u) >> 1) |
           ((c & 0x001Fu) << 3) | ((c & 0x001Cu) >> 2);
}

static void row_mirror_double(const uint16_t* s, int w) {
    int x;
#ifdef VID_NEON
    if ((w & 7) == 0) {
        uint8_t* d = (uint8_t*)line_buf;
        const uint8x8_t zero = vdup_n_u8(0);
        for (x = w - 8; x >= 0; x -= 8) {
            uint16x8_t v = vrev64q_u16(vld1q_u16(s + x));
            uint8x8_t r, g, b, tr, tg, tb;
            uint8x8x2_t zb, zg, zr;
            uint8x8x4_t a, c;
            v = vcombine_u16(vget_high_u16(v), vget_low_u16(v)); /* 8 pixels de tras para frente */
            tr = vshrn_n_u16(v, 8);
            tg = vshrn_n_u16(v, 3);
            tb = vmovn_u16(vshlq_n_u16(v, 3));
            r = vsri_n_u8(tr, tr, 5);
            g = vsri_n_u8(tg, tg, 6);
            b = vsri_n_u8(tb, tb, 5);
            zb = vzip_u8(b, b);
            zg = vzip_u8(g, g);
            zr = vzip_u8(r, r);
            a.val[0] = zb.val[0];
            a.val[1] = zg.val[0];
            a.val[2] = zr.val[0];
            a.val[3] = zero;
            c.val[0] = zb.val[1];
            c.val[1] = zg.val[1];
            c.val[2] = zr.val[1];
            c.val[3] = zero;
            vst4_u8(d, a);
            vst4_u8(d + 32, c);
            d += 64;
        }
        return;
    }
#endif
    for (x = 0; x < w; x++) {
        uint32_t c = x8888(s[w - 1 - x]);
        line_buf[x * 2] = c;
        line_buf[x * 2 + 1] = c;
    }
}

static void cpu_present(const uint16_t* src, int w, int h, uint32_t* dst) {
    int x, y;
    if (w * 2 == PANEL_W && h * 2 == PANEL_H) {
        for (y = 0; y < h; y++) {
            uint32_t* row = dst + (size_t)(PANEL_H - 1 - 2 * y) * PANEL_W;
            row_mirror_double(src + (size_t)y * w, w);
            memcpy(row, line_buf, PANEL_W * 4);
            memcpy(row - PANEL_W, line_buf, PANEL_W * 4);
        }
        return;
    }
    for (y = 0; y < PANEL_H; y++) {
        const uint16_t* row = src + (size_t)((PANEL_H - 1 - y) * h / PANEL_H) * w;
        uint32_t* d = dst + (size_t)y * PANEL_W;
        for (x = 0; x < PANEL_W; x++) {
            uint32_t c = row[(PANEL_W - 1 - x) * w / PANEL_W];
            d[x] = ((c & 0xF800u) << 8) | ((c & 0xE000u) << 3) | ((c & 0x07E0u) << 5) | ((c & 0x0600u) >> 1) |
                   ((c & 0x001Fu) << 3) | ((c & 0x001Cu) >> 2);
        }
    }
}

/* ---------------------------------------------------------------- telas de mensagem */

static void put_text_px(uint32_t* page, int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) {
        return;
    }
    page[(size_t)(PANEL_H - 1 - y) * PANEL_W + (PANEL_W - 1 - x)] = rgb; /* painel girado */
}

/* letras numa linha (UTF-8: acentuadas U+00A0..U+00FF ocupam 2 bytes e uma letra) */
static int utf8_chars(const char* s, int len) {
    int i, n = 0;
    for (i = 0; i < len; i++) {
        n += ((unsigned char)s[i] & 0xC0) != 0x80;
    }
    return n;
}

static void draw_text_line(uint32_t* page, int y, const char* s, int len, int scale, uint32_t rgb) {
    int x0 = (PANEL_W - utf8_chars(s, len) * 8 * scale) / 2, i, col = 0, gy, gx, sy, sx;
    for (i = 0; i < len; i++, col++) {
        unsigned int c = (unsigned char)s[i];
        const unsigned char* g;
        if ((c == 0xC2 || c == 0xC3) && i + 1 < len && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | ((unsigned char)s[++i] & 0x3F);
            g = c >= 0xA0 ? font_latin1[c - 0xA0] : font8x16['?' - 32];
        } else {
            if (c < 32 || c > 126) {
                c = '?';
            }
            g = font8x16[c - 32];
        }
        for (gy = 0; gy < 16; gy++) {
            for (gx = 0; gx < 8; gx++) {
                if (g[gy] & (0x80 >> gx)) {
                    for (sy = 0; sy < scale; sy++) {
                        for (sx = 0; sx < scale; sx++) {
                            put_text_px(page, x0 + (col * 8 + gx) * scale + sx, y + gy * scale + sy, rgb);
                        }
                    }
                }
            }
        }
    }
}

void miyoo_video_message(const char* text, uint32_t rgb) {
    const char* p;
    int lines = 1, y, page;
    uint32_t* dst;
    if (fb_mem == NULL) {
        fprintf(stderr, "[tela] %s\n", text);
        return;
    }
    for (p = text; *p; p++) {
        lines += *p == '\n';
    }
    page = fb_pages == 2 ? fb_page ^ 1 : 0;
    dst = fb_page_ptr(page);
    memset(dst, 0, (size_t)PANEL_W * PANEL_H * 4);
    y = (PANEL_H - lines * 40) / 2;
    for (p = text;;) {
        const char* e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        int scale = utf8_chars(p, len) * 16 <= PANEL_W - 32 ? 2 : 1;
        draw_text_line(dst, y + (scale == 1 ? 8 : 0), p, len, scale, rgb);
        y += 40;
        if (!e) {
            break;
        }
        p = e + 1;
    }
    if (fb_pages == 2) {
        fb_var.yoffset = (unsigned)(page * PANEL_H);
        ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
        fb_page = page;
    }
    fprintf(stderr, "[tela] %s\n", text);
}

/* aviso curto por cima do jogo (estado rapido: "Estado salvo"...), ~1,5 s no alto da tela */
static char g_toast[64];
static uint32_t* g_menuBg;  /* menu de opcoes: a ultima imagem do jogo, escurecida */
static uint32_t* g_menuSrc; /* a mesma imagem sem escurecer (para refazer o fundo se o brilho mudar no menu) */
static int g_menuSrcLv, g_menuBgLv; /* brilho (gamma) com que a imagem foi desenhada / do fundo atual */

/* fundo do menu = imagem guardada, levada do brilho com que foi desenhada para o brilho de agora (a mesma curva do
 * gfx_rdp.c: c^(1/(1+0,08*nivel))), com 1/4 da luz */
static void menu_bg_build(void) {
    extern int gx_brilho;
    const size_t n = (size_t)MENU_W * MENU_H;
    const int lv = gx_brilho < 0 ? 0 : gx_brilho > 10 ? 10 : gx_brilho;
    uint32_t lut[256];
    size_t i;
    int c;
    const double e = (1.0 + 0.08 * g_menuSrcLv) / (1.0 + 0.08 * lv);
    for (c = 0; c < 256; c++) {
        lut[c] = (uint32_t)(255.0 * pow(c / 255.0, e) + 0.5) >> 2; /* (1/4 do brilho) */
    }
    for (i = 0; i < n; i++) {
        const uint32_t p = g_menuSrc[i];
        g_menuBg[i] = (lut[(p >> 16) & 255] << 16) | (lut[(p >> 8) & 255] << 8) | lut[p & 255];
    }
    g_menuBgLv = lv;
}
static int g_menuShown;
#define MTHUMB_W 256
#define MTHUMB_H 192
/* a imagem do jogo quando o menu abriu, 256x192 RGB565 (malloc: os dados estaticos tem de acabar antes de 0x81000000) */
static uint16_t* g_menuThumb;
static volatile int g_menuThumbOk;

/* port/savestate.c: a miniatura de um estado salvo pelo menu */
int miyoo_video_thumb(uint16_t* out) {
    if (!g_menuThumbOk) {
        return 0;
    }
    memcpy(out, g_menuThumb, MTHUMB_W * MTHUMB_H * 2);
    return 1;
}
static volatile double g_toastUntil;

void miyoo_video_toast(const char* text) {
    snprintf(g_toast, sizeof g_toast, "%s", text);
    g_toastUntil = miyoo_now_ms() + 1500.0;
}

/* o icone de bateria fraca do batmon (contorno vermelho, quase vazio, 15 pixels do topo e da direita), na
 * pagina final: por cima da imagem, do HUD pelo acelerador e da caixa de texto. O batmon nao o desenha enquanto o
 * jogo roda (/tmp/hasBatteryDisplay, battery_miyoo.c): numa tela que troca de pagina a cada quadro ele piscaria. */
static void battery_draw(uint32_t* page) {
    const uint32_t red = 0xFF0000u;
    const int x0 = PANEL_W - 15 - 30 - 4, y0 = 15, bw = 30, bh = 15, hw = 4;
    int x, y;
    if (!miyoo_battery_low()) {
        return;
    }
    for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++) {
            if (y == 0 || y == bh - 1 || x == 0 || x == bw - 1 || (x >= 3 && x < 5 && y >= 3 && y < bh - 3)) {
                put_text_px(page, x0 + x, y0 + y, red);
            }
        }
        if (y >= 4 && y < bh - 4) {
            for (x = bw; x < bw + hw; x++) {
                put_text_px(page, x0 + x, y0 + y, red);
            }
        }
    }
}

/* a pagina na tela agora (imagem do Game Switcher, port/savestate.c): 640x480, girada 180 graus */
/* a tela de 752x560 vai reduzida para 640x480 (o tamanho que o Game Switcher recebeu do jogo nas versoes anteriores, que
 * ele mostra certo). Pelo vizinho mais proximo, as linhas do filtro retro ficavam embaralhadas na imagem (relato do Mini
 * Flip, 11/2026); nem a media de area resolve (cada ponto cobre 1,17 linha: sobram faixas). Cada ponto e a media de 2
 * linhas vizinhas (uma do jogo e uma escura: o padrao das linhas some por igual) e 3 colunas (o da mascara de cor) */
static void page_to_small(const uint32_t* src, uint32_t* dst) {
    static uint16_t xs[MENU_W];
    int x, y, c;
    for (x = 0; x < MENU_W; x++) {
        int cx = ((2 * x + 1) * PANEL_W) / (2 * MENU_W) - 1;
        xs[x] = (uint16_t)(cx < 0 ? 0 : cx > PANEL_W - 3 ? PANEL_W - 3 : cx);
    }
    for (y = 0; y < MENU_H; y++) {
        int cy = ((2 * y + 1) * PANEL_H) / (2 * MENU_H);
        const uint32_t *r0, *r1;
        cy = cy < 1 ? 1 : cy > PANEL_H - 1 ? PANEL_H - 1 : cy;
        r0 = src + (size_t)(cy - 1) * PANEL_W;
        r1 = src + (size_t)cy * PANEL_W;
        for (x = 0; x < MENU_W; x++) {
            uint32_t o = 0;
            const int x0 = xs[x];
            for (c = 0; c < 24; c += 8) {
                const unsigned v = ((r0[x0] >> c) & 255u) + ((r0[x0 + 1] >> c) & 255u) + ((r0[x0 + 2] >> c) & 255u) +
                                   ((r1[x0] >> c) & 255u) + ((r1[x0 + 1] >> c) & 255u) + ((r1[x0 + 2] >> c) & 255u);
                o |= (uint32_t)((v + 3u) / 6u) << c;
            }
            dst[(size_t)y * MENU_W + x] = o;
        }
    }
}

/* A thread de tela continua trabalhando enquanto o jogo copia a tela para a imagem do Game Switcher (~100 ms): ela troca
 * de pagina e desenha o quadro seguinte na que estava sendo lida (a imagem, depois o HUD, por ultimo o filtro retro). As
 * linhas lidas entre a imagem nova e o filtro saiam sem ele: uma faixa clara em lugar diferente a cada vez (Mini Flip,
 * 11/2026). A copia agora e feita com a thread de tela parada entre dois quadros (video_hold) */
static int g_vhold, g_vidle;
static void video_hold(int on);

int miyoo_video_front(const uint32_t** page) {
    static uint32_t* snap;
    if (fb_mem == NULL) {
        return 0;
    }
    if (snap == NULL && (snap = (uint32_t*)malloc((size_t)MENU_W * MENU_H * 4)) == NULL) {
        return 0;
    }
    video_hold(1);
    if (PANEL_W == MENU_W && PANEL_H == MENU_H) {
        memcpy(snap, fb_page_ptr(fb_page), (size_t)MENU_W * MENU_H * 4);
    } else {
        page_to_small(fb_page_ptr(fb_page), snap);
    }
    video_hold(0);
    *page = snap;
    return 1;
}

/* pagina da tela (girada) -> imagem de 640x480 (girada), vizinho mais proximo */
static void page_to_menu(const uint32_t* src, uint32_t* dst) {
    static uint16_t xs[MENU_W];
    int x, y;
    for (x = 0; x < MENU_W; x++) {
        xs[x] = (uint16_t)(((2 * x + 1) * PANEL_W) / (2 * MENU_W));
    }
    for (y = 0; y < MENU_H; y++) {
        const uint32_t* s = src + (size_t)(((2 * y + 1) * PANEL_H) / (2 * MENU_H)) * PANEL_W;
        uint32_t* d = dst + (size_t)y * MENU_W;
        for (x = 0; x < MENU_W; x++) {
            d[x] = s[xs[x]];
        }
    }
}

#ifdef HAVE_MI_GFX
static MI_PHY g_mwPhys; /* o menu montado na memoria que o chip 2D le (tela de 752x560: ele amplia) */
static uint32_t* g_mwVirt;
#endif

/* menu de 640x480 (girado) -> pagina da tela: o chip 2D amplia (com suavizacao); sem ele, vizinho mais proximo */
static void menu_to_page(const uint32_t* src, uint32_t* dst, int back) {
    static uint16_t xs[PANEL_MAX_W];
    int x, y;
#ifdef HAVE_MI_GFX
    if (hw_ok && src == g_mwVirt && g_mwVirt != NULL) {
        mi.Flush(g_mwVirt, (MI_U32)(MENU_W * MENU_H * 4));
        if (hw_blit32(g_mwPhys, MENU_W, MENU_H, (MI_PHY)fb_phys + (MI_PHY)back * PANEL_W * PANEL_H * 4, PANEL_W, PANEL_H) == 0) {
            return;
        }
    }
#else
    (void)back;
#endif
    for (x = 0; x < PANEL_W; x++) {
        xs[x] = (uint16_t)(((2 * x + 1) * MENU_W) / (2 * PANEL_W));
    }
    for (y = 0; y < PANEL_H; y++) {
        const uint32_t* s = src + (size_t)(((2 * y + 1) * MENU_H) / (2 * PANEL_H)) * MENU_W;
        uint32_t* d = dst + (size_t)y * PANEL_W;
        for (x = 0; x < PANEL_W; x++) {
            d[x] = s[xs[x]];
        }
    }
}

static void toast_draw(uint32_t* page) {
    battery_draw(page);
    if (g_toastUntil == 0 || miyoo_now_ms() >= g_toastUntil) {
        return;
    }
    miyoo_menu_draw_toast(page, g_toast); /* (menu_miyoo.c: caixa verde com contorno dourado, como o menu) */
}

/* ---------------------------------------------------------------- thread de tela */

/* port/gfx/gfx_hook.c: recebe as imagens dos quadros intermediarios */
extern void (*gx_interp_sink)(const void* fb, const uint16_t* const* img, const int* vs, int n);
/* resolucao maior (port/gfx): a imagem maior de um framebuffer do jogo / o tamanho de uma intermediaria */
extern int gx_hr_image(const void* fbRaw, const uint16_t** img, int* w, int* h);
extern int gx_img_size(const void* img, int* w, int* h);
extern int gx_img_text(const void* img, const uint8_t** buf, int rect[4]);
extern int gx_hr_text_of(const void* fbRaw, const uint8_t** buf, int rect[4]);

/* caixa de texto nitida (gfx_hires.c): regiao de 640x480 (bytes do N64) escrita por cima da tela ja montada,
 * girada 180 como o resto (pixel x,y da imagem vai para 639-x, 479-y do painel) */
static unsigned g_txShown, g_tx640, g_txOther; /* diagnostico: imagens com caixa nitida / em 640 / outras */
static void text_present(const uint8_t* tb, const int r[4], uint32_t* page) {
    extern int gx_hr_le;
    int x, y;
    for (y = r[1]; y < r[3]; y++) {
        const uint16_t* s = (const uint16_t*)(tb + ((size_t)y * 640u + (size_t)r[0]) * 2u);
        uint32_t* d = page + (size_t)(PANEL_H - 1 - y) * PANEL_W + (size_t)(PANEL_W - 1 - r[0]);
        for (x = r[0]; x < r[2]; x++) {
            *d-- = x8888(gx_hr_le ? le_to_565(*s++) : n64_to_565(*s++));
        }
    }
}

static pthread_t flip_thread;
static pthread_mutex_t flip_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cv = PTHREAD_COND_INITIALIZER;
static int flip_started, flip_quit, flip_done;
static const void* pend_fb;   /* quadro novo do VI (NULL = nenhum) */
static int pend_w, pend_h, pend_blank, pend_new;
static int pend_le; /* a imagem do quadro novo e a maior, com a cor na ordem do processador (opcao cor_direta) */
static const uint8_t* pend_tb; /* caixa de texto do quadro novo (NULL = nenhuma) */
static const void* pend_gekey; /* o framebuffer do jogo do quadro novo (as pecas do 2D pelo acelerador vao com ele) */
static int pend_tr[4];
static int clock_on;          /* o vsync e o relogio do VI */
static void (*prev_present)(const void*, const OSViMode*, int);

/* Quadros intermediarios (gfx_hook.c, opcao quadros_extras): para cada quadro do jogo chegam as imagens
 * que devem aparecer no lugar dele, cada uma com o vsync em que entra (contado a partir da troca de quadro
 * do jogo). A thread de tela mostra cada uma no vsync certo. */
static struct {
    const void* fb;
    const uint16_t* img[2];
    int vs[2], n;
} g_ipend[4];
static int g_ipendNext;
static struct {
    const uint16_t* img;
    unsigned due;
} g_isched[8];
static int g_nisched;
static unsigned g_vsync; /* vsyncs contados pela thread de tela */
/* ritmo da tela (log): quantos vsyncs cada imagem ficou na tela (o ideal a 30 fps e 2 sempre) e quantas
 * vezes a imagem adiada passou da hora ainda sendo desenhada */
static unsigned g_rHist[5], g_rLate, g_rLastV;

static void interp_sink(const void* fb, const uint16_t* const* img, const int* vs, int n) {
    int i;
    pthread_mutex_lock(&flip_mx);
    if (n <= 0) {
        /* o quadro foi desenhado sem intermediarios (fora do Play, troca de cena...): as imagens pendentes
         * sao de quadros velhos e nao podem aparecer quando o jogo voltar a usar o mesmo framebuffer */
        for (i = 0; i < 4; i++) {
            g_ipend[i].n = 0;
            g_ipend[i].fb = NULL;
        }
        pthread_mutex_unlock(&flip_mx);
        return;
    }
    g_ipend[g_ipendNext].fb = fb;
    g_ipend[g_ipendNext].n = n > 2 ? 2 : n;
    for (i = 0; i < g_ipend[g_ipendNext].n; i++) {
        g_ipend[g_ipendNext].img[i] = img[i];
        g_ipend[g_ipendNext].vs[i] = vs[i];
    }
    g_ipendNext = (g_ipendNext + 1) & 3;
    pthread_mutex_unlock(&flip_mx);
}

static void hook_vi_present(const void* fb, const OSViMode* mode, int blank) {
    int w = 320, h = 240, k;
    if (prev_present) {
        prev_present(fb, mode, blank);
    }
    if (mode != NULL && mode->comRegs.width >= 64 && mode->comRegs.width <= SRC_MAX_W) {
        w = (int)mode->comRegs.width;
        h = w > 320 ? 480 : 240;
    }
    pthread_mutex_lock(&flip_mx);
    /* o jogo trocou para um quadro que tem imagens intermediarias: agenda elas no lugar dele */
    for (k = 0; k < 4 && !blank && fb != NULL && w == 320; k++) {
        if (g_ipend[k].fb == fb && g_ipend[k].n > 0) {
            int i;
            for (i = 0; i < g_ipend[k].n && g_nisched < 8; i++) {
                g_isched[g_nisched].img = g_ipend[k].img[i];
                g_isched[g_nisched].due = g_vsync + (unsigned)g_ipend[k].vs[i];
                g_nisched++;
            }
            g_ipend[k].n = 0;
            g_ipend[k].fb = NULL;
            pthread_cond_signal(&flip_cv);
            pthread_mutex_unlock(&flip_mx);
            return;
        }
    }
    g_nisched = 0; /* quadro normal: descarta o que estava agendado */
    pend_tb = NULL;
    pend_gekey = blank ? NULL : fb;
    pend_le = 0;
    if (!blank && fb != NULL) {
        const uint16_t* hi;
        int hw, hh;
        if (!gx_hr_text_of(fb, &pend_tb, pend_tr)) {
            pend_tb = NULL;
        }
        if (gx_hr_image(fb, &hi, &hw, &hh) && hw <= SRC_MAX_W && hh <= SRC_MAX_H) {
            extern int gx_hr_le;
            fb = hi; /* desenhado em resolucao maior: mostra essa */
            w = hw;
            h = hh;
            pend_le = gx_hr_le;
        }
    }
    pend_fb = fb;
    pend_w = w;
    pend_h = h;
    pend_blank = blank || fb == NULL;
    pend_new = 1;
    pthread_cond_signal(&flip_cv);
    pthread_mutex_unlock(&flip_mx);
}

/* on = 1: espera a thread de tela parar entre dois quadros (no maximo 300 ms) e a segura ali; on = 0: solta */
static void video_hold(int on) {
    pthread_mutex_lock(&flip_mx);
    g_vhold = on;
    pthread_cond_broadcast(&flip_cv);
    if (on && flip_started && !flip_done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 300 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!g_vidle && !flip_done) {
            if (pthread_cond_timedwait(&flip_cv, &flip_mx, &ts) != 0) {
                fprintf(stderr, "video: a tela nao parou em 300 ms para a copia\n");
                break;
            }
        }
    }
    pthread_mutex_unlock(&flip_mx);
}

static void* flip_proc(void* arg) {
    int buf = 0, frames = 0, fast = 0, ticks = 0, modeRetry = 0;
    double last_tick = 0, last_print = 0, t_conv = 0, t_ccpu = 0, t_show = 0, tmin = 1e9, tmax = 0;
    (void)arg;
    os_thread_class(OS_RT_VIDEO); /* o vsync e o relogio do jogo: pulso na hora */
    pt_thread_name("tela");
    for (;;) {
        const void* fb;
        const void* gekey;
        const uint8_t* tb;
        int tr[4];
        int w, h, blank, job, le = 0;
        double t0, t1, c0;
        pthread_mutex_lock(&flip_mx);
        while (g_vhold && !flip_quit) { /* (video_hold: a copia da tela para o Game Switcher) */
            g_vidle = 1;
            pthread_cond_broadcast(&flip_cv);
            pthread_cond_wait(&flip_cv, &flip_mx);
        }
        g_vidle = 0;
        while (!pend_new && !flip_quit && !g_vhold && !(clock_on && fb_pages == 2)) {
            pthread_cond_wait(&flip_cv, &flip_mx);
        }
        if (g_vhold && !flip_quit) {
            pthread_mutex_unlock(&flip_mx);
            continue; /* (parou esperando: a copia pediu a tela parada) */
        }
        if (flip_quit) {
            flip_done = 1;
            pthread_cond_broadcast(&flip_cv);
            pthread_mutex_unlock(&flip_mx);
            return NULL;
        }
        job = pend_new;
        pend_new = 0;
        fb = pend_fb;
        w = pend_w;
        h = pend_h;
        blank = pend_blank;
        tb = pend_tb;
        gekey = pend_gekey;
        le = pend_le;
        memcpy(tr, pend_tr, sizeof tr);
        {
            /* imagem intermediaria cuja vez chegou (a mais atrasada primeiro). A imagem adiada que ainda
             * esta sendo desenhada (gx_interp_notready) espera: fica na tela a anterior ate ela ficar pronta */
            extern volatile const uint16_t* gx_interp_notready;
            const uint16_t* notReady = (const uint16_t*)__atomic_load_n(&gx_interp_notready, __ATOMIC_ACQUIRE);
            int i, best = -1;
            for (i = 0; i < g_nisched; i++) {
                if (g_isched[i].img == notReady) {
                    if ((int)(g_vsync - g_isched[i].due) >= 0) {
                        g_rLate++; /* a vez dela chegou e ela ainda nao esta pronta */
                    }
                    continue;
                }
                if ((int)(g_vsync - g_isched[i].due) >= 0 && (best < 0 || (int)(g_isched[best].due - g_isched[i].due) > 0)) {
                    best = i;
                }
            }
            if (best >= 0) {
                job = 1;
                fb = g_isched[best].img;
                gekey = fb;
                w = 320;
                h = 240;
                gx_img_size(fb, &w, &h); /* resolucao maior: ate 640x480 */
                {
                    extern int gx_hr_le;
                    le = gx_hr_le; /* (as imagens da fila: sempre na ordem do processador com cor_direta) */
                }
                if (!gx_img_text(fb, &tb, tr)) {
                    tb = NULL;
                }
                blank = 0;
                /* as que ja passaram da hora junto com esta saem tambem (so a mais nova aparece) */
                for (i = 0; i < g_nisched;) {
                    if ((int)(g_vsync - g_isched[i].due) >= 0) {
                        g_isched[i] = g_isched[--g_nisched];
                    } else {
                        i++;
                    }
                }
            }
        }
        if (miyoo_menu_active()) {
            job = 0; /* menu de opcoes aberto (o jogo esta parado): a tela mostra o menu */
            g_nisched = 0;
        } else {
            g_menuShown = 0;
            g_menuThumbOk = 0;
        }
        pthread_mutex_unlock(&flip_mx);

        t0 = miyoo_now_ms();
        {
            struct timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            c0 = (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
        }
        if (job && fb_mem != NULL) {
            pt_ev(PT_FLIP_PRESENT, (int)(g_vsync - g_rLastV));
            int back = fb_pages == 2 ? fb_page ^ 1 : 0;
            uint16_t* cb = conv_buf[buf];
            int isLe = 0, direct = 0;
            {
                /* imagem da fila com a cor na ordem do processador (opcao cor_direta)? */
                isLe = !blank && le && fb != NULL;
            }
#ifdef HAVE_MI_GFX
            if (isLe && hw_ok && img_mma_phys(fb) != 0) {
                /* o chip 2D le a imagem da fila direto (RGBA5551), sem a conversao na CPU */
                cb = (uint16_t*)(uintptr_t)fb;
                direct = 1;
            }
#endif
            if (!direct) {
                buf ^= 1;
            }
            if (blank) {
                memset(cb, 0, (size_t)w * h * 2);
            } else {
                if (!direct) {
                    if (isLe) {
                        convert_le((const uint16_t*)fb, cb, w * h);
                    } else {
                        convert_n64((const uint16_t*)fb, cb, w * h);
                    }
                }
                {
                    /* a ultima coluna: o jogo escurece/pinta a tela com retangulos que param 1 pixel antes da
                     * borda direita (no N64 essa coluna nao aparece direito); repete a vizinha (2 em 640) */
                    const int k = PANEL_W == 640 ? (w >= 640 ? 2 : 1) : (w + 319) / 320; /* (a ultima coluna do N64) */
                    int y, j;
                    for (y = 0; y < h; y++) {
                        uint16_t* row = cb + (size_t)y * w;
                        for (j = 1; j <= k; j++) {
                            row[w - j] = row[w - 1 - k];
                        }
                    }
                }
            }
            {
                int done = 0;
#ifdef HAVE_MI_GFX
                if (hw_ok) {
                    int idx = cb == conv_buf[0] ? 0 : 1;
                    mi.Flush(cb, (MI_U32)(w * h * 2));
                    if ((direct ? hw_blit_fmt(img_mma_phys(fb), E_MI_GFX_FMT_RGBA5551, w, h,
                                              (MI_PHY)fb_phys + (MI_PHY)back * PANEL_W * PANEL_H * 4, PANEL_W, PANEL_H,
                                              hw_rotate, hw_mirror)
                                : hw_blit(hw_phys[idx], w, h, (MI_PHY)fb_phys + (MI_PHY)back * PANEL_W * PANEL_H * 4,
                                          PANEL_W, PANEL_H, hw_rotate, hw_mirror)) == 0) {
                        g_direct += direct;
                        g_directNo += !direct && !blank;
                        done = 1;
                        if (!blank) {
                            pt_ev(PT_FLIP_GE, 0);
                            ge_draw(gekey, back, w, h); /* o HUD / logo pelo acelerador, por cima */
                            pt_ev(PT_FLIP_GE + 1, 0);
                            rt_draw(back); /* filtro retro (opcao retro_filter), por cima de tudo do jogo */
                        }
                    } else {
                        fprintf(stderr, "video: MI_GFX falhou, voltando para a copia pela CPU\n");
                        hw_ok = 0;
                        gx_ge_mem = NULL; /* (o 2D volta para os nucleos nos proximos desenhos) */
                    }
                }
#endif
                if (!done) {
                    cpu_present(cb, w, h, fb_page_ptr(back));
                }
                if (tb != NULL && !blank && PANEL_W == 640) {
                    text_present(tb, tr, fb_page_ptr(back)); /* a caixa de texto em 640 por cima */
                    g_txShown++;
                } else if (w >= 640) {
                    g_tx640++;
                } else {
                    g_txOther++;
                }
            }
            t1 = miyoo_now_ms();
            t_conv += t1 - t0;
            {
                /* quanto da copia e processador (o resto e a espera pelo chip 2D) */
                struct timespec ts;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                t_ccpu += (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6 - c0;
            }
            toast_draw(fb_page_ptr(back)); /* icone de bateria fraca e aviso do estado rapido, por cima de tudo */
            pt_ev(PT_FLIP_PRESENT + 1, 0);
            if (fb_pages == 2) {
                fb_var.yoffset = (unsigned)(back * PANEL_H);
                pt_ev(PT_FLIP_VSYNC, 1);
                ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var); /* espera o vsync */
                pt_ev(PT_FLIP_VSYNC + 1, 1);
                fb_page = back;
            }
            t_show += miyoo_now_ms() - t1;
            frames++;
            {
                extern void gfx_trace(char k, int i0, int i1, float a, float b, float c, float d, float e);
                static unsigned lateSeen;
                const unsigned gap = g_vsync - g_rLastV;
                g_rHist[gap >= 4 ? 4 : gap]++;
                g_rLastV = g_vsync;
                gfx_trace('V', (int)gap, (int)(g_rLate >= lateSeen ? g_rLate - lateSeen : g_rLate), (float)(t1 - t0), 0, 0, 0, 0);
                lateSeen = g_rLate;
            }
        } else if (fb_pages == 2 && clock_on && fb_mem != NULL && miyoo_menu_active() && (!g_menuShown || miyoo_menu_dirty())) {
            /* menu de opcoes (menu_miyoo.c): o fundo e a ultima imagem do jogo, escurecida, guardada ao abrir */
            const int back = fb_page ^ 1;
            const size_t n = (size_t)MENU_W * MENU_H; /* (o menu e o fundo dele: 640x480) */
            const int same = PANEL_W == MENU_W && PANEL_H == MENU_H;
            if (!g_menuShown) {
                const uint32_t* front = fb_page_ptr(fb_page);
                size_t i;
                if (g_menuBg == NULL) {
                    g_menuBg = (uint32_t*)malloc(n * 4);
                    g_menuSrc = (uint32_t*)malloc(n * 4);
                    if (g_menuSrc == NULL) {
                        free(g_menuBg);
                        g_menuBg = NULL;
                    }
                }
                {
                    /* miniatura 256x192 da imagem do jogo (estados do menu): cada ponto e a media do retangulo de
                     * 2 ou 3 pixels de lado que ele cobre (640/256 = 2,5), ja na orientacao certa */
                    int tx, ty;
                    if (g_menuThumb == NULL) {
                        g_menuThumb = (uint16_t*)malloc(MTHUMB_W * MTHUMB_H * 2);
                    }
                    for (ty = 0; g_menuThumb != NULL && ty < MTHUMB_H; ty++) {
                        const int y0 = ty * PANEL_H / MTHUMB_H, y1 = (ty + 1) * PANEL_H / MTHUMB_H;
                        for (tx = 0; tx < MTHUMB_W; tx++) {
                            const int x0 = tx * PANEL_W / MTHUMB_W, x1 = (tx + 1) * PANEL_W / MTHUMB_W;
                            unsigned r = 0, g = 0, b = 0, k = 0;
                            int yy, xx;
                            for (yy = y0; yy < y1; yy++) {
                                const uint32_t* row = front + (size_t)(PANEL_H - 1 - yy) * PANEL_W;
                                for (xx = x0; xx < x1; xx++) {
                                    const uint32_t c = row[PANEL_W - 1 - xx];
                                    r += (c >> 16) & 255;
                                    g += (c >> 8) & 255;
                                    b += c & 255;
                                    k++;
                                }
                            }
                            r /= k;
                            g /= k;
                            b /= k;
                            g_menuThumb[ty * MTHUMB_W + tx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                    }
                    g_menuThumbOk = g_menuThumb != NULL;
                }
                if (g_menuBg != NULL) {
                    extern int gx_brilho;
                    if (same) {
                        memcpy(g_menuSrc, front, n * 4);
                    } else {
                        page_to_menu(front, g_menuSrc);
                    }
                    g_menuSrcLv = gx_brilho < 0 ? 0 : gx_brilho > 10 ? 10 : gx_brilho;
                    for (i = 0; i < n; i++) {
                        g_menuBg[i] = (g_menuSrc[i] >> 2) & 0x3F3F3Fu; /* 1/4 do brilho */
                    }
                    g_menuBgLv = g_menuSrcLv;
                }
                g_menuShown = 1;
            }
            {
                /* o menu e desenhado numa copia na memoria comum (os paineis semitransparentes leem o fundo; ler a
                 * memoria da tela e lento) e vai inteiro para a pagina de tras */
                static uint32_t* s_work;
                uint32_t* dst = fb_page_ptr(back);
                if (s_work == NULL) {
#ifdef HAVE_MI_GFX
                    if (!same && hw_ok && g_mwVirt == NULL) {
                        void* v = NULL;
                        if (mi.MmaAlloc(NULL, (MI_U32)(n * 4), &g_mwPhys) == 0) {
                            if (mi.Mmap(g_mwPhys, (MI_U32)(n * 4), &v, TRUE) == 0) {
                                g_mwVirt = (uint32_t*)v;
                            } else {
                                mi.MmaFree(g_mwPhys);
                                g_mwPhys = 0;
                            }
                        }
                    }
                    s_work = g_mwVirt;
#endif
                    if (s_work == NULL) {
                        s_work = (uint32_t*)malloc(n * 4);
                    }
                }
                if (s_work == NULL && !same) {
                    goto menu_skip; /* (sem memoria: a tela fica como esta) */
                }
                if (s_work != NULL) {
                    dst = s_work;
                }
                if (g_menuBg != NULL) {
                    extern int gx_brilho;
                    if ((gx_brilho < 0 ? 0 : gx_brilho > 10 ? 10 : gx_brilho) != g_menuBgLv) {
                        menu_bg_build(); /* o brilho mudou no menu: o fundo acompanha */
                    }
                    memcpy(dst, g_menuBg, n * 4);
                } else {
                    memset(dst, 0, n * 4);
                }
                miyoo_menu_render(dst);
                if (same) {
                    battery_draw(dst);
                    if (dst != fb_page_ptr(back)) {
                        memcpy(fb_page_ptr(back), dst, n * 4);
                    }
                } else {
                    menu_to_page(dst, fb_page_ptr(back), back);
                    battery_draw(fb_page_ptr(back));
                }
            }
            fb_var.yoffset = (unsigned)(back * PANEL_H);
            ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var); /* espera o vsync */
            fb_page = back;
        menu_skip:;
        } else if (fb_pages == 2 && clock_on) {
            fb_var.yoffset = (unsigned)(fb_page * PANEL_H); /* mesma pagina: so espera o vsync */
            pt_ev(PT_FLIP_VSYNC, 0);
            ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
            pt_ev(PT_FLIP_VSYNC + 1, 0);
        }

        if (clock_on && fb_pages == 2) {
            double now = miyoo_now_ms();
            int missed = 0;
            if (last_tick != 0 && now - last_tick < 8.0) {
                /* a troca de pagina nao esperou o vsync: isto nao e um relogio */
                if (++fast > 30 && !modeRetry) {
                    /* 1a vez: poe o modo da tela de novo e mede outra vez */
                    modeRetry = 1;
                    fast = 0;
                    if (fb_set_mode("a troca de pagina nao esperou o vsync") == 0) {
                        ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var);
                        fb_page = fb_var.yoffset >= (unsigned)PANEL_H ? 1 : 0;
                    }
                    continue;
                }
                if (fast > 30) {
                    fprintf(stderr, "video: a troca de pagina nao espera o vsync; usando relogio de 60 Hz\n");
                    gx_interp_sink = NULL; /* sem vsync nao da para mostrar os intermediarios na hora certa */
                    clock_on = 0;
                    os_hw_vi_set_external(0);
                }
                continue;
            }
            fast = 0;
            if (last_tick != 0) {
                double d = now - last_tick;
                if (d < tmin) {
                    tmin = d;
                }
                if (d > tmax) {
                    tmax = d;
                }
                /* uma volta que passou de um vsync (quadro demorado) entrega os pulsos que faltaram */
                missed = (int)(d / 16.75 + 0.5) - 1;
                if (missed > 3) {
                    missed = 3;
                }
            }
            last_tick = now;
            pthread_mutex_lock(&flip_mx);
            g_vsync += (unsigned)missed + 1u;
            pthread_mutex_unlock(&flip_mx);
            for (; missed > 0; missed--) {
                os_hw_vi_pulse();
                ticks++;
            }
            os_hw_vi_pulse();
            ticks++;
        }

        if (miyoo_opt_perf) {
            double now = miyoo_now_ms();
            if (last_print == 0) {
                last_print = now;
            } else if (now - last_print >= 2000.0) {
                fprintf(stderr, "perf tela: %d quadros em %.1f s (%.1f fps), copia %.2f ms (processador %.2f), espera do vsync %.2f ms | "
                                "%d pulsos, %.2f..%.2f ms | ritmo (vsyncs na tela): 1: %u, 2: %u, 3: %u, 4+: %u, "
                                "adiada atrasada %u\n",
                        frames, (now - last_print) / 1000.0, frames * 1000.0 / (now - last_print),
                        frames ? t_conv / frames : 0, frames ? t_ccpu / frames : 0, frames ? t_show / frames : 0, ticks, ticks ? tmin : 0, tmax,
                        g_rHist[1], g_rHist[2], g_rHist[3], g_rHist[4], g_rLate);
                if (g_txShown) {
                    fprintf(stderr, "perf tela: imagens com caixa de texto nitida %u, imagem toda em 640 %u, outras (sem caixa nitida) %u\n",
                            g_txShown, g_tx640, g_txOther);
                }
                g_txShown = g_tx640 = g_txOther = 0;
#ifdef HAVE_MI_GFX
                if (g_geShown) {
                    fprintf(stderr, "perf tela: 2D pelo acelerador em %u imagens, %.1f pecas por imagem\n", g_geShown,
                            (double)g_geItems / g_geShown);
                }
                g_geShown = g_geItems = 0;
                if (g_direct + g_directNo) {
                    fprintf(stderr, "perf tela: imagens lidas direto pelo chip 2D (sem converter na CPU) %u, convertidas %u\n",
                            g_direct, g_directNo);
                }
                g_direct = g_directNo = 0;
                if (g_bgN + g_bgNo) {
                    fprintf(stderr, "perf tela: fundos JPEG pelo chip 2D %u, pela CPU (nao deu) %u\n", g_bgN, g_bgNo);
                    g_bgN = g_bgNo = 0;
                }
#endif
                memset(g_rHist, 0, sizeof g_rHist);
                g_rLate = 0;
                frames = ticks = 0;
                t_conv = t_ccpu = t_show = 0;
                tmin = 1e9;
                tmax = 0;
                last_print = now;
            }
        }
    }
    return NULL;
}

static int video_inited;

void miyoo_video_init(void) {
    if (video_inited) {
        return;
    }
    video_inited = 1;
    fb_init();
    {
        extern void gx_hr_set_panel(int w, int h); /* gfx_hires.c: desenho no tamanho da tela */
        gx_hr_set_panel(PANEL_W, PANEL_H);
    }
    hw_ok = hw_init();
    if (!hw_ok) {
        int i;
        for (i = 0; i < 2; i++) {
            conv_buf[i] = calloc((size_t)SRC_MAX_W * SRC_MAX_H, 2);
        }
    }
}

void miyoo_video_start(void) {
    miyoo_video_init();
    prev_present = port_hooks.vi_present;
    port_hooks.vi_present = hook_vi_present;
    if (fb_pages == 2) {
        gx_interp_sink = interp_sink; /* quadros intermediarios so com o vsync como relogio */
        clock_on = 1;
        os_hw_vi_set_external(1); /* antes do osCreateViManager (bootproc) */
        fprintf(stderr, "video: o vsync do painel e o relogio do jogo\n");
    }
    if (pthread_create(&flip_thread, NULL, flip_proc, NULL) == 0) {
        flip_started = 1;
    } else {
        fprintf(stderr, "video: nao consegui criar a thread de tela\n");
    }
}

/* A memoria MMA (a que o chip 2D le) nao volta sozinha para o sistema: no reinicio pelo menu de opcoes (o programa
 * se troca por ele mesmo, execv) ela continuava presa ao processo, e no Game Switcher o mesmo. O jogo aberto de novo
 * achava a memoria quase cheia: as imagens da fila iam para a memoria comum (convertidas pela CPU: so 10 de 60 lidas
 * direto pelo chip, log de 03/11), o 2D ficava com 512 KB, e o desenho perdia tempo (resolucao menor). Ao fechar ou
 * reiniciar, tudo volta: imagens da fila, 2D, fundos JPEG e as copias para a tela. (Chamado com o desenho parado:
 * fechar e reiniciar acontecem no ponto seguro entre quadros, savestate.c.) */
static void release_mma(void) {
#ifdef HAVE_MI_GFX
    int i;
    unsigned n = 0, kb = 0;
    if (!hw_ok) {
        return;
    }
    pthread_mutex_lock(&g_blitMx);
    mi.Wait(TRUE, 0);
    for (i = 0; i < g_nImgMma; i++) {
        mi.Munmap(g_imgMma[i].v, (MI_U32)g_imgMma[i].n);
        mi.MmaFree(g_imgMma[i].p);
        kb += (unsigned)(g_imgMma[i].n >> 10);
        n++;
    }
    g_nImgMma = 0;
    for (i = 0; i < GX_GE_PARTS; i++) {
        if (ge_virt[i] != NULL) {
            mi.Munmap(ge_virt[i], ge_cap);
            mi.MmaFree(ge_phys[i]);
            ge_virt[i] = NULL;
            kb += ge_cap >> 10;
            n++;
        }
    }
    ge_ok = 0;
    if (rt_virt != NULL) {
        mi.Munmap(rt_virt, (MI_U32)(PANEL_W * PANEL_H * 4));
        mi.MmaFree(rt_phys);
        rt_virt = NULL;
        rt_phys = 0;
        rt_mode = -1;
        kb += (PANEL_W * PANEL_H * 4) >> 10;
        n++;
    }
    if (rs_full != NULL) {
        mi.Munmap(rs_full, (MI_U32)(PANEL_W * PANEL_H * 4));
        mi.MmaFree(rs_fullp);
        rs_full = NULL;
        rs_fullp = 0;
        rt_mode = -1;
        kb += (PANEL_W * PANEL_H * 4) >> 10;
        n++;
    }
    if (rs_virt != NULL) {
        mi.Munmap(rs_virt, (MI_U32)(PANEL_W * RT_STRIP * 4));
        mi.MmaFree(rs_phys);
        rs_virt = NULL;
        rs_phys = 0;
        rt_strip = 0;
        kb += (PANEL_W * RT_STRIP * 4) >> 10;
        n++;
    }
    if (g_mwVirt != NULL) {
        /* (so no fim do programa: o menu nao abre mais depois disto) */
        mi.Munmap(g_mwVirt, (MI_U32)(MENU_W * MENU_H * 4));
        mi.MmaFree(g_mwPhys);
        g_mwVirt = NULL;
        g_mwPhys = 0;
        kb += (MENU_W * MENU_H * 4) >> 10;
        n++;
    }
    if (g_bgVirt != NULL) {
        mi.Munmap(g_bgVirt, (MI_U32)g_bgCap);
        mi.MmaFree(g_bgPhys);
        g_bgVirt = NULL;
        kb += (unsigned)(g_bgCap >> 10);
        g_bgCap = 0;
        n++;
    }
    for (i = 0; i < 2; i++) {
        if (conv_buf[i] != NULL && hw_phys[i]) {
            mi.Munmap(conv_buf[i], (MI_U32)(SRC_MAX_W * SRC_MAX_H * 2));
            mi.MmaFree(hw_phys[i]);
            conv_buf[i] = NULL;
            hw_phys[i] = 0;
            kb += (SRC_MAX_W * SRC_MAX_H * 2) >> 10;
            n++;
        }
    }
    hw_ok = 0;
    mi.GfxClose();
    pthread_mutex_unlock(&g_blitMx);
    fprintf(stderr, "video: memoria MMA devolvida (%u pedacos, %u KB)\n", n, kb);
#endif
}

void miyoo_video_shutdown(void) {
    if (flip_started) {
        struct timespec ts;
        pthread_mutex_lock(&flip_mx);
        flip_quit = 1;
        pthread_cond_broadcast(&flip_cv);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!flip_done) {
            if (pthread_cond_timedwait(&flip_cv, &flip_mx, &ts) != 0) {
                break;
            }
        }
        pthread_mutex_unlock(&flip_mx);
    }
    if (fb_mem != NULL) {
        memset(fb_mem, 0, fb_pages == 2 ? (size_t)PANEL_W * PANEL_H * 8 : (size_t)PANEL_W * PANEL_H * 4);
        if (fb_pages == 2) {
            fb_var.yoffset = 0;
            ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
        }
    }
    release_mma();
}
