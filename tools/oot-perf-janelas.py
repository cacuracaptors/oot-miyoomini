#!/usr/bin/env python3
"""oot-perf-janelas.py -- o perf_NN.bin em janelas de tempo: quadros por segundo e onde a thread de desenho gastou.

Roda NO WSL (o executavel tem de ser o que gravou a medicao):
  python3 ~/fallout-miyoo/oot-miyoo/tools/oot-perf-janelas.py perf_01.bin ~/fallout-miyoo/oot-arm/oot_headless [segundos]

Serve para comparar o trecho lento com o normal da mesma medicao (ex.: logo depois de ligar o filtro retro).
"""
import bisect
import collections
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rep", os.path.join(here, "oot-perf-report.py"))
rep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rep)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    win = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    threads, ev, sm, proc, hz, clk, tend = rep.read_perf(sys.argv[1])
    addrs, names = rep.symbols(sys.argv[2])
    tn = [n for n, _ in threads]
    libs = []
    import re
    for m in re.finditer(r"^([0-9a-f]+)-([0-9a-f]+) r-xp \S+ \S+ \S+\s+(\S+)$", proc, re.M):
        libs.append((int(m.group(1), 16), int(m.group(2), 16), os.path.basename(m.group(3))))

    def fn(pc):
        for lo, hi, nm_ in libs:
            if lo <= pc < hi and not nm_.startswith("oot"):
                return "[%s]" % nm_
        i = bisect.bisect_right(addrs, pc) - 1
        return names[i] if i >= 0 else "?"

    frames = sorted(e[0] for e in ev if e[1] == 61)
    marks = sorted(e[0] for e in ev if e[1] == 63)
    t0 = min([s[0] for s in sm] + frames[:1])
    t1 = max([s[0] for s in sm] + frames[-1:])
    want = ("desenho", "auxiliar", "tela")
    ids = {i: n for i, n in enumerate(tn) if n in want}
    print("janelas de %.1f s (t em segundos desde o comeco da medicao); marcas SELECT+R1: %s" %
          (win, ", ".join("%.1f" % ((m - t0) / 1e3) for m in marks) or "-"))
    a = t0
    while a < t1:
        b = a + win * 1e3
        fr = [f for f in frames if a <= f < b]
        dts = [fr[i] - fr[i - 1] for i in range(1, len(fr))]
        med = sorted(dts)[len(dts) // 2] if dts else 0.0
        line = "t=%6.1f  quadros %3d  mediana %5.1f ms |" % ((a - t0) / 1e3, len(fr), med)
        for tid, nm in ids.items():
            c = collections.Counter(fn(s[3]) for s in sm if s[1] == tid and a <= s[0] < b)
            tot = sum(c.values())
            top = ", ".join("%s %d" % (k, v) for k, v in c.most_common(5))
            line += "  %s(%d): %s |" % (nm, tot, top)
        print(line)
        a = b


main()
