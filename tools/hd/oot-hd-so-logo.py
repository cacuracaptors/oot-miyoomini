#!/usr/bin/env python3
# oot-hd-so-logo.py -- deixa no texturas_hd.pak so as texturas da logo da abertura (o resto do pacote nao e mais
# usado desde 03/11/2026: libera espaco no cartao). Ficam as do object_mag (escudo, fogo, letras) menos a linha
# "(C) 1998 Nintendo" (gTitleCopyright1998Tex, que fica a original), e as letras do PRESS START (nes_font_static).
#
#   python3 oot-hd-so-logo.py texturas_hd.pak texturas_hd_logo.pak
import os
import struct
import sys

if len(sys.argv) != 3:
    sys.exit("uso: oot-hd-so-logo.py ENTRADA.pak SAIDA.pak")
ent_path, sai_path = sys.argv[1:3]

# enderecos (vrom) dos arquivos na NTSC 1.1 (dmadata da ROM descomprimida). O ntsc11_texturas.tsv nao serve
# aqui: a base dele e a posicao na ROM comprimida, e o pacote usa a vrom.
OBJECT_MAG = (0x1795000, 0x17B4440)
NES_FONT = (0x928000, 0x92C580)
COPYRIGHT = OBJECT_MAG[0] + 0x19300  # gTitleCopyright1998Tex: fica a original


def manter(v):
    return (OBJECT_MAG[0] <= v < OBJECT_MAG[1] and v != COPYRIGHT) or NES_FONT[0] <= v < NES_FONT[1]


d = open(ent_path, "rb").read()
if d[:8] != b"OOTHD01\0":
    sys.exit("ERRO: %s nao e um texturas_hd.pak" % ent_path)
n, _ = struct.unpack_from("<II", d, 8)
ESZ = 32
ents = []
for i in range(n):
    e = list(struct.unpack_from("<IIBBHHHHHIII", d, 16 + i * ESZ))
    if manter(e[0]):
        ents.append(e)
if not ents:
    sys.exit("ERRO: nenhuma textura da logo no pacote (nada foi gravado)")
hdr = b"OOTHD01\0" + struct.pack("<II", len(ents), 0)
pos = len(hdr) + ESZ * len(ents)
ind = bytearray()
blob = bytearray()
for e in ents:
    off, clen = e[9], e[10]
    dados = d[off:off + clen]
    e2 = e[:]
    e2[9] = pos + len(blob)
    ind += struct.pack("<IIBBHHHHHIII", *e2)
    blob += dados
with open(sai_path, "wb") as f:
    f.write(hdr)
    f.write(ind)
    f.write(blob)
print("%s: %d de %d texturas ficaram (%.1f MB -> %.2f MB)" % (sai_path, len(ents), n, len(d) / 1e6,
                                                             (len(hdr) + len(ind) + len(blob)) / 1e6))
