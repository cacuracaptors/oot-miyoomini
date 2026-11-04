#!/usr/bin/env python3
"""oot-genld.py: gera o que ainda falta para o jogo ligar (usa o log da 1a ligacao).

Entradas
  OOT   pasta do decomp (com a pasta build/ntsc-1.1 ja compilada)
  GEN   pasta de saida (recebe oot-port-syms.ld e port_data.c)
  LOG   log do ligador (1a tentativa de ligacao)

Saidas
  GEN/oot-port-syms.ld   simbolos que o ligador precisa:
        * _ovl_*SegmentStart/End, _n64dd*, _codeSegment*  = 0   (tudo e ligado
          estaticamente: nao ha overlays para carregar)
        * _buffersSegmentEnd = inicio da heap do jogo dentro da RAM do N64
        * _dmadataSegmentStart = gDmaDataTable
        * Sequence_N_Size / Soundfont_N_Size / SampleBank_N_Size  (lidos dos .o MIPS)
  GEN/port_data.c        dados que no jogo original vinham de objetos MIPS:
        * gSequenceFontTable (tabela de fontes das sequencias, em endian nativo)
"""
import os
import re
import struct
import sys

oot, gen, logf = sys.argv[1], sys.argv[2], sys.argv[3]
build = os.path.join(oot, "build", "ntsc-1.1")

# inicio da heap do jogo dentro da RAM do N64 (0x80000000..0x80400000).
# O codigo/dados/BSS do jogo ficam NO executavel (nao na RAM do N64), entao a heap
# pode comecar quase no inicio da RAM; so deixamos uma folga.
HEAP_START = 0x80001000


# ------------------------------------------------------------ leitor de ELF32
def elf_read(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        raise ValueError("nao e ELF32: " + path)
    e = ">" if d[5] == 2 else "<"
    shoff, = struct.unpack_from(e + "I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", d, 0x2E)
    secs = []
    for i in range(shnum):
        f = struct.unpack_from(e + "IIIIIIIIII", d, shoff + i * shentsize)
        secs.append(dict(name=f[0], type=f[1], off=f[4], size=f[5], link=f[6]))

    def cstr(off):
        end = d.index(b"\0", off)
        return d[off:end].decode("latin-1")

    stroff = secs[shstrndx]["off"]
    for s in secs:
        s["name"] = cstr(stroff + s["name"])
    syms = {}
    for s in secs:
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        st = secs[s["link"]]["off"]
        for k in range(s["size"] // 16):
            n, val, size, info, other, shndx = struct.unpack_from(e + "IIIBBH", d, s["off"] + k * 16)
            if n == 0:
                continue
            syms[cstr(st + n)] = (val, size, shndx)
    return d, e, secs, syms


def elf_section_bytes(path, name):
    d, e, secs, _ = elf_read(path)
    for s in secs:
        if s["name"] == name:
            return d[s["off"]:s["off"] + s["size"]]
    raise KeyError("%s: sem secao %s" % (path, name))


# ------------------------------------------------------------ simbolos faltando
link = open(logf, errors="replace").read()
undef = set(re.findall(r"undefined reference to [`']([^'`]+)'", link))

lines = ["/* gerado por oot-genld.py: nao editar */"]
zero_re = re.compile(r"^(_ovl_\w+Segment(Start|End)|_n64dd\w+|_codeSegment(Start|BssStart|BssEnd))$")
zeros = sorted(n for n in undef if zero_re.match(n))
for n in zeros:
    lines.append("%s = 0;" % n)
# microcodigos do RSP (rspboot, cic6105, aspMain, F3DZEX2, S2DEX2, njpgdsp): no port o RSP e
# emulado em C (HLE), entao o que importa e cada um ter um ENDERECO PROPRIO, que o renderizador
# usa so para reconhecer qual microcodigo a tarefa pede. Ninguem le o conteudo.
UCODES = ["rspbootText", "cic6105Text", "aspMainText", "aspMainData",
          "gspF3DZEX2_NoN_fifoText", "gspF3DZEX2_NoN_fifoData",
          "gspF3DZEX2_NoN_PosLight_fifoText", "gspF3DZEX2_NoN_PosLight_fifoData",
          "gspS2DEX2d_fifoText", "gspS2DEX2d_fifoData", "njpgdspMainText", "njpgdspMainData"]
ucode_n = 0
for i, u in enumerate(UCODES):
    base = 0x1000 * (i + 1)
    size = 0x3E8 if u == "rspbootText" else 0x400
    for suffix, val in (("Start", base), ("End", base + size)):
        if (u + suffix) in undef:
            lines.append("%s%s = 0x%x;" % (u, suffix, val))
            ucode_n += 1
ucode_names = set(u + x for u in UCODES for x in ("Start", "End"))
if "_buffersSegmentEnd" in undef:
    lines.append("_buffersSegmentEnd = 0x%08x;" % HEAP_START)
if "_dmadataSegmentStart" in undef:
    lines.append("_dmadataSegmentStart = gDmaDataTable;")

# tamanhos dos dados de audio: sao simbolos absolutos dentro dos .o do build
want = set(n for n in undef if re.match(r"^(Sequence|Soundfont|SampleBank)_\d+_Size$", n))
found = {}
audio_dir = os.path.join(build, "assets", "audio")
if want:
    for root, _, files in os.walk(audio_dir):
        for fn in files:
            if not fn.endswith(".o"):
                continue
            path = os.path.join(root, fn)
            try:
                _, _, secs, syms = elf_read(path)
            except Exception:
                continue
            for n in want - set(found):
                if n in syms:
                    val, size, shndx = syms[n]
                    if shndx == 0xFFF1:  # SHN_ABS: o valor E o tamanho
                        found[n] = val
                    else:                # simbolo relativo a uma secao: usa o tamanho da secao
                        found[n] = secs[shndx]["size"]
for n in sorted(found):
    lines.append("%s = 0x%x;" % (n, found[n]))
missing_sizes = sorted(want - set(found))

os.makedirs(gen, exist_ok=True)
with open(os.path.join(gen, "oot-port-syms.ld"), "w") as f:
    f.write("\n".join(lines) + "\n")

# ------------------------------------------------------------ dados em C
c = ["/* gerado por oot-genld.py: nao editar */", ""]
fp = os.path.join(audio_dir, "sequence_font_table.o")
if os.path.exists(fp):
    data = bytearray(elf_section_bytes(fp, ".rodata"))
    n = struct.unpack_from(">H", data, 0)[0] // 2  # primeiro deslocamento = tamanho do indice
    for i in range(n):
        struct.pack_into("<H", data, i * 2, struct.unpack_from(">H", data, i * 2)[0])
    c.append("/* %d sequencias; indice de u16 em endian nativo, o resto sao bytes */" % n)
    c.append("const unsigned char gSequenceFontTable[%d] __attribute__((aligned(16))) = {" % len(data))
    for i in range(0, len(data), 16):
        c.append("    " + ",".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    c.append("};")
    seq_info = "%d sequencias, %d bytes" % (n, len(data))
else:
    seq_info = "NAO ENCONTRADO: " + fp
with open(os.path.join(gen, "port_data.c"), "w") as f:
    f.write("\n".join(c) + "\n")

print("genld: %d simbolos zerados, %d microcodigos, %d tamanhos de audio, gSequenceFontTable (%s)"
      % (len(zeros), ucode_n, len(found), seq_info))
if missing_sizes:
    print("genld: AVISO tamanhos nao achados: " + " ".join(missing_sizes))
other = sorted(n for n in undef if not zero_re.match(n) and n not in want and n not in ucode_names
               and n not in ("_buffersSegmentEnd", "_dmadataSegmentStart", "gSequenceFontTable"))
print("genld: ainda nao resolvidos depois disto: %d" % len(other))
