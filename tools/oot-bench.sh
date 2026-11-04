#!/bin/sh
# Mede o desenho no aparelho com os quadros lentos (lento_*.cap da pasta do jogo). Roda NO WSL:
#   sh tools/oot-bench.sh
# Compila o oot_replay para o Miyoo (-O2, como o jogo) e o roteiro do aparelho, tudo em
# Desktop/oot_bench. Depois: mandar a pasta para o aparelho (scp) e rodar o roteiro (ssh); o
# resultado volta para Desktop/oot_bench/resultado.txt. Para cada quadro lento:
#   tempo de cada uma das 40 vezes com as threads do desenho em tempo real (como no jogo).
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$BASE/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc"
OUT="/mnt/c/Users/Régis/Desktop/oot_bench"
SRC="$PORT/tools/oot_replay.c $PORT/port/gfx/gfx_rsp.c $PORT/port/gfx/gfx_rdp.c $PORT/port/gfx/gfx_tri.c $PORT/port/gfx/gfx_dump.c $PORT/port/gfx/gfx_capture.c $PORT/port/gfx/gfx_hires.c"
FLAGS="-g -std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -I$PORT/port/gfx -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
[ -x "$TC" ] || { echo "ERRO: falta $TC"; exit 1; }
mkdir -p "$OUT" || exit 1
rm -f "$OUT"/rp_o3 "$OUT"/perfil_*.txt "$OUT"/resultado.txt
"$TC" -O2 $FLAGS $SRC -lm -lrt -pthread -o "$OUT/rp_o2" || { echo "ERRO na compilacao"; exit 1; }
cat > "$OUT/rodar.sh" <<'X'
#!/bin/sh
# roda NO APARELHO (chamado pelo ssh): cada quadro lento 30 vezes (normal e com cada opcao de EXTRA), com as threads do desenho em
# tempo real (--rt 1, como no jogo), mostrando o tempo de cada vez
cd /mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT || exit 1
B=/mnt/SDCARD/oot_bench
EXTRA="--sem-roubo"   # cada quadro tambem com cada uma destas opcoes (vazio = so o normal)
chmod +x $B/rp_o2
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
for c in lento_*.cap; do
  for m in "" $EXTRA; do
    echo "== $c $m"
    $B/rp_o2 $c --fast --threads 2 --pin --repeat 30 --each --rt 1 --filtro-rapido $m 2>&1 | grep -v "^cpu:\|paginas="
  done
done
echo fim
X
ls -la "$OUT"
md5sum "$OUT"/rp_o2
