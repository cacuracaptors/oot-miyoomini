#!/usr/bin/env python3
# oot-fonte-latin1.py: acrescenta ao port/miyoo/font8x16.h as letras de U+00A0 a U+00FF (acentos do portugues e das
# outras linguas latinas), no mesmo desenho das ASCII que ja estao la. As letras acentuadas sao a letra ASCII do
# arquivo + um acento desenhado aqui (2 linhas; maiusculas nas linhas 0-1, minusculas nas 2-3; cedilha embaixo); os
# outros simbolos saem da DejaVu Sans Mono Bold (a mesma fonte das ASCII), em 8x16.
#   python3 tools/oot-fonte-latin1.py port/miyoo/font8x16.h
import re, sys, unicodedata
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
path = sys.argv[1]
src = open(path).read()
if "font_latin1" in src:
    src = src[:src.index("\n/* U+00A0")] + "\n"
ascii_rows = re.findall(r"\{ (0x[^}]*)\}", src)[:95]
ASC = [[int(x, 16) for x in r.split(",")] for r in ascii_rows]


def bits(rows):
    return [int(r.replace(".", "0").replace("#", "1"), 2) for r in rows]


ACC = {
    "̀": bits(["..##....", "...##..."]),  # grave
    "́": bits(["....##..", "...##..."]),  # agudo
    "̂": bits(["...##...", "..#..#.."]),  # circunflexo
    "̃": bits(["..##..#.", ".#..##.."]),  # til
    "̈": bits(["........", ".##..##."]),  # trema
    "̊": bits(["...##...", "...##..."]),  # anel
}
CED = bits(["....#...", "...##..."])  # cedilha (linhas 13-14)


def render(ch):
    f = ImageFont.truetype(FONT, 14)
    im = Image.new("L", (8, 16), 0)
    ImageDraw.Draw(im).text((0, 0), ch, font=f, fill=255)
    return [sum(0x80 >> x for x in range(8) if im.getpixel((x, y)) >= 128) for y in range(16)]


def glyph(cp):
    ch = chr(cp)
    dec = unicodedata.normalize("NFD", ch)
    if len(dec) == 2 and ord(dec[0]) < 127 and (dec[1] in ACC or dec[1] == "̧"):
        base = list(ASC[ord(dec[0]) - 32])
        upper = dec[0].isupper()
        if dec[0] in "ij":
            base[2] = base[3] = 0  # sem o pingo
        if dec[1] == "̧":
            base[13] |= CED[0]
            base[14] |= CED[1]
        else:
            y = 0 if upper else 2
            base[y] |= ACC[dec[1]][0]
            base[y + 1] |= ACC[dec[1]][1]
        return base
    if cp == 0xA0:
        return [0] * 16
    return render(ch)


out = ["\n/* U+00A0..U+00FF (Latin-1: acentos), geradas por tools/oot-fonte-latin1.py */",
       "static const unsigned char font_latin1[96][16] = {"]
for cp in range(0xA0, 0x100):
    g = glyph(cp)
    out.append("    { " + ", ".join("0x%02x" % b for b in g) + " }, /* U+%04X */" % cp)
out.append("};\n")
open(path, "w").write(src.rstrip("\n") + "\n" + "\n".join(out))
