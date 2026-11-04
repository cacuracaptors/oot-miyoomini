#!/usr/bin/env python3
"""Le o resultado da ligacao e escreve um relatorio resumido."""
import glob
import os
import re
import subprocess
import sys
from collections import defaultdict

out, report, nm = sys.argv[1], sys.argv[2], sys.argv[3]

lines = []
w = lines.append

fails = open(os.path.join(out, "compile_fail.txt")).read().split("\n")
fails = [f for f in fails if f.strip()]
w("== ARQUIVOS QUE NAO COMPILARAM: %d" % len(fails))
for f in fails[:80]:
    w("  " + f)
    name = f.split()[-1]
    # mostra a primeira linha de erro do log
    cand = glob.glob(os.path.join(out, "log3", "*" + os.path.basename(name).replace(".c", "") + "*"))
    for c in cand[:1]:
        for ln in open(c, errors="replace"):
            if "error" in ln:
                w("      " + ln.strip()[:220])
                break

link = open(os.path.join(out, "link.log"), errors="replace").read()
try:
    status = open(os.path.join(out, "link.status")).read().strip()
except OSError:
    status = "?"
w("")
w("== LIGACAO FINAL: %s" % ("OK (executavel gerado)" if status == "0" else "FALHOU (codigo %s)" % status))
try:
    for ln in open(os.path.join(out, "genld.txt"), errors="replace"):
        w("   " + ln.rstrip())
except OSError:
    pass
undef = set(re.findall(r"undefined reference to [`']([^'`]+)'", link))
multi = defaultdict(set)
for m in re.finditer(r"multiple definition of [`']([^'`]+)'", link):
    multi[m.group(1)] = 1
other = [l for l in link.split("\n")
         if l.strip() and "undefined reference" not in l and "multiple definition" not in l
         and "first defined here" not in l and "in function" not in l]

w("")
w("== SIMBOLOS NAO RESOLVIDOS: %d" % len(undef))
groups = defaultdict(list)
for n in sorted(undef):
    if n.startswith("os") or n.startswith("__os"):
        k = "os*"
    elif re.match(r"^g[A-Z]", n):
        k = "g*"
    elif n.endswith("Start") or n.endswith("End") or n.endswith("Size"):
        k = "*Start/End/Size"
    elif n.startswith("_"):
        k = "_*"
    elif n.startswith("D_"):
        k = "D_*"
    else:
        k = "outros"
    groups[k].append(n)
for k in sorted(groups):
    w("-- %s (%d)" % (k, len(groups[k])))
    w("   " + " ".join(groups[k][:300]))
    if len(groups[k]) > 300:
        w("   ... mais %d" % (len(groups[k]) - 300))

w("")
w("== DEFINICOES DUPLICADAS: %d" % len(multi))
w("   " + " ".join(sorted(multi)[:200]))

w("")
w("== OUTRAS MENSAGENS DO LIGADOR (primeiras 40)")
for l in other[:40]:
    w("   " + l[:220])

open(report, "w").write("\n".join(lines) + "\n")
print("\n".join(lines[:12]))
print("... (relatorio completo em %s)" % report)
