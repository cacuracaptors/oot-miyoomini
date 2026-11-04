#!/usr/bin/env python3
"""oot-idioma-irpara.py: acerta os rotulos "To ..." do menu de pausa no pacote de idioma portugues.

No menu de pausa, com o cursor nas setas de L/R, o jogo mostra embaixo o nome da pagina PARA ONDE a seta
leva ("To Map", "To Equipment"...). A traducao Zelda 64 BR tirou o "To": aparece so "Mapa", que parece o
nome da pagina errada. Aqui os 4 rotulos viram "Ir p/ Mapa" etc., montados com as proprias letras da
traducao (I de "Inventario", r e p de "Equipar") e uma barra desenhada no mesmo estilo.

uso: oot-idioma-irpara.py PACOTE.oot icon_item_nes_static(da ROM 1.1, extraido)   (reescreve o pacote)
Tambem usado pelo oot-idioma.py (funcao fix_labels) ao gerar o pacote do zero.
"""
import struct
import sys
import zlib

NES = 14  # icon_item_nes_static na dmadata
WORDS = {  # textura (ia8 128x16) -> deslocamento no arquivo
    "SelectItem": 0x4880, "Map": 0x5080, "QuestStatus": 0x5880, "Equipment": 0x6080,
}
EQUIP_SHORT = (0x3C00, 56)  # "Equipar" (de onde vem o r e o p)
INVENT = (0x4880, 128)      # "Inventario" (de onde vem o I)


def _cell(b, o, w, c0, c1):
    cols = range(max(0, c0 - 1), min(w, c1 + 1))
    return [[b[o + j * w + i] for i in cols] for j in range(16)], c0 - max(0, c0 - 1), c1 - c0


def _core_segments(b, o, w):
    core = [any((b[o + j * w + i] >> 4) >= 10 and (b[o + j * w + i] & 15) >= 10 for j in range(16)) for i in range(w)]
    segs, i = [], 0
    while i < w:
        if core[i]:
            j = i
            while j < w and core[j]:
                j += 1
            segs.append((i, j))
            i = j
        else:
            i += 1
    return segs


def _slash():
    core = [[0] * 10 for _ in range(16)]
    for r in range(2, 12):
        x = 2 + round((11 - r) * 4 / 9)
        core[r][x] = core[r][x + 1] = 1
    pts = [(jj, ii) for jj in range(16) for ii in range(10) if core[jj][ii]]
    g = [[0] * 10 for _ in range(16)]
    for j in range(16):
        for i in range(10):
            if core[j][i]:
                g[j][i] = 0xFF
                continue
            d = min(abs(j - jj) + abs(i - ii) for jj, ii in pts)
            g[j][i] = 0x0F if d == 1 else 0x0A if d == 2 else 0x04 if d == 3 else 0
    return g, 2, 4


def _word(b, o, w):
    cols = [i for i in range(w) if any(b[o + j * w + i] & 15 for j in range(16))]
    c0, c1 = cols[0], cols[-1] + 1
    return [[b[o + j * w + i] for i in range(c0, c1)] for j in range(16)], 0, c1 - c0


def _compose(parts, width=128):
    items, x = [], 0
    for p in parts:
        if p[0] == "sp":
            x += p[1]
            continue
        g, lead, cw = p
        items.append((x - lead, g))
        x += cw + 1
    start = width // 2 - x // 2
    A = [[0] * width for _ in range(16)]
    I = [[0] * width for _ in range(16)]
    for xo, g in items:
        for j in range(16):
            for i, v in enumerate(g[j]):
                X = start + xo + i
                if 0 <= X < width:
                    A[j][X] = max(A[j][X], v & 15)
                    I[j][X] = max(I[j][X], v >> 4)
    return bytes((I[j][i] << 4) | A[j][i] for j in range(16) for i in range(width))


def fix_labels(nes):
    """nes: icon_item_nes_static ja traduzido (bytes). Devolve com os 4 rotulos "Ir p/ ..."."""
    b = bytearray(nes)
    eq = _core_segments(b, *EQUIP_SHORT)   # E q u i p a r
    inv = _core_segments(b, *INVENT)       # I n v e n t a r i o
    if len(eq) < 7 or len(inv) < 1:
        raise SystemExit("rotulos do menu: as letras de 'Equipar'/'Inventario' nao estao onde eu esperava")
    gI = _cell(b, INVENT[0], INVENT[1], *inv[0])
    gr = _cell(b, EQUIP_SHORT[0], EQUIP_SHORT[1], *eq[6])
    gp = _cell(b, EQUIP_SHORT[0], EQUIP_SHORT[1], *eq[4])
    prefix = [gI, gr, ("sp", 3), gp, _slash(), ("sp", 3)]
    words = {k: _word(b, o, 128) for k, o in WORDS.items()}  # (antes de escrever por cima)
    for k, o in WORDS.items():
        b[o:o + 128 * 16] = _compose(prefix + [words[k]])
    return bytes(b)


def _rd(d, i):
    return struct.unpack("<I", d[i:i + 4])[0], i + 4


def main():
    pack_path, base_path = sys.argv[1:3]
    d = open(pack_path, "rb").read()
    base = open(base_path, "rb").read()
    hdr = 8 + 4 + 32 + 224
    i = hdr
    n, i = _rd(d, i)
    out = bytearray(d[:hdr]) + struct.pack("<I", n)
    done = False
    for _ in range(n):
        k, ln, nr = struct.unpack("<III", d[i:i + 12])
        i0 = i
        i += 12
        rr = []
        for _ in range(nr):
            s, l, c = struct.unpack("<III", d[i:i + 12])
            i += 12
            rr.append((s, d[i:i + l]))
            i += l + ((-l) % 4)
        if k != NES:
            out += d[i0:i]
            continue
        if len(base) != ln:
            raise SystemExit("icon_item_nes_static da ROM com tamanho %d, o pacote espera %d" % (len(base), ln))
        y = bytearray(base)
        for s, seg in rr:
            y[s:s + len(seg)] = seg
        y = fix_labels(bytes(y))
        # trechos de novo (mesma regra do oot-idioma.py)
        r = []
        for p in range(ln):
            if base[p] != y[p]:
                if r and p - r[-1][1] <= 16:
                    r[-1][1] = p + 1
                else:
                    r.append([p, p + 1])
        out += struct.pack("<III", k, ln, len(r))
        for s, e in r:
            seg = y[s:e]
            out += struct.pack("<III", s, e - s, zlib.crc32(base[s:e]) & 0xFFFFFFFF) + seg + b"\0" * ((-len(seg)) % 4)
        done = True
    out += d[i:]
    if not done:
        raise SystemExit("o pacote nao tem o arquivo %d (icon_item_nes_static)" % NES)
    open(pack_path, "wb").write(out)
    print("rotulos do menu acertados em", pack_path)


if __name__ == "__main__":
    main()
