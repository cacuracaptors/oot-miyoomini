#!/bin/sh
# Teste do criador de cartucho + mapa de conversao com uma ROM sintetica (PC).
#   sh tests/cart_roundtrip.sh        (na pasta oot-miyoo)
set -e
D=$(mktemp -d)
HERE=$(pwd)
cp tests/cart_gen.py "$D/gen.py"
(cd "$D" && python3 gen.py)
gcc -O1 -g -Wall -std=gnu99 -no-pie -Iport -Wl,--defsym=_dmadataSegmentRomStart=0x1000 \
    -Wl,--defsym=_dmadataSegmentRomEnd=0x1060 tools/mkcart_main.c port/cart_build.c port/swapmap.c port/inflate.c \
    -o "$D/mkcart"
(cd "$D" && ./mkcart rom.z64 out.cart map.bin > /dev/null 2>&1 && python3 - <<'PY'
import struct
c = open("out.cart", "rb").read()
d = c[0x1000:0x1060]
e = [struct.unpack("<IIII", d[i:i + 16]) for i in range(0, 96, 16)]
A = c[e[1][2]:e[1][2] + 0x100]
B = c[e[3][2]:e[3][2] + 0x203]
assert A == open("exp_A.bin", "rb").read(), "A"
assert B == open("exp_B.bin", "rb").read(), "B"
print("cart_roundtrip: OK")
PY
)
rm -rf "$D"
