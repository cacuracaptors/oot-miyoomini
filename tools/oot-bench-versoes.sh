#!/bin/sh
# Compara NO APARELHO o desenho de varias versoes do renderizador (gfx_fast.inc dos lotes 115, 117, 120, 123 e 126,
# guardados no Desktop, e o atual com as 3 opcoes da dobra do 2o ciclo) nas capturas da pasta do jogo.
# Roda NO WSL:  sh tools/oot-bench-versoes.sh
# Monta os programas em Desktop/oot_bench_v e o roteiro rodar.sh. Depois: scp da pasta para o aparelho e ssh rodando.
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$BASE/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc"
DESK="/mnt/c/Users/Régis/Desktop"
OUT="$DESK/oot_bench_v"
TMP="$HOME/oot_bench_v_tmp"
FLAGS="-std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -O2"
[ -x "$TC" ] || { echo "ERRO: falta $TC"; exit 1; }
rm -rf "$OUT" "$TMP"
mkdir -p "$OUT" "$TMP" || exit 1
printf 'int gx_ceu_mistura __attribute__((weak));\nint gx_corte __attribute__((weak));\nint gx_tex_meia __attribute__((weak));\nint gx_dobra_comb __attribute__((weak));\n' > "$TMP/stubs.c"
for v in 115 117 120 123 126 atual; do
    mkdir -p "$TMP/v$v" || exit 1
    cp -r "$PORT/port/gfx" "$TMP/v$v/gfx" && cp "$PORT"/port/*.h "$TMP/v$v/" || exit 1
    if [ "$v" != atual ]; then
        cp "$DESK/oot_lote$v/gfx_fast.inc" "$TMP/v$v/gfx/" || { echo "ERRO: falta $DESK/oot_lote$v/gfx_fast.inc"; exit 1; }
    fi
    G="$TMP/v$v/gfx"
    "$TC" $FLAGS -I"$G" "$PORT/tools/oot_replay.c" "$G/gfx_rsp.c" "$G/gfx_rdp.c" "$G/gfx_tri.c" "$G/gfx_dump.c" \
        "$G/gfx_capture.c" "$G/gfx_hires.c" "$G/gfx_ge.c" "$TMP/stubs.c" -lm -lrt -pthread -o "$OUT/rp_$v" ||
        { echo "ERRO na compilacao da versao $v"; exit 1; }
    echo "ok: rp_$v"
done
rm -rf "$TMP"
cat > "$OUT/rodar.sh" <<'X'
#!/bin/sh
# roda NO APARELHO (chamado pelo ssh): tempo de desenho de cada captura em cada versao, 3 rodadas intercaladas
cd /mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT || exit 1
B=/mnt/SDCARD/oot_bench_v
chmod +x $B/rp_*
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo "clock: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) temperatura: $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
# imagem da versao atual com a dobra 1 e 2 igual a da referencia (sem as aproximacoes)?
for c in captura_*.cap; do
  for d in 1 2; do
    printf "exato %s dobra%s: " "$c" "$d"; $B/rp_atual $c --fast --compare --sem-aprox --sem-mistura --dobra-comb $d 2>&1 | grep "comparacao"
  done
done
for r in 1 2 3; do
  for c in captura_*.cap; do
    for v in 115 117 120 123 126; do
      printf "%s r%s %-8s " "$c" "$r" "$v"; $B/rp_$v $c --fast --threads 2 --pin --repeat 30 --rt 1 --filtro-rapido 2>&1 | grep "^tempo"
    done
    for d in 0 1 2; do
      printf "%s r%s %-8s " "$c" "$r" "atual$d"; $B/rp_atual $c --fast --threads 2 --pin --repeat 30 --rt 1 --filtro-rapido --dobra-comb $d 2>&1 | grep "^tempo"
    done
  done
done
echo "clock: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) temperatura: $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
echo fim
X
ls -la "$OUT"
