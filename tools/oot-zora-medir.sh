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
R="--fast --threads 2 --pin --rt 1 --resolucao 384 --filtro-rapido --extras"
for n in 00 07; do
  C=lento_$n.cap
  [ -f $C ] || continue
  echo "== $C"
  BGDBG=1 ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido 2>&1 | grep "^\[prog\]" | grep "800ac100\|800b0100\|02010eb8\|060025a0\|0200f6b8"
  for v in "inteiro:" "sem_alias:--sem-alias" "inteiro:" "sem_alias:--sem-alias"; do
    nome=${v%%:*}; op=${v#*:}
    printf "%-12s " $nome
    PMFORCE=1 ./rp $C $R $op --repeat 60 2>&1 | grep "^tempo\|^threads" | tr '\n' ' '
    echo
  done
done
# imagens com e sem o atalho do combinador (lote 190) em varias cenas: iguais? (as diferentes ficam em reg_dif/)
mkdir -p reg_dif
for C in reg/*.cap; do
  b=$(basename $C .cap)
  ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido --out /tmp/a.png > /dev/null 2>&1
  ./rp $C --fast --threads 2 --resolucao 384 --filtro-rapido --sem-alias --out /tmp/b.png > /dev/null 2>&1
  if cmp -s /tmp/a.png /tmp/b.png; then echo "igual     $b"; else echo "DIFERENTE $b"; cp /tmp/a.png reg_dif/$b.novo.png; cp /tmp/b.png reg_dif/$b.antes.png; fi
done
echo fim
Y
chmod +x "$OUT/rodar.sh"
echo "ok: $OUT/rp e $OUT/rodar.sh"
