#!/bin/sh
# Mede no aparelho o desenho das capturas da pasta do jogo (captura_NN.cap). Roda NO WSL:
#   sh tools/oot-bench-dianoite.sh
# Compila o oot_replay para o Miyoo (-O2, como o jogo) e o roteiro do aparelho em Desktop/oot_bench_dn. Depois:
# mandar a pasta para o aparelho (scp) e rodar o roteiro (ssh), guardando o resultado em resultado.txt.
# Lote 126: recorte da parte vazia (so texturas com metade ou mais vazia, contas em float): comparacao e tempos.
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$BASE/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc"
OUT="/mnt/c/Users/Régis/Desktop/oot_bench_dn"
G="$PORT/port/gfx"
SRC="$PORT/tools/oot_replay.c $G/gfx_rsp.c $G/gfx_rdp.c $G/gfx_tri.c $G/gfx_dump.c $G/gfx_capture.c $G/gfx_hires.c $G/gfx_ge.c"
FLAGS="-std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -I$G -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
[ -x "$TC" ] || { echo "ERRO: falta $TC"; exit 1; }
mkdir -p "$OUT" || exit 1
rm -f "$OUT"/resultado.txt "$OUT"/img_*.png
"$TC" -O2 $FLAGS $SRC -lm -lrt -pthread -o "$OUT/rp_o2" || { echo "ERRO na compilacao"; exit 1; }
cat > "$OUT/rodar.sh" <<'X'
#!/bin/sh
# roda NO APARELHO (chamado pelo ssh): imagens da Navi e do ceu (img_*) e tempos do ceu pre-misturado
cd /mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT || exit 1
B=/mnt/SDCARD/oot_bench_dn
chmod +x $B/rp_o2
rm -f $B/img_*.png
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
# lote 125: recorte da parte vazia das texturas transparentes: imagem igual a referencia (sem as aproximacoes) e tempos
for c in captura_09.cap captura_13.cap captura_14.cap captura_15.cap captura_16.cap captura_17.cap captura_41.cap; do
  [ -f $c ] || continue
  printf "exato: "; $B/rp_o2 $c --fast --compare --sem-aprox --sem-mistura 2>&1 | grep "comparacao"
done
for r in 1 2 3; do
  for c in captura_09.cap captura_13.cap captura_15.cap captura_16.cap captura_17.cap captura_41.cap; do
    [ -f $c ] || continue
    for m in "" "--sem-corte"; do
      printf "%s rodada %s %-7s " "$c" "$r" "$([ -z "$m" ] && echo novo || echo antes)"
      $B/rp_o2 $c --fast --threads 2 --pin --repeat 40 --rt 1 --filtro-rapido $m 2>&1 | grep "^tempo"
    done
  done
done
echo fim
X
ls -la "$OUT"
