#!/bin/sh
# Perfil do desenho NO APARELHO: em que funcoes e linhas o tempo vai, em algumas capturas (pasta do jogo).
# Roda NO WSL, em duas vezes:
#   sh tools/oot-perfil-capturas.sh            -> compila o oot_replay do Miyoo e o roteiro em Desktop/oot_perfil
#   (scp da pasta para o aparelho, ssh rodando o roteiro, scp das amostras de volta)
#   sh tools/oot-perfil-capturas.sh relatorio  -> transforma as amostras em Desktop/oot_perfil/perfil_NN.txt
PORT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$HOME/fallout-miyoo"
TC="$BASE/miyoomini-toolchain/bin/arm-linux-gnueabihf-gcc"
OUT="/mnt/c/Users/Régis/Desktop/oot_perfil"
G="$PORT/port/gfx"
if [ "$1" = "relatorio" ]; then
    for a in "$OUT"/amostras_*.txt; do
        [ -f "$a" ] || { echo "ERRO: nenhuma amostra em $OUT (faltou o scp de volta?)"; exit 1; }
        n="$(basename "$a" .txt | sed 's/amostras_//')"
        sh "$PORT/tools/oot-prof-report.sh" "$a" "$OUT/rp_o2" "$OUT/perfil_$n.txt" > /dev/null && echo "ok: perfil_$n.txt"
    done
    exit 0
fi
SRC="$PORT/tools/oot_replay.c $G/gfx_rsp.c $G/gfx_rdp.c $G/gfx_tri.c $G/gfx_dump.c $G/gfx_capture.c $G/gfx_hires.c $G/gfx_ge.c"
FLAGS="-g -std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -I$G -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
[ -x "$TC" ] || { echo "ERRO: falta $TC"; exit 1; }
mkdir -p "$OUT" || exit 1
rm -f "$OUT"/amostras_*.txt "$OUT"/perfil_*.txt
"$TC" -O2 $FLAGS $SRC -lm -lrt -pthread -o "$OUT/rp_o2" || { echo "ERRO na compilacao"; exit 1; }
cat > "$OUT/rodar.sh" <<'Y'
#!/bin/sh
# roda NO APARELHO (chamado pelo ssh): cada captura 300 vezes, amostrando onde o processador esta, como no jogo
cd /mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT || exit 1
B=/mnt/SDCARD/oot_perfil
chmod +x $B/rp_o2
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
for n in 02 09 15 17; do
  [ -f captura_$n.cap ] || continue
  echo "== captura_$n"
  $B/rp_o2 captura_$n.cap --fast --threads 2 --pin --repeat 300 --rt 1 --filtro-rapido --prof $B/amostras_$n.txt 2>&1 | grep "^tempo\|^threads"
done
echo fim
Y
ls -la "$OUT"
