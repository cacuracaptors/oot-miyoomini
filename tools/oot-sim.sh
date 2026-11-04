#!/bin/sh
# Roda o jogo no PC (WSL, build i386) num lugar da lista tools/oot-tour-list.txt, grava quadros do jogo e
# os quadros intermediarios (30 fps) em PNG e o log. Tudo vai para uma pasta na Area de Trabalho.
#
#   sh tools/oot-sim.sh [ENTRADA] [SEGUNDOS] [CENA]
#   sem argumentos: a abertura do jogo (--warp 0xBB --cena 0xFFF1, 125 s), a mesma que usamos nos testes
#   ex.: sh tools/oot-sim.sh 0x400 40        patio do castelo
#   variaveis: EVERY=quadros entre os PNG do jogo (60), MAXQ=quantos PNG (120), INTERP=desde,quantas (intermediarios; vazio = nao grava)
#
# Antes: HOST32=1 sh tools/oot-build.sh (cria ~/fallout-miyoo/oot-i386/oot_headless).
ROOT=${ROOT:-$HOME/fallout-miyoo}
BIN=${BIN:-$ROOT/oot-i386}
DESK=${DESK:-/mnt/c/Users/Régis/Desktop}
ENT=${1:-0xBB}
SECS=${2:-125}
CENA=${3:-}
[ "$ENT" = 0xBB ] && [ -z "$CENA" ] && CENA=0xFFF1
EVERY=${EVERY:-60}
MAXQ=${MAXQ:-120}
INTERP=${INTERP:-}
OUT="$DESK/oot_sim_$ENT"
[ -x "$BIN/oot_headless" ] || { echo "ERRO: falta $BIN/oot_headless (HOST32=1 sh tools/oot-build.sh)"; exit 1; }
# a ROM (so e usada se o oot.cart estiver faltando ou for de um mapa antigo: ai o jogo refaz o cartucho)
ROM=$(ls "$BIN"/*.z64 "$BIN"/*.n64 "$BIN"/*.v64 2>/dev/null | head -1)
if [ -z "$ROM" ]; then
    ZIP="$DESK/Legend of Zelda, The - Ocarina of Time (USA) (Rev A).zip"
    if [ -f "$ZIP" ]; then
        python3 - "$ZIP" "$BIN/rom_sim.n64" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
n = [i for i in z.namelist() if i.lower().endswith((".n64", ".z64", ".v64"))][0]
open(sys.argv[2], "wb").write(z.read(n))
PY
        ROM="$BIN/rom_sim.n64"
    fi
fi
# (o nome da ROM tem espacos: o jogo recebe um atalho sem espacos)
ROMARG=""
if [ -n "$ROM" ]; then
    ln -sf "$ROM" /tmp/oot_rom_sim.n64
    ROMARG="--rom /tmp/oot_rom_sim.n64"
fi
rm -rf "$OUT"
mkdir -p "$OUT" || exit 1
cd "$BIN" || exit 1
INTERPARG=""
[ -n "$INTERP" ] && INTERPARG="--interp-dump $OUT/i,$INTERP"
CENAARG=""
[ -n "$CENA" ] && CENAARG="--cena $CENA"
./oot_headless $ROMARG --warp "$ENT" $CENAARG --seconds "$SECS" --mudo --sram /tmp/oot_sim.sav \
    --dump-frames "$OUT/q" --dump-every "$EVERY" --dump-max "$MAXQ" $INTERPARG > "$OUT/log.txt" 2>&1
grep -E "^\[halo\]|^\[sol\]" "$OUT/log.txt" | head -300 > "$OUT/halo.txt"
head -3 "$OUT/log.txt" | grep -q "ERRO\|Nao achei" && { echo "o jogo nao abriu:"; head -5 "$OUT/log.txt"; }
echo "pronto: $OUT"
echo "  $(ls "$OUT"/q*.png 2>/dev/null | wc -l) quadros do jogo, $(ls "$OUT"/i*.png 2>/dev/null | wc -l) intermediarios"
echo "  $(wc -l < "$OUT/halo.txt") linhas [halo]/[sol] em halo.txt"
