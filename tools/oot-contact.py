#!/usr/bin/env python3
"""oot-contact.py: junta os quadros PNG gravados pelo oot_headless numa unica imagem (folha de contato).
uso: oot-contact.py PASTA SAIDA.png [colunas=4] [metade=1]
   metade=1 reduz cada quadro para 160x120 (0 deixa 320x240).
So usa a biblioteca padrao do Python."""
import glob
import os
import struct
import sys
import zlib


def read_png(path):
    d = open(path, "rb").read()
    pos = 8
    idat = b""
    w = h = 0
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    rows = []
    for y in range(h):
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def half(w, h, rows):
    """reduz para a metade pegando 1 pixel de cada 2x2 (rapido, sem laco por pixel)"""
    out = []
    for y in range(0, h - 1, 2):
        r = rows[y]
        o = bytearray((w // 2) * 3)
        o[0::3] = r[0:(w // 2) * 6:6]
        o[1::3] = r[1:(w // 2) * 6:6]
        o[2::3] = r[2:(w // 2) * 6:6]
        out.append(bytes(o))
    return w // 2, h // 2, out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    folder, out = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    shrink = (sys.argv[4] != "0") if len(sys.argv) > 4 else True
    files = sorted(glob.glob(os.path.join(folder, "*.png")))
    if not files:
        sys.exit("nenhum PNG em " + folder)
    imgs = []
    for f in files:
        try:
            w, h, rows = read_png(f)
            if len(rows[0]) != w * 3:
                raise ValueError("formato diferente")
        except Exception as e:  # PNG de outro formato: ignora
            print("ignorando %s (%s)" % (f, e))
            continue
        if shrink:
            w, h, rows = half(w, h, rows)
        imgs.append((w, h, rows))
    if not imgs:
        sys.exit("nenhum PNG compativel")
    w, h = imgs[0][0], imgs[0][1]
    nrows = (len(imgs) + cols - 1) // cols
    gap = 2
    W = cols * w + (cols - 1) * gap
    H = nrows * h + (nrows - 1) * gap
    blank = b"\x40\x40\x40" * W
    sheet = [bytearray(blank) for _ in range(H)]
    for i, (iw, ih, rows) in enumerate(imgs):
        cx, cy = (i % cols) * (w + gap), (i // cols) * (h + gap)
        for y in range(ih):
            sheet[cy + y][cx * 3:(cx + iw) * 3] = rows[y]
    write_png(out, W, H, [bytes(r) for r in sheet])
    print("%d quadros -> %s (%dx%d)" % (len(imgs), out, W, H))


main()
