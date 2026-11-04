#!/usr/bin/env python3
"""oot-ovl.py: prepara a "recarga" dos overlays do jogo (port/ovl_reset.c).

No N64 o codigo de cada ator, efeito, tela (gamestate) e do menu de pausa / do Link vem da ROM cada vez que e
carregado (Overlay_Load): as variaveis do arquivo (.data) voltam aos valores iniciais e as zeradas (.bss) voltam a
zero. O jogo conta com isso (ex.: o Zora do jogo de mergulho guarda "ja estou aqui" numa variavel do arquivo; sem a
recarga ele so aparecia na 1a visita). No port tudo fica ligado no executavel, entao este script:

  1. marca, em cada objeto de overlay (obj3/src_overlays_*.o, com .data/.bss ja renomeadas para game_data/game_bss
     pelo oot-build.sh), o comeco e o fim dos dados dele: simbolos __povl_N_ds/_de/_bs/_be (objcopy --add-symbol);
  2. gera GEN/port_ovl.c com a tabela: para cada ator / efeito / tela / overlay do kaleido, quais trechos sao dele.

uso: oot-ovl.py OOT OBJ GAME_FILES GEN       (variavel OBJCOPY = o objcopy da toolchain)
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elfmini  # noqa: E402

oot, objdir, game_files, gen = sys.argv[1:5]
objcopy = os.environ.get("OBJCOPY", "objcopy").split()


def table(path, pat_def, pat_other):
    """nomes dos overlays na ordem da tabela (None = entrada sem overlay)"""
    out = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("*") or line.startswith("/**") or line.startswith("//"):
            continue
        m = re.search(pat_def, line)
        if m:
            out.append(m.group(1))
        elif re.search(pat_other, line):
            out.append(None)
    return out


inc = os.path.join(oot, "include", "tables")
actors = table(os.path.join(inc, "actor_table.h"), r"\bDEFINE_ACTOR\((\w+),", r"\bDEFINE_ACTOR_(INTERNAL|UNSET)\(")
effects = table(os.path.join(inc, "effect_ss_table.h"), r"\bDEFINE_EFFECT_SS\((\w+),", r"\bDEFINE_EFFECT_SS_UNSET\(")
states = table(os.path.join(inc, "gamestate_table.h"), r"\bDEFINE_GAMESTATE\(\w+,\s*\w+,\s*(\w+)\)",
               r"\bDEFINE_GAMESTATE_INTERNAL\(")
kaleido = ["kaleido_scope", "player_actor"]  # (ordem de gKaleidoMgrOverlayTable)
if len(actors) < 400 or len(effects) < 30 or len(states) != 6:
    sys.exit("ERRO oot-ovl.py: tabelas do decomp com tamanho estranho (%d atores, %d efeitos, %d telas)"
             % (len(actors), len(effects), len(states)))

# arquivos de cada overlay (so os que o port compila)
ovl_files = {}
for f in open(game_files, encoding="utf-8").read().split():
    m = re.match(r"src/overlays/(actors|effects|gamestates|misc)/ovl_(\w+)/", f)
    if m:
        ovl_files.setdefault(m.group(2), []).append(f)

ranges = []      # (nome do overlay, tem data, tem bss)
first = {}       # overlay -> (indice do 1o trecho, quantos)
nfail = 0
for ovl in sorted(ovl_files):
    i0 = len(ranges)
    for f in ovl_files[ovl]:
        o = os.path.join(objdir, f.replace("/", "_")[:-2] + ".o")
        if not os.path.isfile(o):
            continue
        try:
            e = elfmini.Elf(o)
        except Exception as ex:  # noqa: BLE001
            print("AVISO oot-ovl.py: %s: %s" % (o, ex))
            continue
        sz = {s.name: s.size for s in e.sections}
        if ".data" in sz or ".bss" in sz:
            print("AVISO oot-ovl.py: %s ainda tem .data/.bss (nao foi para game_data)" % o)
        dsz, bsz = sz.get("game_data", 0), sz.get("game_bss", 0)
        if dsz == 0 and bsz == 0:
            continue
        k = len(ranges)
        args = []
        if dsz:
            args += ["--add-symbol", "__povl_%d_ds=game_data:0,global" % k,
                     "--add-symbol", "__povl_%d_de=game_data:%d,global" % (k, dsz)]
        if bsz:
            args += ["--add-symbol", "__povl_%d_bs=game_bss:0,global" % k,
                     "--add-symbol", "__povl_%d_be=game_bss:%d,global" % (k, bsz)]
        r = subprocess.run(objcopy + args + [o], capture_output=True, text=True)
        if r.returncode != 0:
            print("AVISO oot-ovl.py: objcopy falhou em %s: %s" % (o, r.stderr.strip()[:300]))
            nfail += 1
            continue
        ranges.append((ovl, dsz, bsz))
    if len(ranges) > i0:
        first[ovl] = (i0, len(ranges) - i0)

os.makedirs(gen, exist_ok=True)
with open(os.path.join(gen, "port_ovl.c"), "w", encoding="utf-8") as g:
    g.write("/* gerado por tools/oot-ovl.py: trechos de dados de cada overlay (port/ovl_reset.c) */\n")
    g.write("typedef struct { char *ds, *de, *bs, *be; } PortOvlRange;\n")
    for k, (ovl, dsz, bsz) in enumerate(ranges):
        if dsz:
            g.write("extern char __povl_%d_ds[], __povl_%d_de[];\n" % (k, k))
        if bsz:
            g.write("extern char __povl_%d_bs[], __povl_%d_be[];\n" % (k, k))
    g.write("const PortOvlRange port_ovl_ranges[] = {\n")
    for k, (ovl, dsz, bsz) in enumerate(ranges):
        d = "__povl_%d_ds, __povl_%d_de" % (k, k) if dsz else "0, 0"
        b = "__povl_%d_bs, __povl_%d_be" % (k, k) if bsz else "0, 0"
        g.write("    { %s, %s }, /* %s */\n" % (d, b, ovl))
    g.write("    { 0, 0, 0, 0 }\n};\n")
    g.write("const int port_ovl_nranges = %d;\n" % len(ranges))

    def tab(name, lst):
        g.write("/* %s: {1o trecho, quantos} por indice da tabela do jogo */\n" % name)
        g.write("const unsigned short %s[][2] = {\n" % name)
        for n in lst:
            a = first.get(n) if n else None
            g.write("    { %d, %d }, /* %s */\n" % ((a[0] if a else 0), (a[1] if a else 0), n or "-"))
        g.write("};\nconst int %s_n = %d;\n" % (name, len(lst)))

    tab("port_ovl_actors", actors)
    tab("port_ovl_effects", effects)
    tab("port_ovl_states", states)
    tab("port_ovl_kaleido", kaleido)

used = set(a for a in actors + effects + states + kaleido if a)
sem = sorted(o for o in ovl_files if o not in used)
print("oot-ovl.py: %d overlays com dados (%d trechos), %d KB de .data, %d KB de .bss%s"
      % (len(first), len(ranges), sum(r[1] for r in ranges) // 1024, sum(r[2] for r in ranges) // 1024,
         ("; objcopy falhou em %d" % nfail) if nfail else ""))
if sem:
    print("oot-ovl.py: overlays fora das tabelas (sem recarga): " + " ".join(sem))
