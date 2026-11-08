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
for C in lento_00.cap lento_07.cap ../oot_kakariko/lento_08.cap ../oot_kakariko/lento_10.cap; do
  [ -f $C ] || continue
  echo "== $C"
  BGDBG=1 ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido 2>&1 | grep "^\[fundo\] fecha" | head -4
  for v in "novo:" "sem_parcial:--sem-fundo-parcial" "novo:" "sem_parcial:--sem-fundo-parcial"; do
    nome=${v%%:*}; op=${v#*:}
    printf "%-12s " $nome
    PMFORCE=1 ./rp $C $R $op --repeat 60 2>&1 | grep "^tempo" | tr '\n' ' '
    echo
  done
done
# imagens com e sem o fundo parcial (lote 192) em varias cenas: iguais? (as diferentes ficam em reg_dif/)
mkdir -p reg_dif
rm -f reg_dif/*
for C in reg/*.cap lento_00.cap lento_07.cap ../oot_kakariko/lento_08.cap ../oot_kakariko/lento_10.cap ../oot_kakariko/lento_15.cap; do
  [ -f $C ] || continue
  b=$(basename $C .cap)
  ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido --out /tmp/a.png > /dev/null 2>&1
  ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido --sem-fundo-parcial --out /tmp/b.png > /dev/null 2>&1
  if cmp -s /tmp/a.png /tmp/b.png; then echo "igual     $b"; else echo "DIFERENTE $b"; cp /tmp/a.png reg_dif/$b.novo.png; cp /tmp/b.png reg_dif/$b.antes.png; fi
done
echo fim
Y
chmod +x "$OUT/rodar.sh"
echo "ok: $OUT/rp e $OUT/rodar.sh"
