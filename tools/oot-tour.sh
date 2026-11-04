#!/bin/sh
# Passeio automatico pelo jogo (no PC, com o build i386): entra direto em cada lugar da lista
# tools/oot-tour-list.txt (os 125 lugares do menu de mapas do decomp), roda alguns segundos e
# grava quadros em PNG. Serve para achar defeitos de imagem no jogo todo sem precisar jogar.
#
#   sh tools/oot-tour.sh              crianca, de dia
#   sh tools/oot-tour.sh adulto       Link adulto
#
# Varios lugares rodam ao mesmo tempo (um por nucleo). No fim: um zip no Desktop com os quadros
# e um resumo (lugares que travaram ou nao mostraram nada).
ROOT=${ROOT:-$HOME/fallout-miyoo}
BIN=${BIN:-$ROOT/oot-i386}
DESK=${DESK:-/mnt/c/Users/Régis/Desktop}
LIST=$(cd "$(dirname "$0")" && pwd)/oot-tour-list.txt
AGE=${1:-crianca}
SECS=${SECS:-11}
OUTDIR=${OUTDIR:-/tmp/oot-tour-$AGE}
AGEFLAG=""
[ "$AGE" = adulto ] && AGEFLAG="--adulto"

[ -x "$BIN/oot_headless" ] || { echo "ERRO: falta $BIN/oot_headless (HOST32=1 sh tools/oot-build.sh)"; exit 1; }
[ -f "$BIN/oot.cart" ] || { echo "ERRO: falta $BIN/oot.cart (rode o jogo uma vez com --rom)"; exit 1; }
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR" || exit 1
N=$(nproc 2>/dev/null || echo 2)
TOTAL=$(grep -vc '^#' "$LIST")
echo "passeio ($AGE): $TOTAL lugares, $N de cada vez, ~$SECS s cada..."
cd "$BIN" || exit 1
grep -v '^#' "$LIST" | while read -r ENT NAME LABEL; do
    echo "$ENT $LABEL"
done | xargs -P "$N" -L1 sh -c '
    ent=$0; label=$1
    tag=$(printf "%s_%s" "$ent" "$label")
    ./oot_headless --warp "$ent" '"$AGEFLAG"' --seconds '"$SECS"' --sram "'"$OUTDIR"'/sav_$ent" \
        --dump-frames "'"$OUTDIR"'/$tag" --dump-every 50 --dump-max 4 > "'"$OUTDIR"'/log_$tag.txt" 2>&1
    n=$(ls "'"$OUTDIR"'"/"$tag"_*.png 2>/dev/null | wc -l)
    if grep -q "FALHA" "'"$OUTDIR"'/log_$tag.txt"; then st="TRAVOU"; else st="ok"; fi
    echo "$tag: $n quadros, $st"
    rm -f "'"$OUTDIR"'/sav_$ent"
' | sort > "$OUTDIR/resumo.txt"

echo
grep -c ": 4 quadros, ok" "$OUTDIR/resumo.txt" | awk '{print $1 " lugares com 4 quadros e sem travar"}'
grep -v ": 4 quadros, ok" "$OUTDIR/resumo.txt" | sed 's/^/  atencao: /'
ZIP="$DESK/oot_passeio_$AGE.zip"
rm -f "$ZIP"
python3 - "$OUTDIR" "$ZIP" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(src)):
        if f.endswith(".png") or f == "resumo.txt" or (f.startswith("log_") and os.path.getsize(os.path.join(src, f)) < 200000):
            z.write(os.path.join(src, f), f)
PY
ls -l "$ZIP" | awk '{print "zip: " $NF " (" $5 " bytes)"}'
