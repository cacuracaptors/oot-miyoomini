#!/bin/sh
# Compila o reprodutor de quadros (tools/oot_replay.c) para o PC e para o Miyoo Mini Plus.
# Roda NO WSL:   sh ~/fallout-miyoo/oot-miyoo/tools/oot-replay-build.sh
#
# Resultado em ~/fallout-miyoo/oot-replay/:
#   oot_replay       -> roda no PC (WSL)
#   oot_replay_arm   -> roda no Miyoo (copie junto com os .cap)
#
# Os dois usam -ffp-contract=off: o compilador nao pode fundir multiplicacao e soma de jeitos
# diferentes no caminho rapido e na referencia, senao a comparacao bit a bit deixa de valer.
# -g so acrescenta informacao de depuracao (para o perfil por linha); nao muda o codigo gerado.
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
OUT="$BASE/oot-replay"
SRC="$PORT/tools/oot_replay.c $PORT/port/gfx/gfx_rsp.c $PORT/port/gfx/gfx_rdp.c $PORT/port/gfx/gfx_tri.c $PORT/port/gfx/gfx_dump.c $PORT/port/gfx/gfx_capture.c $PORT/port/gfx/gfx_hires.c $PORT/port/gfx/gfx_ge.c"
FLAGS="-O2 -g -std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -I$PORT/port/gfx -Wall -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast"
mkdir -p "$OUT" || exit 1

echo "== PC =="
gcc $FLAGS -msse4.1 $SRC -lm -lrt -pthread -o "$OUT/oot_replay" || { echo "ERRO: falhou a compilacao para o PC"; exit 1; }
echo "ok: $OUT/oot_replay"

echo "== Miyoo =="
CROSS=""
ACHADO="$(find "$BASE/miyoomini-toolchain" -maxdepth 3 -name arm-linux-gnueabihf-gcc 2>/dev/null | head -1 | sed 's/gcc$//')"
for c in "$CROSS_PREFIX" "$ACHADO" \
         "/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-" \
         "/opt/miyoomini-toolchain/bin/arm-linux-gnueabihf-"; do
    if [ -n "$c" ] && [ -x "${c}gcc" ]; then CROSS="$c"; break; fi
done
if [ -z "$CROSS" ]; then
    echo "ERRO: compilador ARM (arm-linux-gnueabihf-gcc) do container nao encontrado em $BASE/miyoomini-toolchain."
    echo "Copie uma vez (Docker Desktop aberto), no WSL:"
    echo "  docker cp miyoomini-toolchain-debug:/opt/miyoomini-toolchain $BASE/miyoomini-toolchain"
    exit 1
fi
echo "compilador: ${CROSS}gcc ($(${CROSS}gcc -dumpversion))"
${CROSS}gcc $FLAGS -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard $SRC -lm -lrt -pthread \
    -o "$OUT/oot_replay_arm" || { echo "ERRO: falhou a compilacao para o Miyoo"; exit 1; }
echo "ok: $OUT/oot_replay_arm"
md5sum "$OUT/oot_replay" "$OUT/oot_replay_arm"
