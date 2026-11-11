#!/bin/sh
# Compara NO APARELHO o tempo de desenho de duas versoes do renderizador: o gfx_fast.inc de ANTES (um arquivo guardado)
# e o atual (DEPOIS), nas capturas da pasta do jogo (lento_*.cap e captura_*.cap, ate 10), em 576 de largura.
# Roda NO WSL:   sh tools/oot-bench-ab.sh "/mnt/c/Users/Régis/Desktop/oot_lote239/antes/gfx_fast.inc"
# Monta os dois programas e o roteiro rodar.sh em Desktop/oot_bench_ab. Depois: scp da pasta para o aparelho e ssh
# rodando o roteiro (ele mostra uma linha por medicao e, no fim, a soma de cada versao e a diferenca).
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$BASE/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc"
OUT="/mnt/c/Users/Régis/Desktop/oot_bench_ab"
TMP="$HOME/oot_bench_ab_tmp"
ANTES="$1"
FLAGS="-std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -O2"
[ -x "$TC" ] || { echo "ERRO: falta $TC"; exit 1; }
[ -f "$ANTES" ] || { echo "ERRO: diga o gfx_fast.inc de antes (falta: $ANTES)"; exit 1; }
rm -rf "$OUT" "$TMP"
mkdir -p "$OUT" "$TMP" || exit 1
for v in antes depois; do
    mkdir -p "$TMP/$v" || exit 1
    cp -r "$PORT/port/gfx" "$TMP/$v/gfx" && cp "$PORT"/port/*.h "$TMP/$v/" || exit 1
    if [ "$v" = antes ]; then
        cp "$ANTES" "$TMP/$v/gfx/gfx_fast.inc" || exit 1
    fi
    G="$TMP/$v/gfx"
    "$TC" $FLAGS -I"$G" "$PORT/tools/oot_replay.c" "$G/gfx_rsp.c" "$G/gfx_rdp.c" "$G/gfx_tri.c" "$G/gfx_dump.c" \
        "$G/gfx_capture.c" "$G/gfx_hires.c" "$G/gfx_ge.c" -lm -lrt -pthread -o "$OUT/rp_$v" ||
        { echo "ERRO na compilacao ($v)"; exit 1; }
done
cat > "$OUT/rodar.sh" <<'X'
#!/bin/sh
# roda NO APARELHO (chamado pelo ssh): cada captura 30 vezes com cada versao, 3 rodadas alternadas, com as threads do
# desenho em tempo real (--rt 1, como no jogo)
cd /mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT || exit 1
B=/mnt/SDCARD/oot_bench_ab
chmod +x $B/rp_antes $B/rp_depois
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
CAPS="$(ls -t lento_*.cap captura_*.cap 2>/dev/null | head -10)" # (as 10 mais novas)
[ -n "$CAPS" ] || { echo "ERRO: nenhuma captura (lento_*.cap ou captura_*.cap) na pasta do jogo"; exit 1; }
echo "clock: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) temperatura: $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
: > $B/resultado.txt
for r in 1 2 3; do
  for c in $CAPS; do
    for v in antes depois; do
      L="$($B/rp_$v $c --fast --threads 2 --pin --repeat 30 --rt 1 --filtro-rapido --resolucao 576 2>&1 | grep '^tempo')"
      echo "$c r$r $v $L" | tee -a $B/resultado.txt
    done
  done
done
echo "clock: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) temperatura: $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
# linha: captura rN versao tempo: melhor X ms, media Y ms (30 vezes)
awk '$4 == "tempo:" { b[$3] += $6; m[$3] += $9; n[$3]++ }
     END { if (n["antes"] == 0 || n["depois"] == 0 || n["antes"] != n["depois"]) { print "ERRO: medicoes incompletas"; exit }
           printf "SOMA do melhor tempo: antes %.1f ms, depois %.1f ms (%+.2f%%)\n", b["antes"], b["depois"], 100 * (b["depois"] - b["antes"]) / b["antes"]
           printf "SOMA da media:        antes %.1f ms, depois %.1f ms (%+.2f%%)\n", m["antes"], m["depois"], 100 * (m["depois"] - m["antes"]) / m["antes"] }' $B/resultado.txt
echo fim
X
ls -la "$OUT"
