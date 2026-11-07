#!/bin/sh
# Diagnostico da cena da agua (Lago Hylia / Dominio Zora): quanto custa cada parte do desenho NO APARELHO.
# Roda NO WSL. Compila o reprodutor de quadros para o Miyoo com GX_REPLAY_DIAG (TSKIP / BGDBG; nada disso
# entra no jogo) e escreve Desktop/oot_zora/rp + rodar.sh. Os lento_NN.cap ficam na mesma pasta.
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$(find "$BASE/miyoomini-toolchain" -maxdepth 3 -name arm-linux-gnueabihf-gcc 2>/dev/null | head -1)"
OUT="/mnt/c/Users/Régis/Desktop/oot_zora"
G="$PORT/port/gfx"
SRC="$PORT/tools/oot_replay.c $G/gfx_rsp.c $G/gfx_rdp.c $G/gfx_tri.c $G/gfx_dump.c $G/gfx_capture.c $G/gfx_hires.c $G/gfx_ge.c"
FLAGS="-O2 -g -std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -DGX_REPLAY_DIAG -I$G -I$PORT/port -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
[ -x "$TC" ] || { echo "ERRO: nao achei o arm-linux-gnueabihf-gcc em $BASE/miyoomini-toolchain"; exit 1; }
mkdir -p "$OUT" || exit 1
"$TC" $FLAGS $SRC -lm -lrt -pthread -o "$OUT/rp" || { echo "ERRO na compilacao"; exit 1; }
cat > "$OUT/rodar.sh" <<'Y'
#!/bin/sh
# roda NO APARELHO: cada captura 60 vezes a 384 com 2 threads, inteira e sem cada parte
cd /mnt/SDCARD/oot_zora || exit 1
chmod +x ./rp
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
CEU=800ac100,800b0100,800b0100
for n in 00 07; do
  C=lento_$n.cap
  [ -f $C ] || continue
  echo "== $C"
  for m in 0 1 2; do
    # PMFORCE: refaz a mistura em toda repeticao, como no jogo com a agua deslizando
    printf "premix_scrolling=%s  " $m
    PMFORCE=1 ./rp $C --fast --threads 2 --pin --rt 1 --resolucao 384 --filtro-rapido --mistura-desliza $m --repeat 60 2>&1 | grep "^tempo\|^misturas" | tr '\n' ' '
    echo
  done
done
echo fim
Y
chmod +x "$OUT/rodar.sh"
echo "ok: $OUT/rp e $OUT/rodar.sh"
