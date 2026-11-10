#!/bin/sh
# Monta o zip do release, SEM dados do jogo (roda no WSL, depois de sh tools/oot-build.sh):
#   sh tools/oot-release.sh 1.0.0
#
# 1. copia o executavel do build ARM, tira a depuracao e marca como "release" (port/romdata.c)
# 2. faz um cartucho (oot.cart) com a ROM americana 1.1 e o mapa de conversao deste build, como o jogo faz no aparelho
# 3. tools/oot-romdata.c: acha no executavel os dados que vem da ROM (regioes do mapa da ligacao,
#    tools/oot-romdata-regioes.py), zera e escreve a receita oot_dados.rcp (de onde vem cada pedaco no cartucho);
#    confere refazendo tudo
# 4. pasta do cartao SD (tools/oot-package.sh) com a receita, idiomas/pt.oot, texturas_hd.pak so com a logo e a
#    imagem do atalho; zip na Area de Trabalho, com o relatorio
#
# Precisa: ~/fallout-miyoo/oot-arm (build com oot_headless.map), a ROM americana 1.1 (em ~/fallout-miyoo/oot-i386 ou o
# .zip na Area de Trabalho), ~/fallout-miyoo/oot-arm/texturas_hd.pak (copiado do aparelho) e
# a imagem do atalho em dist/ (dist/Zelda - Ocarina of Time.png, no repositorio).
VER=$1
[ -n "$VER" ] || { echo "uso: sh tools/oot-release.sh VERSAO (ex.: 1.0.0)"; exit 1; }
ROOT=${ROOT:-$HOME/fallout-miyoo}
OUT=${OUT:-$ROOT/oot-arm}
PORT="$(cd "$(dirname "$0")/.." && pwd)"
TC=${TC:-$ROOT/miyoomini-toolchain/bin/arm-linux-gnueabihf}
DESK=${DESK:-/mnt/c/Users/Régis/Desktop}
W=$OUT/release
rm -rf "$W" && mkdir -p "$W" || exit 1

[ "$(cat "$OUT/link.status" 2>/dev/null)" = 0 ] || { echo "ERRO: compile antes (sh tools/oot-build.sh)"; exit 1; }
[ -f "$OUT/oot_headless.map" ] || { echo "ERRO: falta $OUT/oot_headless.map (compile de novo com o oot-build.sh novo)"; exit 1; }
[ -f "$OUT/texturas_hd.pak" ] || { echo "ERRO: falta $OUT/texturas_hd.pak (copie do aparelho)"; exit 1; }
IMG="$PORT/dist/Zelda - Ocarina of Time.png"
[ -f "$IMG" ] || { echo "ERRO: falta $IMG"; exit 1; }

echo "[1/5] executavel"
"$TC-objcopy" --strip-debug "$OUT/oot_headless" "$W/oot_full" || exit 1
python3 - "$W/oot_full" <<'PY' || exit 1
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
n = d.count(b"OOTDEV1\0")
if n != 1:
    sys.exit("ERRO: marca OOTDEV1 achada %d vezes" % n)
i = d.find(b"OOTDEV1\0")
d[i:i + 8] = b"OOTREL1\0"
open(p, "wb").write(d)
PY

echo "[2/5] cartucho de referencia (ROM americana 1.1)"
ROM=$(ls "$ROOT"/oot-i386/*.n64 "$ROOT"/oot-i386/*.z64 "$ROOT"/oot-i386/*.v64 2>/dev/null | head -1)
if [ -z "$ROM" ]; then
    ZIP="$DESK/Legend of Zelda, The - Ocarina of Time (USA) (Rev A).zip"
    [ -f "$ZIP" ] || { echo "ERRO: nao achei a ROM americana 1.1"; exit 1; }
    python3 - "$ZIP" "$W/rom.n64" <<'PY' || exit 1
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
n = [i for i in z.namelist() if i.lower().endswith((".n64", ".z64", ".v64"))][0]
open(sys.argv[2], "wb").write(z.read(n))
PY
    ROM="$W/rom.n64"
fi
DS=$("$TC-nm" "$OUT/oot_headless" | awk '$3=="_dmadataSegmentRomStart"{print $1}')
DE=$("$TC-nm" "$OUT/oot_headless" | awk '$3=="_dmadataSegmentRomEnd"{print $1}')
gcc -O2 -no-pie -I"$PORT/port" "$PORT/tools/mkcart_main.c" "$PORT/port/cart_build.c" "$PORT/port/swapmap.c" \
    "$PORT/port/inflate.c" -Wl,--defsym,_dmadataSegmentRomStart=0x$DS -Wl,--defsym,_dmadataSegmentRomEnd=0x$DE \
    -o "$W/mkcart" || exit 1
"$W/mkcart" "$ROM" "$W/oot.cart" "$OUT/oot_swapmap.bin" 2> "$W/mkcart.log" || { tail -3 "$W/mkcart.log"; exit 1; }

echo "[3/5] dados do jogo -> receita"
python3 "$PORT/tools/oot-romdata-regioes.py" "$OUT/oot_headless.map" "$W/oot_full" > "$W/regioes.txt" || exit 1
gcc -O2 -o "$W/oot-romdata" "$PORT/tools/oot-romdata.c" || exit 1
"$W/oot-romdata" make "$W/oot.cart" "$W/oot_full" "$W/regioes.txt" "$W/oot_dados.rcp" "$W/oot" "$W/relatorio.txt" || exit 1

echo "[4/5] texturas da logo"
python3 "$PORT/tools/hd/oot-hd-so-logo.py" "$OUT/texturas_hd.pak" "$W/texturas_hd.pak" || exit 1

echo "[5/5] pasta do cartao e zip"
VERSION="$VER" P="$W/pacote" EXE="$W/oot" RCP="$W/oot_dados.rcp" PAK="$W/texturas_hd.pak" IMG="$IMG" OUT="$OUT" ROOT="$ROOT" \
    sh "$PORT/tools/oot-package.sh" > "$W/pacote.log" || { cat "$W/pacote.log"; exit 1; }
ZIPOUT="$DESK/ZeldaOoT-miyoomini-v$VER.zip"
rm -f "$ZIPOUT"
(cd "$W/pacote" && python3 - "$ZIPOUT" <<'PY') || exit 1
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=9)
for base, dirs, files in os.walk("."):
    dirs.sort()
    for f in sorted(files):
        path = os.path.join(base, f)
        info = zipfile.ZipInfo.from_file(path, os.path.normpath(path))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, open(path, "rb").read())
    if not files and not dirs:
        z.writestr(os.path.normpath(base) + "/", "")
z.close()
PY
cp "$W/relatorio.txt" "$DESK/ZeldaOoT-miyoomini-v$VER-relatorio.txt"
echo "zip: $ZIPOUT"
ls -l "$ZIPOUT"
