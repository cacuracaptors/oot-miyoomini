#!/usr/bin/env python3
"""oot-assets.py: gera o MAPA DE CONVERSAO dos dados da ROM (big-endian -> endian nativo).

Ideia (a mesma do oot-dc e do romkit do Mario Kart 64):
  1. Os arquivos de assets extraidos da ROM (extracted/ntsc-1.1/assets/**/*.c) sao COMPILADOS para
     a maquina de destino (little-endian). O compilador produz os dados exatamente como o jogo
     compilado vai le-los (structs, u16, u32, f32...).
  2. Cada simbolo desses dados e comparado com os bytes da ROM descomprimida, no lugar onde o
     decomp o colocou (a posicao vem do ELF MIPS que o proprio decomp gerou).
  3. Para cada pedaco de 2 bytes classifica-se: igual (raw), trocado de 2 em 2 (swap2) ou
     trocado de 4 em 4 (swap4). Ponteiros (relocacoes) sao sempre swap4.
  4. O resultado e um mapa compacto (2 bits por meia-palavra) por arquivo da ROM. O mapa
     contem so ESTRUTURA, nenhum byte do jogo, e pode ir para o repositorio. No aparelho, o
     conversor da primeira execucao aplica o mapa ao cartucho feito da ROM do jogador.
  5. VERIFICACAO: o mapa e aplicado a ROM e o resultado e comparado com os bytes compilados.

Codigos por meia-palavra: 0 raw, 1 swap2, 2 = 1a metade de um swap4, 3 = 2a metade.

uso: oot-assets.py <decomp> <TREE> <OUT> [--cc "gcc -m32 ..."] [-j N] [--report arq.txt]
     (TREE = saida do oot-patch.py; OUT recebe oot_swapmap.bin e os objetos compilados)
"""
import argparse
import concurrent.futures as cf
import glob
import os
import shlex
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elfmini  # noqa: E402

MAGIC = b"OOTSWP1\0"
RAW, SW2, SW4A, SW4B = 0, 1, 2, 3

ap = argparse.ArgumentParser()
ap.add_argument("oot")
ap.add_argument("tree")
ap.add_argument("out")
ap.add_argument("--cc", default=os.environ.get("OOT_ASSET_CC",
                "gcc -m32 -malign-double -malign-data=abi"))
ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
ap.add_argument("--report", default=None)
ap.add_argument("--version", default="ntsc-1.1")
ap.add_argument("--rom", default=None, help="ROM descomprimida (padrao: baseroms/<v>/baserom-decompressed.z64)")
ap.add_argument("--elf", default=None, help="ELF MIPS do decomp (padrao: build/<v>/*.elf)")
ap.add_argument("--reuse", action="store_true", help="nao recompila objetos que ja existem e estao atualizados")
ap.add_argument("--limit", type=int, default=0, help="so os N primeiros arquivos (teste)")
args = ap.parse_args()

OOT, TREE, OUT = os.path.abspath(args.oot), os.path.abspath(args.tree), os.path.abspath(args.out)
V = args.version
BUILD = os.path.join(OOT, "build", V)
EXTR = os.path.join(OOT, "extracted", V)
os.makedirs(os.path.join(OUT, "assets_obj"), exist_ok=True)
rep = []


def log(s=""):
    print(s)
    rep.append(s)


# ------------------------------------------------------------------ entradas
rom_path = args.rom or os.path.join(OOT, "baseroms", V, "baserom-decompressed.z64")
if not os.path.isfile(rom_path):
    sys.exit("ERRO: nao achei a ROM descomprimida: %s\n  (e criada por 'make setup VERSION=%s' do decomp)" % (rom_path, V))
rom = open(rom_path, "rb").read()
if rom[:4] != b"\x80\x37\x12\x40":
    sys.exit("ERRO: %s nao esta em ordem z64 (big-endian)" % rom_path)

elf_path = args.elf
if not elf_path:
    cands = [p for p in glob.glob(os.path.join(BUILD, "*.elf")) if "reloc" not in os.path.basename(p)]
    if not cands:
        sys.exit("ERRO: nao achei o ELF MIPS em %s/*.elf (rode 'make' do decomp)" % BUILD)
    elf_path = max(cands, key=os.path.getsize)
log("ELF MIPS : " + elf_path)
log("ROM      : %s (%d bytes)" % (rom_path, len(rom)))
mips = elfmini.Elf(elf_path)
msyms = mips.symbols()
absval = {n: v for n, v, sz, t, b, sh in msyms}

# segmentos: secoes de saida "..nome" com conteudo
segs = {}  # nome -> dict(sec, rom)
for s in mips.sections:
    if s.name.startswith("..") and not s.name.endswith(".bss") and s.type == elfmini.SHT_PROGBITS:
        nm = s.name[2:]
        rs = absval.get("_%sSegmentRomStart" % nm)
        if rs is None:
            continue
        segs[nm] = dict(sec=s, rom=rs)
sec_to_seg = {v["sec"].idx: k for k, v in segs.items()}
log("segmentos com dados no ELF: %d" % len(segs))
if not segs:
    sys.exit("ERRO: nenhum segmento '..nome' no ELF (formato inesperado do linker script)")
mips_by_name = {}
for n, v, sz, t, b, sh in msyms:
    if sh in sec_to_seg and t in (0, 1):  # NOTYPE / OBJECT
        mips_by_name.setdefault(n, []).append((v, sz, sh))

# ------------------------------------------------------------------ lista de assets a compilar
F3DEX1_DIRS = ("assets/misc/z_select_static/",)
# soundfonts: arquivos gerados pelo 'sfc' com layout proprio (ligados por linker_scripts/soundfont.ld, simbolos
# viram offsets ABS). Nao entram neste mapa: o audio e convertido dentro do AudioLoad (fase do audio).
SKIP_DIRS = ("assets/audio/soundfonts/",)


objs_dir = os.path.join(BUILD, "assets")
cfiles = []
for root, _, files in os.walk(objs_dir):
    for fn in files:
        if not fn.endswith(".o"):
            continue
        rel = os.path.relpath(os.path.join(root, fn), BUILD)[:-2] + ".c"
        for base in (EXTR, OOT, BUILD):
            c = os.path.join(base, rel)
            if os.path.isfile(c):
                cfiles.append((rel, c))
                break
cfiles.sort()
nskip = sum(1 for rel, c in cfiles if rel.startswith(SKIP_DIRS))
cfiles = [(rel, c) for rel, c in cfiles if not rel.startswith(SKIP_DIRS)]
if args.limit:
    cfiles = cfiles[:args.limit]
log("arquivos de assets (.c) a compilar: %d  (soundfonts de audio ignorados: %d)" % (len(cfiles), nskip))

DEFS = ["-D_LANGUAGE_C", "-DCOMPILER_GCC", "-DNON_MATCHING", "-DAVOID_UB", "-DPLATFORM_N64=1",
        "-DPLATFORM_GC=0", "-DPLATFORM_IQUE=0", "-DOOT_VERSION=NTSC_1_1", "-DOOT_REVISION=1",
        "-DOOT_REGION=REGION_US", "-DLIBULTRA_VERSION=LIBULTRA_VERSION_I", "-DLIBULTRA_PATCH=1",
        "-DDEBUG_FEATURES=0", "-DNDEBUG", "-DF3DEX_GBI_2", "-DMML_VERSION=MML_VERSION_OOT",
        "-D_MIPS_ISA_MIPS2=2", "-D_MIPS_ISA=_MIPS_ISA_MIPS2", "-D_ABIO32=1", "-D_MIPS_SIM=_ABIO32",
        "-D_MIPS_SZINT=32", "-D_MIPS_SZLONG=32", "-D_MIPS_SZPTR=32"]
FLAGS = ["-O0", "-w", "-fno-strict-aliasing", "-fwrapv", "-nostdinc", "-fno-builtin", "-funsigned-char",
         "-std=gnu90", "-fno-common", "-fno-toplevel-reorder", "-fno-zero-initialized-in-bss",
         "-fno-section-anchors", "-fno-pic", "-fno-pie"]
INCS = ["-I" + TREE, "-I" + os.path.join(TREE, "include"), "-I" + os.path.join(TREE, "include/libc"),
        "-I" + BUILD, "-I" + OOT, "-I" + EXTR]
CC = shlex.split(args.cc)


def compile_one(item):
    rel, c = item
    o = os.path.join(OUT, "assets_obj", rel[:-2].replace("/", "_") + ".o")
    if args.reuse and os.path.exists(o) and os.path.getmtime(o) >= os.path.getmtime(c):
        return rel, o, 0, ""
    defs = DEFS
    if rel.startswith(F3DEX1_DIRS):  # o Makefile do decomp compila estes com F3DEX_GBI (microcodigo 1)
        defs = [d for d in DEFS if d != "-DF3DEX_GBI_2"] + ["-DF3DEX_GBI"]
    r = subprocess.run(CC + defs + FLAGS + INCS + ["-I" + os.path.dirname(c), "-c", c, "-o", o],
                       capture_output=True, text=True, cwd=OOT)
    return rel, o, r.returncode, r.stderr


# ------------------------------------------------------------------ compila
log("compilando para a maquina de destino: " + args.cc)
results = []
fails = []
done = 0
with cf.ThreadPoolExecutor(max_workers=args.j) as ex:
    for rel, o, rc, err in ex.map(compile_one, cfiles):
        done += 1
        if done % 200 == 0:
            print("  ... %d/%d" % (done, len(cfiles)), flush=True)
        if rc == 0:
            results.append((rel, o))
        else:
            fails.append((rel, err))
log("compilados: %d   com ERRO: %d" % (len(results), len(fails)))
for rel, err in fails[:8]:
    log("  ERRO em %s:\n    %s" % (rel, "\n    ".join(err.strip().splitlines()[:4])))

# ------------------------------------------------------------------ compara
# codes[seg] = bytearray (1 codigo por meia-palavra, 0 = raw) cobrindo o arquivo todo
codes = {}
for nm, v in segs.items():
    size = v["sec"].size
    codes[nm] = bytearray((size + 1) // 2)
# posicao de cada simbolo checado (para a verificacao): (seg, offset_no_segmento, bytes_arm, mascara_reloc)
checked = []
stat = dict(sym=0, matched=0, unmatched=0, bytes=0, conflicts=0, sizediff=0, rawonly=0)
unmatched = []
conflicts = []
conflict_dump = []
sec_bytes_cache = {}


def classify(l, b, relmask, al4):
    """l = bytes compilados (nativos), b = bytes da ROM; devolve (codigos, nconflitos).
    al4: o inicio do simbolo esta alinhado a 4 na ROM (so entao vale swap4)"""
    n = len(l)
    out = bytearray((n + 1) // 2)
    bad = 0
    i = 0
    while i < n:
        if al4 and i + 4 <= n and i % 4 == 0:
            l4, b4 = l[i:i + 4], b[i:i + 4]
            if relmask[i // 4] or (l4 != b4 and l4 == b4[::-1]):
                out[i // 2] = SW4A
                out[i // 2 + 1] = SW4B
                i += 4
                continue
            if l4 == b4:
                i += 4
                continue
        # meia-palavra
        if i + 2 <= n:
            l2, b2 = l[i:i + 2], b[i:i + 2]
            if l2 == b2:
                pass
            elif l2 == b2[::-1]:
                out[i // 2] = SW2
            else:
                bad += 1
            i += 2
        else:  # byte final
            if l[i] != b[i]:
                bad += 1
            i += 1
    return out, bad


for rel, o in results:
    try:
        arm = elfmini.Elf(o)
    except Exception as ex:
        log("  aviso: nao consegui ler %s: %s" % (o, ex))
        continue
    secs = {s.idx: s for s in arm.sections
            if s.type == elfmini.SHT_PROGBITS and (s.flags & elfmini.SHF_ALLOC) and not (s.flags & elfmini.SHF_EXEC)}
    relmasks = {}
    for idx, s in secs.items():
        m = bytearray((s.size + 3) // 4)
        for off in arm.relocs_for(s):
            if off // 4 < len(m):
                m[off // 4] = 1
        relmasks[idx] = m
    for name, val, size, typ, bind, shndx in arm.symbols():
        if shndx not in secs or size == 0 or typ not in (0, 1):
            continue
        stat["sym"] += 1
        cands = mips_by_name.get(name)
        if not cands:
            stat["unmatched"] += 1
            unmatched.append((rel, name, size))
            continue
        maddr, msize, mshndx = cands[0]
        seg = sec_to_seg[mshndx]
        msec = segs[seg]["sec"]
        off = maddr - msec.addr
        if off < 0 or off + size > msec.size:
            stat["unmatched"] += 1
            unmatched.append((rel, name, size))
            continue
        if msize and msize != size:
            stat["sizediff"] += 1
        s = secs[shndx]
        l = arm.section_bytes(s)[val:val + size]
        vrom = segs[seg]["rom"] + off
        b = rom[vrom:vrom + size]
        if len(b) != size:
            stat["unmatched"] += 1
            unmatched.append((rel, name, size))
            continue
        rm = relmasks[shndx]
        al4 = (vrom % 4 == 0) and (val % 4 == 0)
        # mascara de relocacao por palavra de 4 bytes DO SIMBOLO (so faz sentido alinhado)
        mask = [1 if (al4 and (val // 4 + k) < len(rm) and rm[val // 4 + k]) else 0
                for k in range((size + 3) // 4)]
        if vrom % 2:  # inicio impar: so pode ser u8 (raw)
            al4 = False
        cd, bad = classify(l, b, mask, al4)
        if bad:
            stat["conflicts"] += 1
            conflicts.append((rel, name, "%d meias-palavras nao batem (nem raw, nem swap)" % bad))
            if len(conflict_dump) < 40:
                d = ["  DETALHE %s (%s)  tam=%d vrom=%08X sym_off=%d al4=%s" % (name, rel, size, vrom, val, al4)]
                nshow = 0
                for k in range(0, size - 1, 2):
                    l2, b2 = l[k:k + 2], b[k:k + 2]
                    if l2 != b2 and l2 != b2[::-1]:
                        d.append("     +0x%03X  compilado=%s  rom=%s  reloc_palavra=%d"
                                 % (k, l2.hex(), b2.hex(), mask[k // 4] if k // 4 < len(mask) else -1))
                        nshow += 1
                        if nshow >= 8:
                            break
                d.append("     compilado[0:48]=" + l[:48].hex())
                d.append("     rom      [0:48]=" + b[:48].hex())
                conflict_dump.append("\n".join(d))
        c = codes[seg]
        o0 = off // 2
        for i, x in enumerate(cd):
            if x:
                c[o0 + i] = x
        stat["matched"] += 1
        stat["bytes"] += size
        checked.append((seg, off, l, mask, name))

log("")
log("simbolos de dados compilados : %d" % stat["sym"])
log("  com par no ELF MIPS        : %d  (%d bytes)" % (stat["matched"], stat["bytes"]))
log("  sem par no ELF MIPS        : %d" % stat["unmatched"])
log("  tamanho diferente do MIPS  : %d" % stat["sizediff"])
log("  CONFLITOS (nao classificados): %d" % len(conflicts))
for rel, name, why in conflicts[:60]:
    log("    %s : %s  -> %s" % (rel, name, why))
for d in conflict_dump:
    log(d)
for rel, name, size in unmatched[:15]:
    log("    sem par: %s : %s (%d bytes)" % (rel, name, size))

# cobertura por segmento
tot_seg = sum(v["sec"].size for v in segs.values())
nonraw = {nm: sum(1 for x in c if x) for nm, c in codes.items()}
log("segmentos com alguma troca de bytes: %d de %d" % (sum(1 for x in nonraw.values() if x), len(segs)))

# ------------------------------------------------------------------ aplica (igual ao conversor) e verifica
def apply_codes(data, cd):
    data = bytearray(data)
    n = len(data)
    i = 0
    while i < len(cd):
        c = cd[i]
        p = i * 2
        if c == SW2 and p + 2 <= n:
            data[p], data[p + 1] = data[p + 1], data[p]
        elif c == SW4A and i + 1 < len(cd) and cd[i + 1] == SW4B and p + 4 <= n:
            data[p:p + 4] = data[p:p + 4][::-1]
            i += 1
        i += 1
    return bytes(data)


verify_bad = 0
conv_cache = {}
for seg, off, l, mask, name in checked:
    if seg not in conv_cache:
        sz = segs[seg]["sec"].size
        conv_cache[seg] = apply_codes(rom[segs[seg]["rom"]:segs[seg]["rom"] + sz], codes[seg])
    got = conv_cache[seg][off:off + len(l)]
    # os slots de relocacao tem valor "0 + addend" no .o, entao so comparamos o resto
    ok = True
    for k in range(0, len(l), 4):
        if mask[k // 4]:
            continue
        if got[k:k + 4] != l[k:k + 4]:
            ok = False
            break
    if not ok:
        verify_bad += 1
        if verify_bad <= 10:
            log("    VERIFICACAO falhou: %s" % name)
log("verificacao (mapa aplicado a ROM == dados compilados): %s  (%d simbolos, %d falhas)"
    % ("OK" if verify_bad == 0 else "FALHOU", len(checked), verify_bad))

# ------------------------------------------------------------------ grava o mapa
body = bytearray()
nfiles = 0
for nm, v in sorted(segs.items(), key=lambda kv: kv[1]["rom"]):
    cd = codes[nm]
    if not any(cd):
        continue
    nh = len(cd)
    packed = bytearray((nh + 3) // 4)
    for i, x in enumerate(cd):
        packed[i // 4] |= x << (2 * (i % 4))
    body += struct.pack("<III", v["rom"], v["sec"].size, nh) + packed
    nfiles += 1
raw = struct.pack("<I", nfiles) + bytes(body)
comp = zlib.compressobj(9, zlib.DEFLATED, -15)
deflated = comp.compress(raw) + comp.flush()
mp = os.path.join(OUT, "oot_swapmap.bin")
with open(mp, "wb") as f:
    f.write(MAGIC + struct.pack("<II", len(raw), zlib.crc32(raw) & 0xFFFFFFFF) + deflated)
log("")
log("mapa: %s  (%d arquivos, %d bytes de dados, %d comprimidos)" % (mp, nfiles, len(raw), len(deflated)))

if args.report:
    with open(args.report, "w") as f:
        f.write("\n".join(rep) + "\n")
    print("relatorio: " + args.report)
sys.exit(0 if (verify_bad == 0) else 2)
