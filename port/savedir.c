/*
 * savedir.c -- onde ficam os saves: na pasta do perfil do OnionOS, ou na pasta do jogo.
 *
 * No OnionOS os saves (oot.sav, os estados, as miniaturas deles e os avisos de retomada) ficam em
 *   /mnt/SDCARD/Saves/CurrentProfile/saves/ZeldaOoT/
 * junto dos saves do RetroArch, entao quem troca o perfil da a cada jogador os seus: o Modo Visitante do OnionOS troca a
 * pasta CurrentProfile inteira, e apps como o Kids Mode trocam a pasta saves/ dela. Fora do OnionOS (ou se essa pasta nao
 * puder ser criada) o jogo continua usando ./saves, como antes.
 *
 * MIGRACAO: as versoes anteriores guardavam tudo em ./saves (a pasta do jogo). Na primeira vez que um perfil joga, cada
 * arquivo de ./saves e copiado para a pasta desse perfil; um arquivo que ja esteja la nunca e sobrescrito, e ./saves fica
 * como estava (um backup, e o que uma versao anterior do jogo ainda le). Um perfil ja copiado tem MIGRATED.txt; apagar
 * esse arquivo copia de novo na proxima vez (ainda sem sobrescrever). Se a copia falhar (cartao cheio...), esta vez usa
 * ./saves e a copia e tentada de novo na proxima.
 */
#include "savedir.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ONION_DIR "/mnt/SDCARD/.tmp_update"
#define PROFILE_SAVES "/mnt/SDCARD/Saves/CurrentProfile/saves"
#define GAME_SUBDIR "ZeldaOoT"
#define LEGACY_DIR "saves"
#define MIGRATED_NOTE "MIGRATED.txt" /* na pasta do perfil: este perfil ja foi copiado */
#define MOVED_NOTE "MOVED.txt"       /* em ./saves: para onde os saves foram */

static char g_saveDir[PORT_PATH_MAX];

static int is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char* path) {
    return access(path, F_OK) == 0;
}

static int ends_with(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

/* o que nao e copiado de ./saves: os avisos de cada sessao (uma retomada pendente num perfil nao pode comecar uma
 * retomada em outro), arquivos pela metade e as proprias notas */
static int skip_on_copy(const char* name) {
    return name[0] == '.' || !strcmp(name, "resume_boot") || !strcmp(name, "state_loading") ||
           ends_with(name, ".tmp") || !strcmp(name, MIGRATED_NOTE) || !strcmp(name, MOVED_NOTE);
}

static int write_text(const char* path, const char* text) {
    size_t len = strlen(text);
    int ok;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return 0;
    }
    ok = write(fd, text, len) == (ssize_t)len;
    fsync(fd);
    close(fd);
    return ok;
}

/* src -> dst.tmp, depois renomeado: dst nunca fica pela metade */
static int copy_file(const char* src, const char* dst) {
    static char buf[64 * 1024];
    char tmp[PORT_PATH_MAX + 8];
    int in, out, ok = 1;
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    in = open(src, O_RDONLY);
    if (in < 0) {
        return 0;
    }
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return 0;
    }
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf), done = 0;
        if (r < 0) {
            ok = 0;
        }
        if (r <= 0) {
            break;
        }
        while (done < r) {
            ssize_t w = write(out, buf + done, (size_t)(r - done));
            if (w <= 0) {
                ok = 0;
                break;
            }
            done += w;
        }
        if (!ok) {
            break;
        }
    }
    close(in);
    if (fsync(out) != 0) {
        ok = 0;
    }
    if (close(out) != 0) {
        ok = 0;
    }
    if (ok && rename(tmp, dst) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(tmp);
    }
    return ok;
}

/* a primeira vez que este perfil joga: ./saves copiado para ele. 1 = a pasta pode ser usada */
static int seed_from_legacy(const char* dir) {
    char note[PORT_PATH_MAX], moved[PORT_PATH_MAX];
    int copied = 0, kept = 0, failed = 0;
    struct dirent* e;
    DIR* d;
    snprintf(note, sizeof note, "%s/" MIGRATED_NOTE, dir);
    if (exists(note)) {
        return 1;
    }
    d = opendir(LEGACY_DIR);
    if (d == NULL) {
        return 1; /* nada de uma versao anterior: comeca do zero */
    }
    while ((e = readdir(d)) != NULL) {
        char src[PORT_PATH_MAX], dst[PORT_PATH_MAX];
        struct stat st;
        if (skip_on_copy(e->d_name)) {
            continue;
        }
        if (snprintf(src, sizeof src, LEGACY_DIR "/%s", e->d_name) >= (int)sizeof src ||
            snprintf(dst, sizeof dst, "%s/%s", dir, e->d_name) >= (int)sizeof dst) {
            failed++;
            continue;
        }
        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (exists(dst)) {
            kept++; /* nunca sobrescreve o que este perfil ja tem */
            continue;
        }
        if (copy_file(src, dst)) {
            copied++;
        } else {
            failed++;
        }
    }
    closedir(d);

    fprintf(stderr, "[saves] copiados de ./" LEGACY_DIR " para %s: %d (%d ja estavam la, %d falharam)\n", dir, copied,
            kept, failed);
    if (failed) {
        return 0;
    }
    if (copied + kept == 0) {
        return 1; /* ./saves vazia: nada a anotar */
    }
    write_text(note, "Ocarina of Time's saves were copied here from the game's own folder\n"
                     "(Roms/PORTS/Games/ZeldaOoT/saves) the first time this profile played.\n"
                     "\n"
                     "Delete this file to copy them again (files already here are never overwritten).\n");
    snprintf(moved, sizeof moved, LEGACY_DIR "/" MOVED_NOTE);
    if (!exists(moved)) {
        write_text(moved, "Ocarina of Time now keeps its saves per OnionOS profile, in\n"
                          "Saves/CurrentProfile/saves/ZeldaOoT on the SD card.\n"
                          "\n"
                          "This folder is left as it was: a backup, copied once into each profile\n"
                          "the first time that profile plays.\n");
    }
    sync();
    return 1;
}

static void resolve(void) {
    char dir[PORT_PATH_MAX];
    snprintf(g_saveDir, sizeof g_saveDir, LEGACY_DIR);
    if (!is_dir(ONION_DIR)) {
        mkdir(LEGACY_DIR, 0777);
        fprintf(stderr, "[saves] fora do OnionOS: ./" LEGACY_DIR "\n");
        return;
    }
    snprintf(dir, sizeof dir, PROFILE_SAVES "/" GAME_SUBDIR);
    mkdir(PROFILE_SAVES, 0777); /* (a CurrentProfile e do OnionOS: nao e criada aqui) */
    mkdir(dir, 0777);
    if (!is_dir(dir)) {
        mkdir(LEGACY_DIR, 0777);
        fprintf(stderr, "[saves] nao consegui criar %s: usando ./" LEGACY_DIR "\n", dir);
        return;
    }
    if (!seed_from_legacy(dir)) {
        mkdir(LEGACY_DIR, 0777);
        fprintf(stderr, "[saves] a copia nao terminou: usando ./" LEGACY_DIR " desta vez\n");
        return;
    }
    snprintf(g_saveDir, sizeof g_saveDir, "%s", dir);
    fprintf(stderr, "[saves] %s\n", g_saveDir);
}

const char* port_save_dir(void) {
    if (g_saveDir[0] == 0) {
        resolve();
    }
    return g_saveDir;
}

int port_save_path(char* out, size_t size, const char* name) {
    return snprintf(out, size, "%s/%s", port_save_dir(), name) < (int)size;
}
