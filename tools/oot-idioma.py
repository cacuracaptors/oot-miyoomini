#!/usr/bin/env python3
"""oot-idioma.py: gera o pacote de idioma do port (idiomas/pt.oot).

uso: python3 oot-idioma.py ROM_1.1(.z64/.n64/.v64)  ROM_1.0(.z64)  PATCH.bps  SAIDA.oot

Aplica o patch de traducao (BPS) na ROM USA 1.0 (so na memoria) e guarda no pacote apenas o que
a traducao muda nos arquivos de dados (textos, fonte, texturas com texto), como trechos por cima
da ROM USA 1.1, que e a do port. O codigo e os overlays que o patch tambem muda (partes do
"Redux") ficam de fora. Traducao usada: Zelda 64 BR v1.8 (oot_18.bps).
"""
import struct, sys, zlib

# ---- BPS
def rd(p):
    v=0; s=1
    while True:
        b=p.d[p.i]; p.i+=1
        v+=(b&0x7f)*s
        if b&0x80: return v
        s<<=7; v+=s
class P: pass
def apply(src, patch):
    assert patch[:4]==b'BPS1'
    p=P(); p.d=patch; p.i=4
    ss=rd(p); ts=rd(p); ms=rd(p); p.i+=ms
    assert ss==len(src),(ss,len(src))
    out=bytearray(ts); o=0; sr=0; tr=0
    end=len(patch)-12
    while p.i<end:
        x=rd(p); cmd=x&3; ln=(x>>2)+1
        if cmd==0: out[o:o+ln]=src[o:o+ln]; o+=ln
        elif cmd==1: out[o:o+ln]=patch[p.i:p.i+ln]; p.i+=ln; o+=ln
        elif cmd==2:
            d=rd(p); sr+= (-1 if d&1 else 1)*(d>>1)
            out[o:o+ln]=src[sr:sr+ln]; sr+=ln; o+=ln
        else:
            d=rd(p); tr+= (-1 if d&1 else 1)*(d>>1)
            for k in range(ln): out[o]=out[tr]; o+=1; tr+=1
    import struct
    sc,tc,pc=struct.unpack('<III',patch[-12:])
    assert zlib.crc32(src)&0xffffffff==sc,'src crc'
    assert zlib.crc32(out)&0xffffffff==tc,'tgt crc'
    return bytes(out)

# ---- dmadata / Yaz0
def yaz0(d):
    assert d[:4]==b'Yaz0',d[:4]
    n=struct.unpack('>I',d[4:8])[0]; out=bytearray(); i=16
    while len(out)<n:
        c=d[i]; i+=1
        for b in range(8):
            if len(out)>=n: break
            if c&0x80: out.append(d[i]); i+=1
            else:
                b1,b2=d[i],d[i+1]; i+=2
                dist=((b1&0xf)<<8|b2)+1; ln=b1>>4
                if ln==0: ln=d[i]+0x12; i+=1
                else: ln+=2
                for k in range(ln): out.append(out[-dist])
            c<<=1
    return bytes(out)
def table(rom):
    off=rom.find(struct.pack('>IIII',0,0x1060,0,0))
    ents=[]; i=off
    while True:
        vs,ve,ps,pe=struct.unpack('>IIII',rom[i:i+16]); i+=16
        if vs==0 and ve==0 and ents: break
        ents.append((vs,ve,ps,pe))
    return off,ents
def files(rom):
    off,ents=table(rom); out=[]
    for vs,ve,ps,pe in ents:
        if ps==0xffffffff: out.append(None); continue
        if pe==0: out.append(rom[ps:ps+ve-vs])
        else: out.append(yaz0(rom[ps:pe]))
    return off,ents,out

def load_z64(p):
    d = open(p, 'rb').read()
    if d[:4] == bytes.fromhex('37804012'):
        x = bytearray(d); x[0::2], x[1::2] = d[1::2], d[0::2]; d = bytes(x)
    elif d[:4] == bytes.fromhex('40123780'):
        x = bytearray(d); x[0::4], x[1::4], x[2::4], x[3::4] = d[3::4], d[2::4], d[1::4], d[0::4]; d = bytes(x)
    return d

ROM11, ROM10, PATCH, OUT = sys.argv[1:5]
a = load_z64(ROM11)
assert a[0x3B:0x40] == b'CZLE\x01', 'a primeira ROM tem de ser a USA 1.1 (Rev A)'
r10 = load_z64(ROM10)
assert zlib.crc32(r10) & 0xffffffff, 'ROM 1.0'
b = apply(r10, open(PATCH, 'rb').read())
_, ea, fa = files(a); _, eb, fb = files(b)


DATA = [8, 12, 14, 15, 16, 17, 21, 23, 939, 517, 520, 582, 620, 637, 660, 691, 705, 706] + list(range(880, 936))
NES = 22

def ranges(x, y, gap=16):
    r = []
    for i in range(len(x)):
        if x[i] != y[i]:
            if r and i - r[-1][1] <= gap:
                r[-1][1] = i + 1
            else:
                r.append([i, i + 1])
    return r

out = bytearray(b'OOTLNG1\0' + struct.pack('<I', 1))
out += b'Portugues do Brasil'.ljust(32, b'\0')
out += ('Zelda 64 BR v1.8 (2024): Hyllian, Odin, Fserve, Solid_One; graficos: Patcher64+').encode().ljust(224, b'\0')[:224]
out += struct.pack('<I', len(DATA))
tot = 0
# rotulos "To ..." do menu de pausa: a traducao tirou o "To" (tools/oot-idioma-irpara.py)
import importlib.util, os
_spec = importlib.util.spec_from_file_location("irpara", os.path.join(os.path.dirname(os.path.abspath(__file__)), "oot-idioma-irpara.py"))
_irpara = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_irpara)
for k in DATA:
    x, y = fa[k], fb[k]
    if k == _irpara.NES:
        y = _irpara.fix_labels(y)
    assert len(x) == len(y), k
    rr = ranges(x, y)
    out += struct.pack('<III', k, len(x), len(rr))
    for s, e in rr:
        seg = y[s:e]
        out += struct.pack('<III', s, e - s, zlib.crc32(x[s:e]) & 0xffffffff) + seg + b'\0' * ((-len(seg)) % 4)
        tot += e - s

# tabela de mensagens: o codigo (arquivo 27) traz a tabela; acha na ROM traduzida
code = fb[27]
i = code.find(b'\x00\x01\x23\x00\x07\x00\x00\x00')
assert i > 0
msgs = []
base = None
while True:
    tid, tp, _, addr = struct.unpack('>HBBI', code[i:i + 8]); i += 8
    if tid == 0xFFFF:
        break
    if base is None:
        base = addr
    msgs.append((tid, tp, addr - base))
blob = fb[NES]
out += struct.pack('<I', len(msgs))
for tid, tp, off in msgs:
    assert off < len(blob)
    out += struct.pack('<HBBI', tid, tp, 0, off)
out += struct.pack('<I', len(blob)) + blob
open(OUT, 'wb').write(out)
print('arquivos', len(DATA), 'bytes de texturas/fonte', tot, 'mensagens', len(msgs), 'texto', len(blob), 'pacote', len(out))
