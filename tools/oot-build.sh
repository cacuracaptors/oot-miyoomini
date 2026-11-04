#!/bin/sh
# Compila o jogo (decomp) + a camada de plataforma para ARM e LIGA tudo.
# Escreve um relatorio do que falta / do que esta duplicado.
#   uso: sh oot-build.sh [relatorio.txt]
#
# Passos:
#   1. oot-patch.py   cria a "arvore do port" (copia dos includes + 4 arquivos ajustados)
#   2. compila so os arquivos que o proprio decomp usou no build da ROM ntsc-1.1
#      (menos os que a camada de plataforma substitui)
#   3. compila a camada de plataforma (platform/*.c)
#   4. 1a ligacao -> oot-genld.py gera o que falta -> 2a ligacao
ROOT=${ROOT:-$HOME/fallout-miyoo}
OOT=${OOT:-$ROOT/oot}
PORT=${PORT:-$ROOT/oot-miyoo}
# HOST32=1: compila para PC de 32 bits (i386) em vez de ARM, para testar o jogo no WSL
HOST32=${HOST32:-0}
if [ "$HOST32" = 1 ]; then
    OUT=${OUT:-$ROOT/oot-i386}
    CC=${CC:-gcc -m32 -msse2 -mfpmath=sse}
    NM=${NM:-nm}
    OBJCOPY=${OBJCOPY:-objcopy}
    REPORT_DEFAULT=/mnt/c/Users/Régis/Desktop/oot_build32.txt
else
    TC=${TC:-$ROOT/miyoomini-toolchain/bin/arm-linux-gnueabihf}
    OUT=${OUT:-$ROOT/oot-arm}
    CC=${CC:-$TC-gcc}
    NM=${NM:-$TC-nm}
    OBJCOPY=${OBJCOPY:-$TC-objcopy}
    REPORT_DEFAULT=/mnt/c/Users/Régis/Desktop/oot_linkcheck.txt
    # nunca usar o compilador ARM do Ubuntu (glibc nova demais: o jogo nao abre no aparelho)
    case "$(command -v ${CC%% *} 2>/dev/null)" in
        /usr/bin/*|/bin/*)
            echo "ERRO: $CC e o compilador ARM do Ubuntu. Use a toolchain do Miyoo Mini em"
            echo "      $ROOT/miyoomini-toolchain (copiada do Docker), nunca a do Ubuntu."
            exit 1 ;;
    esac
    if [ ! -x "${CC%% *}" ] && ! command -v "${CC%% *}" > /dev/null 2>&1; then
        echo "ERRO: nao achei o compilador ARM (${CC%% *}). Confira a pasta $ROOT/miyoomini-toolchain."
        exit 1
    fi
fi
SYMDIR=${SYMDIR:-$ROOT/oot-arm}   # onde esta o oot-syms.ld (igual para ARM e i386)
REPORT=${1:-$REPORT_DEFAULT}
TREE=$OUT/tree
OBJ=$OUT/obj3
LOG=$OUT/log3
GEN=$OUT/gen
PLAT=$OUT/platform_obj
BUILD=$OOT/build/ntsc-1.1

if [ "$1" = "--one" ]; then
    # compila UM arquivo do jogo:  oot-build.sh --one src/x.c
    cd "$OOT" || exit 1
    f=$2
    src=$f
    [ -f "$TREE/$f" ] && src="$TREE/$f"   # versao ajustada, se existir
    o=$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    if ! $CC -c "$src" -o "$o" -O2 -w -fno-strict-aliasing -fwrapv -fno-common -nostdinc -fno-builtin -funsigned-char \
        -std=gnu90 -D_LANGUAGE_C -DCOMPILER_GCC -DNON_MATCHING -DAVOID_UB -DPLATFORM_N64=1 -DPLATFORM_GC=0 \
        -DPLATFORM_IQUE=0 -DOOT_VERSION=NTSC_1_1 -DOOT_REVISION=1 -DOOT_REGION=REGION_US \
        -DLIBULTRA_VERSION=LIBULTRA_VERSION_I -DLIBULTRA_PATCH=1 -DDEBUG_FEATURES=0 -DNDEBUG -DF3DEX_GBI_2 \
        -DMML_VERSION=MML_VERSION_OOT -D_MIPS_ISA_MIPS2=2 -D_MIPS_ISA=_MIPS_ISA_MIPS2 -D_ABIO32=1 \
        -D_MIPS_SIM=_ABIO32 -D_MIPS_SZINT=32 -D_MIPS_SZLONG=32 -D_MIPS_SZPTR=32 \
        -I"$TREE" -I"$TREE/include" -I"$TREE/include/libc" -Ibuild/ntsc-1.1 -I. -Iextracted/ntsc-1.1 \
        -I"$OOT/$(dirname "$f")" \
        > "$LOG/$(basename "$o").txt" 2>&1; then
        echo "FALHOU $f"
    fi
    exit 0
fi

if [ ! -d "$BUILD/src" ]; then
    echo "ERRO: nao achei $BUILD/src (o decomp precisa ter sido compilado: make REGION=US ...)"
    exit 1
fi

mkdir -p "$OUT" "$OBJ" "$LOG" "$GEN" "$PLAT"
rm -f "$OBJ"/*.o "$LOG"/*.txt "$GEN"/* "$PLAT"/*.o "$OUT"/link*.log "$OUT"/link.status

echo "[1/6] preparando a arvore do port (cabecalhos + arquivos ajustados)..."
python3 "$PORT/tools/oot-patch.py" "$OOT" "$TREE" || exit 1

echo "[2/6] compilando o jogo (pode demorar alguns minutos)..."
cd "$OOT" || exit 1
# lista = objetos que o proprio build da ROM usou, menos o que o port substitui:
#  - n64dd/libleo (64DD), gcc_fix/libgcc (so MIPS), libc/ (memset etc: usa a glibc)
#  - libultra/os, libultra/io, libultra/bb: reescritos em platform/os_core.c e os_hw.c
#    (fica so a tabela de modos de video)
#  - string/ldiv/sprintf: usa a glibc; build.c: identificacao em port_stubs.c
#  - driverominit.c, audio/internal/os.c: reescritos em os_hw.c
#  - carregamento de overlays (loadfragment2/relocation): tudo e ligado estaticamente
(cd "$BUILD" && find src -name '*.o') | sed 's/\.o$/.c/' | sort > "$OUT/candidates.txt"
: > "$OUT/game_files.txt.tmp"
while read -r f; do
    [ -f "$OOT/$f" ] && echo "$f" >> "$OUT/game_files.txt.tmp"
done < "$OUT/candidates.txt"
grep -E -v '^src/(n64dd|libleo|gcc_fix|libgcc|libc)/|^src/libultra/(os|io|bb)/|^src/libultra/libc/(string|ldiv)\.c$|fault_gc|__osMalloc_gc|^src/boot/(build|driverominit)\.c$|^src/audio/internal/os\.c$|^src/libc64/sprintf\.c$|^src/libu64/(loadfragment2_n64|loadfragment2_gc|load_gc|relocation_gc)\.c$' \
    "$OUT/game_files.txt.tmp" > "$OUT/game_files.txt"
grep -E '^src/libultra/io/(vimode|vitbl)' "$OUT/game_files.txt.tmp" >> "$OUT/game_files.txt"
rm -f "$OUT/game_files.txt.tmp"
echo "      $(wc -l < "$OUT/game_files.txt") arquivos"

N=$(nproc 2>/dev/null || echo 2)
OUT="$OUT" ROOT="$ROOT" OOT="$OOT" PORT="$PORT" HOST32="$HOST32" CC="$CC" NM="$NM" TREE="$TREE" OBJ="$OBJ" LOG="$LOG" \
    xargs -P "$N" -I{} sh "$PORT/tools/oot-build.sh" --one {} < "$OUT/game_files.txt" > "$OUT/compile_fail.txt" 2>&1
cd "$ROOT" || exit 1

# Estado rapido (MENU+R2 salva, MENU+L2 carrega; port/savestate.c): os dados e o BSS do jogo vao para secoes proprias
# (game_data / game_bss; o ligador cria __start_/__stop_ delas). Ficam de fora o que pertence as outras threads do
# jogo (escalonador, controles, DMA, erros) e os buffers que o desenho usa (pools de comandos). O som (src/audio,
# gAudioHeap) entra desde 03/11: a thread dele estaciona durante a copia (audio_thread_manager.c, Port_AudioPark):
# esses continuam como estao quando um estado e carregado. O z-buffer entra: o fundo do menu de pausa fica nele. -fno-common (acima) poe as variaveis sem valor
# inicial no BSS do proprio arquivo, em vez de "comuns".
echo "      separando os dados do jogo (estado rapido)..."
NSS=0
while read -r f; do
    case "$f" in
        src/boot/*|src/buffers/gfxbuffers.c|src/libultra/*|src/libu64/*|src/libc/*|src/makerom/*) continue ;;
        src/code/main.c|src/code/sched.c|src/code/irqmgr.c|src/code/padmgr.c|src/code/fault_n64.c|src/code/fault_gc*.c) continue ;;
        src/code/audio_thread_manager.c|src/code/z_std_dma.c|src/code/stackcheck.c) continue ;;
    esac
    o=$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    [ -f "$o" ] || continue
    $OBJCOPY --rename-section .data=game_data --rename-section .bss=game_bss "$o" \
        || echo "FALHOU objcopy $f" >> "$OUT/compile_fail.txt"
    NSS=$((NSS + 1))
done < "$OUT/game_files.txt"
echo "      $NSS arquivos com os dados separados"
# recarga dos overlays como no N64 (port/ovl_reset.c): marca os dados de cada overlay e gera a tabela
OBJCOPY="$OBJCOPY" python3 "$PORT/tools/oot-ovl.py" "$OOT" "$OBJ" "$OUT/game_files.txt" "$GEN" \
    || echo "FALHOU tools/oot-ovl.py" >> "$OUT/compile_fail.txt"
$CC -c "$GEN/port_ovl.c" -o "$GEN/port_ovl.o" -O2 -w 2>> "$OUT/compile_fail.txt" \
    || echo "FALHOU gen/port_ovl.c" >> "$OUT/compile_fail.txt"

echo "[3/6] compilando a camada de plataforma e o programa principal..."
# no ARM: a camada do aparelho (port/miyoo: tela, botoes, som, bateria) e as opcoes do Cortex-A7
DEV_SRC=""
DEV_FLAGS=""
EXTRA_LIBS=""
if [ "$HOST32" != 1 ]; then
    DEV_SRC="$PORT/port/miyoo/*.c"
    DEV_FLAGS="-DTARGET_MIYOO -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
    EXTRA_LIBS="-ldl -lrt"
fi
for f in "$PORT"/platform/*.c "$PORT"/port/*.c "$PORT"/port/gfx/*.c "$PORT"/port/audio/*.c $DEV_SRC; do
    b=$(basename "$f" .c)
    $CC -c "$f" -o "$PLAT/$b.o" -std=gnu99 -O2 -g1 -Wall -ffp-contract=off -D_LANGUAGE_C -DF3DEX_GBI_2 $DEV_FLAGS \
        -I"$PORT/platform" -I"$PORT/port" -idirafter "$OOT/include" 2> "$LOG/platform_$b.txt" \
        || echo "FALHOU $f" >> "$OUT/compile_fail.txt"
done

SYMS=""
[ -f "$SYMDIR/oot-syms.ld" ] && SYMS="$SYMDIR/oot-syms.ld"
export LC_ALL=C

echo "[4/6] 1a ligacao (para descobrir o que falta)..."
# O executavel fica em 0x80400000, logo depois da RAM do N64 (0x80000000..0x803FFFFF): assim os
# enderecos de dados estaticos do jogo (listas de comandos, vertices) tem o mesmo formato dos do N64.
LINKBASE="-Wl,-Ttext-segment=0x80400000"
$CC -no-pie $LINKBASE -rdynamic -o "$OUT/oot_headless" "$OBJ"/*.o "$PLAT"/*.o "$GEN/port_ovl.o" $SYMS -lm -lpthread $EXTRA_LIBS \
    > "$OUT/link1.log" 2>&1
python3 "$PORT/tools/oot-genld.py" "$OOT" "$GEN" "$OUT/link1.log" | tee "$OUT/genld.txt"
$CC -c "$GEN/port_data.c" -o "$GEN/port_data.o" -O2 -w 2>> "$OUT/compile_fail.txt" \
    || echo "FALHOU gen/port_data.c" >> "$OUT/compile_fail.txt"

echo "[5/6] 2a ligacao..."
$CC -no-pie $LINKBASE -rdynamic -o "$OUT/oot_headless" "$OBJ"/*.o "$PLAT"/*.o "$GEN/port_data.o" "$GEN/port_ovl.o" \
    $SYMS "$GEN/oot-port-syms.ld" -lm -lpthread $EXTRA_LIBS -Wl,-Map="$OUT/oot_headless.map" > "$OUT/link.log" 2>&1
echo $? > "$OUT/link.status"
echo "ligacao terminou (codigo $(cat "$OUT/link.status"))"
if [ "$(cat "$OUT/link.status")" = 0 ]; then
    END=$($NM "$OUT/oot_headless" | awk '$3=="_end"{print $1}')
    echo "fim dos dados do executavel (_end): 0x$END"
    case "$END" in
        80[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]) ;;
        *) echo "AVISO: _end fora de 0x80000000..0x80FFFFFF: o renderizador nao enxerga os dados estaticos." ;;
    esac
fi

echo "[6/6] gerando o mapa de conversao dos dados (big-endian -> nativo)..."
# precisa do decomp ja compilado (ELF MIPS) e da ROM descomprimida que o proprio decomp gera.
# SKIP_ASSETS=1 reaproveita o oot_swapmap.bin que ja esta em $OUT.
if [ "$SKIP_ASSETS" = 1 ] && [ -f "$OUT/oot_swapmap.bin" ]; then
    echo "      (reaproveitando $OUT/oot_swapmap.bin)"
elif [ ! -f "$OOT/baseroms/ntsc-1.1/baserom-decompressed.z64" ]; then
    echo "AVISO: falta $OOT/baseroms/ntsc-1.1/baserom-decompressed.z64 (gerado pelo make do decomp): sem mapa."
else
    python3 "$PORT/tools/oot-assets.py" "$OOT" "$TREE" "$OUT/assets" --report "$OUT/assets_report.txt" \
        > "$OUT/assets_log.txt" 2>&1
    tail -n 8 "$OUT/assets_log.txt"
    if [ -f "$OUT/assets/oot_swapmap.bin" ]; then
        cp "$OUT/assets/oot_swapmap.bin" "$OUT/oot_swapmap.bin"
        echo "mapa: $OUT/oot_swapmap.bin"
    else
        echo "AVISO: o mapa nao foi gerado. Veja $OUT/assets_log.txt"
    fi
fi

python3 "$PORT/tools/oot-linkreport.py" "$OUT" "$REPORT" "$NM"
if [ -f "$OUT/assets_log.txt" ]; then
    { echo; echo "=== mapa de conversao (ultimas linhas de assets_log.txt) ==="; tail -n 40 "$OUT/assets_log.txt"; } >> "$REPORT"
fi
echo "relatorio: $REPORT"
[ "$(cat "$OUT/link.status")" = 0 ] && echo "executavel: $OUT/oot_headless"
