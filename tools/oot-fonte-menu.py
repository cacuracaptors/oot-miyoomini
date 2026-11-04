#!/usr/bin/env python3
"""oot-fonte-menu.py: gera port/miyoo/font_menu.h, a fonte do menu de opcoes (letras suavizadas, 26 pixels de
altura, largura de cada letra), a partir da DejaVu Sans Condensed Bold (licenca livre: Bitstream Vera / DejaVu).
Letras: ASCII 32..126 e Latin-1 160..255 (acentos do portugues).
   python3 tools/oot-fonte-menu.py [fonte.ttf] > port/miyoo/font_menu.h
"""
import sys
from PIL import Image, ImageDraw, ImageFont

ttf = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"
SIZE, H, TOP = 19, 26, 2
f = ImageFont.truetype(ttf, SIZE)
codes = list(range(32, 127)) + list(range(160, 256))
o = sys.stdout
o.write("/* gerado por tools/oot-fonte-menu.py (DejaVu Sans Condensed Bold %d px): fonte do menu de opcoes.\n" % SIZE)
o.write(" * Letras ASCII 32..126 e Latin-1 160..255; alfa 0..255, %d linhas por letra. */\n" % H)
o.write("#define FM_H %d\n" % H)
ws, offs, px = [], [], []
for c in codes:
    ch = chr(c)
    adv = int(round(f.getlength(ch)))
    w = max(adv, 1)
    im = Image.new("L", (w + 4, H), 0)
    ImageDraw.Draw(im).text((0, TOP), ch, font=f, fill=255)
    im = im.crop((0, 0, w, H))
    offs.append(len(px))
    ws.append(w)
    px.extend(im.tobytes())
o.write("static const unsigned char k_fm_w[%d] = {" % len(ws))
o.write(", ".join(str(x) for x in ws) + "};\n")
o.write("static const unsigned int k_fm_off[%d] = {" % len(offs))
o.write(", ".join(str(x) for x in offs) + "};\n")
o.write("static const unsigned char k_fm_px[%d] = {\n" % len(px))
for i in range(0, len(px), 32):
    o.write("    " + ",".join(str(x) for x in px[i:i + 32]) + ",\n")
o.write("};\n")
