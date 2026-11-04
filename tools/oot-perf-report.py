#!/usr/bin/env python3
"""oot-perf-report.py -- transforma o perf_NN.bin (opcao perf_trace do jogo) numa pagina HTML.

Roda NO WSL:
  python3 ~/fallout-miyoo/oot-miyoo/tools/oot-perf-report.py perf_00.bin ~/fallout-miyoo/oot-arm/oot_headless [saida.html]

O executavel tem de ser exatamente o que gravou a medicao (os enderecos das amostras viram nomes de funcao pelo
nm do toolchain do Miyoo). Saida padrao: Desktop/oot_perf.html (e um resumo em texto ao lado, oot_perf.txt).

O que a pagina mostra:
  - resumo por thread: tempo de CPU por segundo (pelas amostras), nucleo onde rodou, trocas de contexto forcadas;
  - cada nucleo: ocupado / ocioso / interrupcoes (do /proc/stat) e quem o ocupou (amostras);
  - outros processos do aparelho que usaram CPU durante a medicao;
  - quadros: tempo entre um desenho e o seguinte, e para os mais lentos o que cada thread fez naquele intervalo;
  - funcoes mais caras de cada thread (no geral e so nos quadros lentos);
  - linha do tempo: os 2 nucleos e cada thread, com zoom (arraste a barra / roda do mouse).
"""
import bisect
import collections
import html
import json
import os
import re
import struct
import subprocess
import sys

CODES = {
    2: "jogo: thread do N64", 4: "jogo: ocioso (esperando)", 10: "tarefa de desenho", 12: "tarefa de som",
    14: "tarefa JPEG", 20: "rasterizando", 22: "esperando a auxiliar", 24: "pegando linhas da auxiliar",
    30: "auxiliar rasterizando", 32: "auxiliar dormindo", 40: "tela: mostrando imagem", 42: "tela: esperando vsync",
    44: "tela: 2D pelo acelerador", 50: "som: preparando bloco", 70: "fundo JPEG pelo acelerador", 80: "DMA do cartucho",
    90: "carregando textura nova",
}
COLORS = {
    2: "#4e79a7", 4: "#d9d9d9", 10: "#59a14f", 12: "#edc948", 14: "#b07aa1", 20: "#e15759", 22: "#bab0ac",
    24: "#ff9da7", 30: "#f28e2b", 32: "#eeeeee", 40: "#76b7b2", 42: "#cfe8e6", 44: "#9c755f", 50: "#edc948",
    70: "#9c755f", 80: "#8cd17d", 90: "#b6992d",
}


def find_tool(name):
    base = os.path.expanduser("~/fallout-miyoo/miyoomini-toolchain")
    for root, _dirs, files in os.walk(base):
        if "arm-linux-gnueabihf-" + name in files:
            return os.path.join(root, "arm-linux-gnueabihf-" + name)
        if root.count(os.sep) - base.count(os.sep) > 3:
            continue
    return name


def symbols(exe):
    nm = find_tool("nm")
    try:
        out = subprocess.run([nm, "-n", "--defined-only", exe], capture_output=True, text=True).stdout
    except OSError:
        return [], []
    addrs, names = [], []
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[1] in "tT" and not p[2].startswith("$"):
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def read_perf(path):
    d = open(path, "rb").read()
    if d[:8] != b"OOTPERF1":
        raise SystemExit("ERRO: %s nao e um perf_NN.bin" % path)
    ver, nth, ne, ns, nproc, hz, clk, tend = struct.unpack_from("<8I", d, 8)
    o = 40
    threads = []
    for i in range(nth):
        nm = d[o:o + 24].split(b"\0")[0].decode("latin-1")
        sys_tid, = struct.unpack_from("<i", d, o + 24)
        threads.append((nm, sys_tid))
        o += 28
    ev = []
    for i in range(ne):
        t, code, tid, cpu, arg = struct.unpack_from("<QHBBi", d, o)
        ev.append((t / 1e6, code, tid, cpu, arg))  # ms
        o += 16
    sm = []
    for i in range(ns):
        tus, tid, cpu, pc, lr = struct.unpack_from("<IBB2xII", d, o)
        sm.append((tus / 1e3, tid, cpu, pc, lr))
        o += 16
    proc = d[o:o + nproc].decode("latin-1", "replace")
    return threads, ev, sm, proc, hz, clk, tend / 1e3


def spans_of(ev, nthreads):
    """pares comeco/fim por thread -> (tid, code, t0, t1, arg); o jogo vira a faixa 'N64' pelas trocas"""
    open_ = [dict() for _ in range(nthreads)]
    out = []
    game = []
    for t, code, tid, cpu, arg in sorted(ev, key=lambda e: e[0]):
        if code in (2, 4, 5):
            game.append((t, code, arg))
            continue
        if code in (61, 63):
            continue
        if tid >= nthreads:
            continue
        if code % 2 == 0:
            open_[tid].setdefault(code, []).append((t, arg, cpu))
        else:
            st = open_[tid].get(code - 1)
            if st:
                t0, a, c = st.pop()
                out.append((tid, code - 1, t0, t, a, c))
    # faixa do jogo: cada troca vale ate a proxima
    gl = []
    for i, (t, code, arg) in enumerate(game):
        t1 = game[i + 1][0] if i + 1 < len(game) else t
        if code == 2:
            gl.append((t, t1, "N64 t%d" % arg, 2))
        elif code == 4:
            gl.append((t, t1, "ocioso", 4))
    return out, gl


def parse_proc(proc, clk):
    """/proc/stat por nucleo, ctx por thread, outros processos -> resumos"""
    cores = collections.defaultdict(list)  # cpu -> [(t, user, nice, sys, idle, iowait, irq, softirq)]
    ctx = collections.defaultdict(list)    # tid -> [(t, vol, nonvol)]
    tasks = collections.defaultdict(list)  # tid -> [(t, name, utime, stime, cpu)]
    pids = collections.defaultdict(list)   # pid -> [(t, name, utime, stime)]
    cur, t = None, 0.0
    for line in proc.splitlines():
        if line.startswith("@"):
            p = line.split()
            cur = p[0][1:]
            if cur == "ctx" and len(p) >= 4:
                ctx[int(p[1])].append((t, int(p[2]), int(p[3])))
                cur = None
            elif len(p) >= 2:
                t = int(p[1]) / 1e3
            continue
        if cur == "stat" and re.match(r"cpu\d", line):
            p = line.split()
            cores[int(p[0][3:])].append((t,) + tuple(int(x) for x in p[1:8]))
        elif cur in ("task", "pid"):
            m = re.match(r"(\d+) \((.*)\) (.*)", line)
            if not m:
                continue
            f = m.group(3).split()
            ut, st, last_cpu = int(f[11]), int(f[12]), int(f[36]) if len(f) > 36 else -1
            if cur == "task":
                tasks[int(m.group(1))].append((t, m.group(2), ut, st, last_cpu))
            else:
                pids[int(m.group(1))].append((t, m.group(2), ut, st))
    return cores, ctx, tasks, pids


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    perf, exe = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else "/mnt/c/Users/Régis/Desktop/oot_perf.html"
    threads, ev, sm, proc, hz, clk, tend = read_perf(perf)
    nth = len(threads)
    tnames = [n for n, _ in threads]
    addrs, names = symbols(exe)

    # bibliotecas (/proc/self/maps gravado junto): amostras fora do executavel viram "biblioteca"
    libs = []
    for m in re.finditer(r"^([0-9a-f]+)-([0-9a-f]+) r-xp \S+ \S+ \S+\s+(\S+)$", proc, re.M):
        libs.append((int(m.group(1), 16), int(m.group(2), 16), os.path.basename(m.group(3))))
    text_end = addrs[-1] + 0x10000 if addrs else 0

    def fn(pc):
        for lo, hi, nm_ in libs:
            if lo <= pc < hi and not nm_.startswith("oot"):
                return "[%s]" % nm_
        i = bisect.bisect_right(addrs, pc) - 1
        if i < 0 or pc > text_end:
            return "?%08x" % pc
        return names[i]

    spans, game = spans_of(ev, nth)
    t_lo = min([e[0] for e in ev] + [s[0] for s in sm] + [0.0])
    t_hi = max([e[0] for e in ev] + [s[0] for s in sm] + [1.0])
    dur = max(t_hi - t_lo, 1e-3)
    txt = []

    def P(s=""):
        txt.append(s)

    P("Medicao: %s, %.1f s, %d eventos, %d amostras (%d por s de CPU por thread)" % (os.path.basename(perf), dur / 1e3, len(ev), len(sm), hz))

    # ---- threads: CPU (amostras), nucleos
    per_t = collections.Counter(s[1] for s in sm)
    per_tc = collections.Counter((s[1], s[2]) for s in sm)
    cores, ctx, tasks, pids = parse_proc(proc, clk)
    sysid = {i: threads[i][1] for i in range(nth)}
    P("")
    P("== Threads (CPU pelas amostras; trocas de contexto pelo /proc) ==")
    P("%-14s %8s %7s  %-22s %10s %10s" % ("thread", "ms/s", "%nucleo", "nucleos (amostras)", "trocas vol", "forcadas"))
    rows_t = []
    def proc_ms(i):
        l = tasks.get(sysid[i], [])
        if len(l) < 2:
            return None
        u = (l[-1][2] + l[-1][3]) - (l[0][2] + l[0][3])
        sp = max(l[-1][0] - l[0][0], 1)
        return u * 1000.0 / clk / (sp / 1e3)

    # (o relogio de CPU por thread do kernel do aparelho dispara a cada tick, ~100 Hz, nao a PT_HZ: o tempo de cada
    # thread vem do /proc; as amostras dao so a divisao por funcao e por nucleo)
    tot_s = sum(per_t.values())
    tot_p = sum(v for v in (proc_ms(i) for i in range(nth)) if v)
    if tot_s and tot_p:
        hz = tot_s / (tot_p / 1000.0 * (dur / 1e3))
        P("(amostras efetivas: ~%.0f por segundo de CPU)" % hz)
    for i in range(nth):
        pm = proc_ms(i)
        ms_s = pm if pm is not None else per_t[i] * (1000.0 / hz) / (dur / 1e3)
        cs = ", ".join("%d:%d%%" % (c, 100 * per_tc[(i, c)] // max(per_t[i], 1)) for c in sorted({c for (ti, c) in per_tc if ti == i}))
        cx = ctx.get(sysid[i], [])
        vol = cx[-1][1] - cx[0][1] if len(cx) > 1 else 0
        nv = cx[-1][2] - cx[0][2] if len(cx) > 1 else 0
        rows_t.append((tnames[i], ms_s, cs, vol, nv))
        P("%-14s %8.1f %6.1f%%  %-22s %10d %10d" % (tnames[i], ms_s, ms_s / 10.0, cs, vol, nv))

    # ---- nucleos (/proc/stat)
    P("")
    P("== Nucleos (/proc/stat no periodo) ==")
    rows_c = []
    for c in sorted(cores):
        a, b = cores[c][0], cores[c][-1]
        d = [b[k] - a[k] for k in range(1, 8)]
        tot = max(sum(d), 1)
        user, nice, sy, idle, iow, irq, sirq = d
        rows_c.append((c, 100.0 * (user + nice) / tot, 100.0 * sy / tot, 100.0 * (irq + sirq) / tot, 100.0 * (idle + iow) / tot))
        P("nucleo %d: usuario %.1f%%  sistema %.1f%%  interrupcoes %.1f%%  ocioso %.1f%%" % rows_c[-1])
    per_c = collections.Counter(s[2] for s in sm)
    for c in sorted(per_c):
        who = collections.Counter(tnames[s[1]] if s[1] < nth else "?" for s in sm if s[2] == c)
        P("  nucleo %d (amostras do jogo): %s" % (c, ", ".join("%s %.0f ms/s" % (k, v * 1000.0 / hz / (dur / 1e3)) for k, v in who.most_common(8))))

    # ---- outros processos
    P("")
    P("== Outros processos que usaram CPU (ms por segundo) ==")
    rows_p = []
    for pid, lst in pids.items():
        if len(lst) < 2:
            continue
        used = (lst[-1][2] + lst[-1][3]) - (lst[0][2] + lst[0][3])
        span = max(lst[-1][0] - lst[0][0], 1)
        if used > 0:
            rows_p.append((lst[-1][1], pid, used * 1000.0 / clk / (span / 1e3)))
    rows_p.sort(key=lambda r: -r[2])
    for name, pid, ms in rows_p[:15]:
        P("  %-20s pid %-6d %6.1f ms/s" % (name, pid, ms))
    if not rows_p:
        P("  (nenhum)")

    # ---- quadros
    frames = sorted(e[0] for e in ev if e[1] == 61)
    fr = []
    for i in range(1, len(frames)):
        fr.append((frames[i - 1], frames[i]))
    P("")
    P("== Quadros (intervalo entre desenhos terminados) ==")
    if fr:
        ds = sorted(b - a for a, b in fr)
        P("%d desenhos; mediana %.1f ms, 90%% ate %.1f ms, pior %.1f ms" % (len(ds), ds[len(ds) // 2], ds[int(len(ds) * 0.9)], ds[-1]))
    # o que cada thread fez em cada quadro (eventos) e onde gastou CPU (amostras)
    sp_by = collections.defaultdict(list)
    for tid, code, a, b, arg, cpu in spans:
        sp_by[(tid, code)].append((a, b))

    def busy(lst, a, b):
        s = 0.0
        for x, y in lst:
            if y > a and x < b:
                s += min(y, b) - max(x, a)
        return s

    worst = sorted(fr, key=lambda ab: -(ab[1] - ab[0]))[:12]
    sm_t = [s[0] for s in sm]
    rows_f = []
    for a, b in worst:
        P("")
        P("-- quadro de %.1f ms (em %.2f s):" % (b - a, a / 1e3))
        parts = []
        for (tid, code), lst in sorted(sp_by.items()):
            v = busy(lst, a, b)
            if v >= 0.3:
                parts.append("%s/%s %.1f" % (tnames[tid] if tid < nth else "?", CODES.get(code, code), v))
        gv = collections.defaultdict(float)
        for x, y, lab, code in game:
            if y > a and x < b:
                gv[lab] += min(y, b) - max(x, a)
        P("   eventos: " + "; ".join(parts))
        P("   jogo: " + ", ".join("%s %.1f" % (k, v) for k, v in sorted(gv.items(), key=lambda kv: -kv[1])[:6]))
        i0, i1 = bisect.bisect_left(sm_t, a), bisect.bisect_right(sm_t, b)
        cpu_t = collections.Counter()
        fn_t = collections.defaultdict(collections.Counter)
        for s in sm[i0:i1]:
            cpu_t[(s[1], s[2])] += 1
            fn_t[s[1]][fn(s[3])] += 1
        P("   CPU (amostras, ms): " + ", ".join("%s@n%d %.1f" % (tnames[t] if t < nth else "?", c, v * 1000.0 / hz) for (t, c), v in cpu_t.most_common(10)))
        for t, cnt in sorted(fn_t.items(), key=lambda kv: -sum(kv[1].values()))[:4]:
            P("     %s: %s" % (tnames[t] if t < nth else "?", ", ".join("%s %.1f" % (k, v * 1000.0 / hz) for k, v in cnt.most_common(6))))
        rows_f.append((a, b))

    # ---- funcoes por thread (geral)
    P("")
    P("== Funcoes mais caras por thread (todo o periodo, ms por segundo) ==")
    fall = collections.defaultdict(collections.Counter)
    for s in sm:
        fall[s[1]][fn(s[3])] += 1
    for t, cnt in sorted(fall.items(), key=lambda kv: -sum(kv[1].values())):
        tot = sum(cnt.values())
        P("%s (%.1f ms/s):" % (tnames[t] if t < nth else "?", tot * 1000.0 / hz / (dur / 1e3)))
        for k, v in cnt.most_common(12):
            P("    %5.1f%%  %s" % (100.0 * v / tot, k))

    # ---- pagina
    lanes = ["nucleo 0", "nucleo 1", "jogo (N64)"] + tnames
    segs = []  # (lane, t0, t1, cor, rotulo)
    # nucleos: amostras agrupadas (cada amostra = 1/hz de CPU) -> blocos de 1 ms por thread
    for s in sm:
        lane = 0 if s[2] == 0 else 1
        segs.append((lane, round(s[0], 3), round(s[0] + 1000.0 / hz, 3), "t%d" % s[1], tnames[s[1]] if s[1] < nth else "?"))
    for x, y, lab, code in game:
        segs.append((2, round(x, 3), round(y, 3), COLORS.get(code, "#888"), lab))
    for tid, code, a, b, arg, cpu in spans:
        segs.append((3 + tid, round(a, 3), round(b, 3), COLORS.get(code, "#888"), "%s (n%d)" % (CODES.get(code, code), cpu)))
    tcol = {}
    pal = ["#4e79a7", "#f28e2b", "#e15759", "#76b7b2", "#59a14f", "#edc948", "#b07aa1", "#ff9da7", "#9c755f", "#bab0ac"]
    for i in range(nth):
        tcol["t%d" % i] = pal[i % len(pal)]
    data = {"lanes": lanes, "segs": segs, "frames": [round(f, 3) for f in frames], "t0": t_lo, "t1": t_hi, "tcol": tcol,
            "marks": [round(e[0], 3) for e in ev if e[1] == 63]}
    page = PAGE.replace("/*DATA*/", json.dumps(data)).replace("/*TEXT*/", html.escape("\n".join(txt)))
    open(out, "w", encoding="utf-8").write(page)
    txtp = os.path.splitext(out)[0] + ".txt"
    open(txtp, "w", encoding="utf-8").write("\n".join(txt) + "\n")
    print("pagina: %s\nresumo: %s" % (out, txtp))


PAGE = r"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>OoT - medicao dos nucleos</title>
<style>body{font:13px system-ui,sans-serif;margin:12px;background:#fff;color:#222}canvas{border:1px solid #ccc;display:block}
pre{font:12px ui-monospace,monospace;background:#f6f6f6;padding:8px;overflow:auto}#tip{position:fixed;background:#222;color:#fff;
padding:3px 6px;font-size:12px;pointer-events:none;display:none}label{margin-right:12px}</style></head><body>
<h3>Linha do tempo (roda do mouse: zoom; arrastar: mover; passe o mouse para ver o que e)</h3>
<div><label>janela: <span id="win"></span></label><button onclick="zoomAll()">tudo</button>
<button onclick="worst()">ir para o quadro mais lento</button></div>
<canvas id="cv" width="1400" height="600"></canvas><div id="tip"></div>
<p>Linhas finas verticais cinza: um desenho terminou. Vermelhas: SELECT+R1. Nas linhas dos nucleos, cada cor e uma thread
(a legenda esta no nome da linha da thread).</p>
<h3>Resumo</h3><pre>/*TEXT*/</pre>
<script>
const D=/*DATA*/;const cv=document.getElementById('cv'),cx=cv.getContext('2d'),tip=document.getElementById('tip');
const LH=22,LW=150;cv.height=D.lanes.length*LH+20;let a=D.t0,b=Math.min(D.t1,D.t0+500);
function X(t){return LW+(t-a)/(b-a)*(cv.width-LW)}
function draw(){cx.clearRect(0,0,cv.width,cv.height);cx.font='12px sans-serif';
for(let i=0;i<D.lanes.length;i++){const y=i*LH;cx.fillStyle=i%2?'#fafafa':'#f0f0f0';cx.fillRect(LW,y,cv.width-LW,LH);
let c='#222';if(i>=3&&D.tcol['t'+(i-3)])c=D.tcol['t'+(i-3)];cx.fillStyle=c;cx.fillText(D.lanes[i],4,y+15);}
for(const s of D.segs){if(s[2]<a||s[1]>b)continue;const x0=Math.max(X(s[1]),LW),x1=Math.max(X(s[2]),x0+1);
cx.fillStyle=s[3].startsWith('t')?(D.tcol[s[3]]||'#888'):s[3];cx.fillRect(x0,s[0]*LH+3,x1-x0,LH-6);}
cx.strokeStyle='#999';for(const f of D.frames){if(f<a||f>b)continue;const x=X(f);cx.beginPath();cx.moveTo(x,0);cx.lineTo(x,cv.height-20);cx.stroke();}
cx.strokeStyle='#d00';for(const f of D.marks){if(f<a||f>b)continue;const x=X(f);cx.beginPath();cx.moveTo(x,0);cx.lineTo(x,cv.height-20);cx.stroke();}
cx.fillStyle='#222';const step=Math.pow(10,Math.floor(Math.log10((b-a)/5)));for(let t=Math.ceil(a/step)*step;t<b;t+=step){cx.fillText((t/1000).toFixed(3)+'s',X(t),cv.height-5);}
document.getElementById('win').textContent=((b-a)).toFixed(1)+' ms a partir de '+(a/1000).toFixed(3)+' s';}
cv.addEventListener('wheel',e=>{e.preventDefault();const t=a+(e.offsetX-LW)/(cv.width-LW)*(b-a),k=e.deltaY>0?1.25:0.8;a=t-(t-a)*k;b=t+(b-t)*k;draw();});
let drag=null;cv.addEventListener('mousedown',e=>{drag=[e.offsetX,a,b]});window.addEventListener('mouseup',()=>drag=null);
cv.addEventListener('mousemove',e=>{if(drag){const d=(e.offsetX-drag[0])/(cv.width-LW)*(drag[2]-drag[1]);a=drag[1]-d;b=drag[2]-d;draw();return;}
const lane=Math.floor(e.offsetY/LH),t=a+(e.offsetX-LW)/(cv.width-LW)*(b-a);let hit=null;
for(const s of D.segs){if(s[0]==lane&&s[1]<=t&&s[2]>=t){hit=s;break;}}
if(hit){tip.style.display='block';tip.style.left=(e.clientX+12)+'px';tip.style.top=(e.clientY+12)+'px';tip.textContent=hit[4]+': '+(hit[2]-hit[1]).toFixed(2)+' ms';}else tip.style.display='none';});
function zoomAll(){a=D.t0;b=D.t1;draw()}
function worst(){let w=0,wa=D.t0;for(let i=1;i<D.frames.length;i++){const d=D.frames[i]-D.frames[i-1];if(d>w){w=d;wa=D.frames[i-1];}}a=wa-20;b=wa+w+20;draw();}
draw();
</script></body></html>"""

if __name__ == "__main__":
    main()
