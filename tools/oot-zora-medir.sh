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
R="--fast --threads 2 --pin --rt 1 --resolucao 384 --filtro-rapido --extras"
B="--fast --resolucao 384 --filtro-rapido --fundo-parcial"
mkdir -p diag
rm -f diag/*
for C in ../oot_kakariko/lento_08.cap reg/captura_03.cap; do
  b=$(basename $C .cap)
  echo "== $C"
  # 1) o fundo adiado antigo esta certo? (sem a mudanca: com e sem o fundo adiado)
  ./rp $C $B --threads 2 --sem-fundo-parcial --out diag/$b.antigo.png > /dev/null 2>&1
  ./rp $C $B --threads 2 --sem-fundo --out diag/$b.sem_fundo.png > /dev/null 2>&1
  if cmp -s diag/$b.antigo.png diag/$b.sem_fundo.png; then echo "fundo adiado antigo: igual a sem fundo adiado"; else echo "fundo adiado antigo: DIFERENTE de sem fundo adiado"; fi
  # 2) com 1 e 2 threads
  ./rp $C $B --threads 1 --out diag/$b.novo1.png > /dev/null 2>&1
  ./rp $C $B --threads 2 --out diag/$b.novo2.png > /dev/null 2>&1
  if cmp -s diag/$b.novo1.png diag/$b.antigo.png; then echo "novo 1 thread: igual"; else echo "novo 1 thread: DIFERENTE"; fi
  if cmp -s diag/$b.novo2.png diag/$b.antigo.png; then echo "novo 2 threads: igual"; else echo "novo 2 threads: DIFERENTE"; fi
  # 3) o primeiro retangulo que estraga a imagem (1 thread)
  K=1
  while [ $K -le 80 ]; do
    BGPMAX=$K ./rp $C $B --threads 1 --out /tmp/k.png > /dev/null 2>&1
    if ! cmp -s /tmp/k.png diag/$b.antigo.png; then
      echo "primeiro retangulo errado: $K"
      cp /tmp/k.png diag/$b.k$K.png
      BGPMAX=$((K-1)) ./rp $C $B --threads 1 --out diag/$b.k$((K-1)).png > /dev/null 2>&1
      BGDBG=1 BGPMAX=$K ./rp $C $B --threads 1 2>&1 | grep "^\[fundo\]" | tail -6
      BGCOVDUMP=$K BGPMAX=$K ./rp $C $B --threads 1 > /dev/null 2>&1; [ -f cov.pgm ] && mv cov.pgm diag/$b.cov$K.pgm
      if [ $K -gt 1 ]; then BGCOVDUMP=$((K-1)) BGPMAX=$K ./rp $C $B --threads 1 > /dev/null 2>&1; [ -f cov.pgm ] && mv cov.pgm diag/$b.cov$((K-1)).pgm; fi
      break
    fi
    K=$((K+1))
  done
  [ $K -gt 80 ] && echo "nenhum dos 80 primeiros retangulos estraga"
  # 4) tempo
  for v in "novo:" "sem_parcial:--sem-fundo-parcial"; do
    nome=${v%%:*}; op=${v#*:}
    printf "%-12s " $nome
    PMFORCE=1 ./rp $C $R --fundo-parcial $op --repeat 60 2>&1 | grep "^tempo"
  done
done
echo fim
Y
chmod +x "$OUT/rodar.sh"
echo "ok: $OUT/rp e $OUT/rodar.sh"
