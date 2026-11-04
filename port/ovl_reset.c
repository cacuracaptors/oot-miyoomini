/* ovl_reset.c -- recarga dos overlays como no N64.
 *
 * No N64 o codigo de cada ator, efeito soft sprite, tela (gamestate) e do menu de pausa / do Link vem da ROM
 * toda vez que e carregado (Overlay_Load): as variaveis do arquivo (.data) voltam aos valores iniciais e as
 * zeradas (.bss) voltam a zero. O jogo conta com isso. Exemplos:
 *  - o Zora do jogo de mergulho (Dominio Zora, Escama de Prata) guarda numa variavel do arquivo que ja esta na
 *    cena; sem a recarga a variavel ficava ligada e ele sumia em toda visita depois da 1a (o mesmo defeito
 *    relatado no port do miyoo64 do mr-finster, corrigido la do mesmo jeito: hh_ovl.c);
 *  - o desabamento da Torre de Ganon (volta ao comeco da fuga so na 1a vez), efeitos do Arwing etc.
 * No port tudo esta ligado no executavel. tools/oot-ovl.py marca os dados de cada overlay (gen/port_ovl.c);
 * aqui eles sao copiados antes do jogo comecar e postos de volta (com o .bss zerado) onde o N64 recarregaria:
 *   atores      a 1a instancia depois que o overlay foi liberado (nenhuma instancia sobrando, a nao ser
 *               ACTOROVL_ALLOC_PERSISTENT) ou depois de uma cena nova (Actor_InitContext);
 *   efeitos     o 1o efeito depois de EffectSs_InitInfo / EffectSs_ClearAll;
 *   kaleido     cada KaleidoManager_LoadOvl (menu de pausa e Link trocam de lugar na mesma area);
 *   telas       cada tela que comeca (titulo, abertura, selecao de arquivos).
 * As marcas "carregado" e as contagens sao estado do jogo: ficam em game_data (vao no estado rapido).
 * Opcao reload_overlays (options.txt) = 0 desliga (para comparar). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *ds, *de, *bs, *be;
} PortOvlRange;

/* gen/port_ovl.c (tools/oot-ovl.py) */
extern const PortOvlRange port_ovl_ranges[];
extern const int port_ovl_nranges;
extern const unsigned short port_ovl_actors[][2], port_ovl_effects[][2], port_ovl_states[][2], port_ovl_kaleido[][2];
extern const int port_ovl_actors_n, port_ovl_effects_n, port_ovl_states_n, port_ovl_kaleido_n;

int port_opt_reload_overlays = 1;

#define MAX_ACTORS 512
#define MAX_EFFECTS 64
#define GAME_DATA __attribute__((section("game_data")))
static uint16_t sActorCount[MAX_ACTORS] GAME_DATA;
static uint8_t sActorLoaded[MAX_ACTORS] GAME_DATA;
static uint8_t sEffectLoaded[MAX_EFFECTS] GAME_DATA;

/* variaveis do port que moravam no z_player.c e tem de sobreviver a recarga do Link (troca de cena, pausa):
 * a mascara lembrada (persistent_masks) */
int32_t Port_MaskMemory GAME_DATA;

static uint8_t** g_snap;
static int g_on;
static unsigned g_nReset;

/* antes do jogo comecar (port_main.c, logo antes do bootproc): os dados de todos os overlays com os valores
 * iniciais (e o que o port ajustou neles ao abrir) */
void Port_OvlSnapshot(void) {
    size_t bytes = 0, bss = 0;
    int i;
    g_on = port_opt_reload_overlays != 0 && port_ovl_nranges > 0;
    if (port_ovl_actors_n > MAX_ACTORS || port_ovl_effects_n > MAX_EFFECTS) {
        fprintf(stderr, "[overlays] tabelas maiores que o previsto (%d atores, %d efeitos): recarga desligada\n",
                port_ovl_actors_n, port_ovl_effects_n);
        g_on = 0;
    }
    if (!g_on) {
        fprintf(stderr, "[overlays] recarga desligada (reload_overlays = 0)\n");
        return;
    }
    g_snap = (uint8_t**)calloc((size_t)port_ovl_nranges, sizeof *g_snap);
    if (g_snap == NULL) {
        g_on = 0;
        return;
    }
    for (i = 0; i < port_ovl_nranges; i++) {
        const PortOvlRange* r = &port_ovl_ranges[i];
        if (r->ds != NULL && r->de > r->ds) {
            size_t n = (size_t)(r->de - r->ds);
            g_snap[i] = (uint8_t*)malloc(n);
            if (g_snap[i] != NULL) {
                memcpy(g_snap[i], r->ds, n);
                bytes += n;
            }
        }
        if (r->bs != NULL && r->be > r->bs) {
            bss += (size_t)(r->be - r->bs);
        }
    }
    fprintf(stderr, "[overlays] recarga ligada: %d trechos, .data %u KB copiados, .bss %u KB\n", port_ovl_nranges,
            (unsigned)(bytes >> 10), (unsigned)(bss >> 10));
}

static void reset_ovl(const unsigned short e[2]) {
    int i;
    if (!g_on) {
        return;
    }
    for (i = e[0]; i < e[0] + e[1] && i < port_ovl_nranges; i++) {
        const PortOvlRange* r = &port_ovl_ranges[i];
        if (g_snap[i] != NULL) {
            memcpy(r->ds, g_snap[i], (size_t)(r->de - r->ds));
        }
        if (r->bs != NULL && r->be > r->bs) {
            memset(r->bs, 0, (size_t)(r->be - r->bs));
        }
    }
    if (e[1] > 0) {
        g_nReset++;
    }
}

/* Actor_Spawn, antes de ler o perfil do ator: o overlay como o Overlay_Load deixa, se nao estava carregado */
void Port_OvlActorLoad(int id) {
    if (id < 0 || id >= port_ovl_actors_n || id >= MAX_ACTORS || sActorLoaded[id]) {
        return;
    }
    reset_ovl(port_ovl_actors[id]);
    sActorLoaded[id] = 1;
}

/* Actor_Spawn deu certo: mais uma instancia (no port o numLoaded do jogo so sobe: todo ator e "nao overlay") */
void Port_OvlActorRef(int id) {
    if (id >= 0 && id < MAX_ACTORS && sActorCount[id] < 0xFFFF) {
        sActorCount[id]++;
    }
}

/* Actor_Delete (drop 1: uma instancia a menos) e Actor_Spawn que falhou (drop 0): sem instancias o N64 libera o
 * overlay, a nao ser ACTOROVL_ALLOC_PERSISTENT (fica ate a cena acabar) */
void Port_OvlActorRelease(int id, int persistent, int drop) {
    if (id < 0 || id >= MAX_ACTORS) {
        return;
    }
    if (drop && sActorCount[id] > 0) {
        sActorCount[id]--;
    }
    if (sActorCount[id] == 0 && !persistent) {
        sActorLoaded[id] = 0;
    }
}

/* Actor_InitContext: cena nova, todos os overlays de atores sao carregados de novo */
void Port_OvlActorFreeAll(void) {
    memset(sActorLoaded, 0, sizeof sActorLoaded);
    memset(sActorCount, 0, sizeof sActorCount);
}

void Port_OvlEffectLoad(int type) {
    if (type < 0 || type >= port_ovl_effects_n || type >= MAX_EFFECTS || sEffectLoaded[type]) {
        return;
    }
    reset_ovl(port_ovl_effects[type]);
    sEffectLoaded[type] = 1;
}

void Port_OvlEffectFreeAll(void) {
    memset(sEffectLoaded, 0, sizeof sEffectLoaded);
}

/* KaleidoManager_LoadOvl: 0 = menu de pausa, 1 = Link */
void Port_OvlKaleidoLoad(int index) {
    if (index >= 0 && index < port_ovl_kaleido_n) {
        reset_ovl(port_ovl_kaleido[index]);
    }
}

/* Graph_ThreadEntry, tela nova (indice de gGameStateOverlayTable) */
void Port_OvlGameStateLoad(int index) {
    if (index >= 0 && index < port_ovl_states_n) {
        reset_ovl(port_ovl_states[index]);
    }
}

unsigned Port_OvlResets(void) {
    return g_nReset;
}
