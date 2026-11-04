#!/usr/bin/env python3
"""oot-romdata-regioes.py: as regioes de dados do jogo dentro do executavel, para o tools/oot-romdata.c.

uso: python3 tools/oot-romdata-regioes.py MAPA EXE > regioes.txt

Do mapa da ligacao (oot_headless.map) saem as secoes de dados (game_data, .data*, .rodata*) dos arquivos do jogo
(obj3/*.o: o decomp, com os ajustes do port) e do gen/port_data.o (tabela de fontes das musicas, que vem da ROM).
Os arquivos do port (port/, platform/) ficam de fora: sao do port. O deslocamento de cada regiao no arquivo sai
dos cabecalhos de programa do EXE (o que vai ser publicado, ja sem a depuracao).
Linha: "endereco tamanho deslocamento nome" (hexadecimal).
"""
import re
import struct
import sys

mapf, exef = sys.argv[1], sys.argv[2]
d = open(exef, "rb").read()
if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
    sys.exit("ERRO: %s nao e um ELF de 32 bits little-endian" % exef)
phoff = struct.unpack_from("<I", d, 28)[0]
phentsize, phnum = struct.unpack_from("<HH", d, 42)
segs = []
for i in range(phnum):
    t, off, va, _, fs, _, _ = struct.unpack_from("<IIIIIII", d, phoff + i * phentsize)
    if t == 1:
        segs.append((va, off, fs))

num = r"0x[0-9a-fA-F]+"
started = False
pending = None
out = []
for line in open(mapf, errors="replace").read().split("\n"):
    if line.startswith("Linker script and memory map"):
        started = True
        continue
    if not started:
        continue
    m = re.match(r"^ (\S+)\s*$", line)
    if m and (m.group(1).startswith(".") or m.group(1).startswith("game_")):
        pending = m.group(1)
        continue
    sec = None
    m = re.match(r"^ (\S+)\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
    if m:
        sec, a, sz, obj = m.groups()
    elif pending:
        m = re.match(r"^\s+(%s)\s+(%s)\s+(\S.*)$" % (num, num), line)
        if m:
            sec = pending
            a, sz, obj = m.groups()
    pending = None
    if not sec or not (sec == "game_data" or sec.startswith(".data") or sec.startswith(".rodata")):
        continue
    obj = obj.strip()
    if "/obj3/" not in obj and not obj.endswith("/gen/port_data.o"):
        continue
    a, sz = int(a, 16), int(sz, 16)
    if a == 0 or sz == 0:
        continue
    fo = None
    for va, off, fs in segs:
        if va <= a and a + sz <= va + fs:
            fo = off + a - va
    if fo is None:
        sys.exit("ERRO: regiao fora do arquivo: %s %x" % (obj, a))
    out.append("%x %x %x %s:%s" % (a, sz, fo, obj.split("/")[-1], sec))
if len(out) < 100:
    sys.exit("ERRO: so %d regioes no mapa (o mapa e deste executavel?)" % len(out))
sys.stdout.write("\n".join(out) + "\n")
sys.stderr.write("%d regioes\n" % len(out))
