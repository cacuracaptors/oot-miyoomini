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
FLAGS="-O2 -std=gnu99 -ffp-contract=off -fno-strict-aliasing -D_LANGUAGE_C -DF3DEX_GBI_2 -DGX_REPLAY_DIAG -I$G -I$PORT/port -w -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static"
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
for n in 00 03 07; do
  C=lento_$n.cap
  [ -f $C ] || continue
  echo "== $C"
  BGDBG=1 ./rp $C --fast --threads 2 --resolucao 384 2>&1 | grep "^\[fundo\]" | head -8
  for v in "inteiro:" "sem_ceu:$CEU" "sem_fundo_do_lago:02010eb8" "sem_agua:060025a0" "sem_lago_e_agua:02010eb8,060025a0"; do
    nome=${v%%:*}; sk=${v#*:}
    printf "%-20s " $nome
    TSKIP=$sk ./rp $C --fast --threads 2 --pin --rt 1 --resolucao 384 --filtro-rapido --repeat 60 2>&1 | grep "^tempo"
  done
  printf "%-20s " "sem_fundo_adiado"
  ./rp $C --fast --threads 2 --pin --rt 1 --resolucao 384 --filtro-rapido --sem-fundo --repeat 60 2>&1 | grep "^tempo"
done
echo fim
Y
chmod +x "$OUT/rodar.sh"
echo "ok: $OUT/rp e $OUT/rodar.sh"
