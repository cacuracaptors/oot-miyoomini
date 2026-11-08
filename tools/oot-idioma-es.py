#!/usr/bin/env python3
"""oot-idioma-es.py: gera o pacote de idioma espanhol do port (idiomas/es.oot).

uso: python3 oot-idioma-es.py ROM_1.1  ROM_1.0  es.ppf  SAIDA.oot
     (ROMs .z64/.n64/.v64 ou .zip; es.ppf = Files/Games/Ocarina of Time/Languages/es.ppf do Patcher64+)

Traducao: a de eduardo_a2j (v2.2, projeto Dorando), como o Patcher64+ a traz (es.ppf: texto, fonte, creditos e os
graficos com texto: rotulos do HUD e do menu de pausa, nomes dos itens e dos lugares, tela de titulo). O PPF vale para
a ROM USA 1.0 descomprimida (cada arquivo no seu endereco virtual).
  - texto: o PPF troca o bloco de texto (nes_message_data_static) mas nao a tabela de mensagens do codigo; as mensagens
    espanholas estao na mesma ordem da tabela, uma depois da outra (o Patcher64+ refaz a tabela andando pelo bloco).
    Aqui a tabela e refeita do mesmo jeito.
  - letras: a fonte do PPF desenha o "¡" no lugar do "<", o "ñ" no lugar do "\\" e o "¿" no 0x82; o resto dos
    acentos fica onde a fonte do jogo ja os tem (Languages.json do Patcher64+).
  - os outros arquivos (fonte, creditos, texturas com texto): os trechos que o PPF muda, por cima da 1.1 (o pacote
    confere os bytes da 1.1). Num nome de item a 1.1 e diferente da 1.0; ali vale a textura espanhola inteira. O
    n64dd (mensagens do 64DD) fica de fora.
"""
import importlib.util
import os
import struct
import sys
import zlib

_spec = importlib.util.spec_from_file_location('idioma_it', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                       'oot-idioma-it.py'))
_it = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_it)
load_z64, files, apply_ppf_files, ranges, ARGS = _it.load_z64, _it.files, _it.apply_ppf_files, _it.ranges, _it.ARGS

FONT, TEXT, STAFF, CODE, N64DD = 21, 22, 23, 27, 28


def msg_table(code):
    i = code.find(b'\x00\x01\x23\x00\x07\x00\x00\x00')
    assert i > 0
    msgs = []
    base = None
    while True:
        tid, tp, _, addr = struct.unpack('>HBBI', code[i:i + 8])
        i += 8
        if tid == 0xFFFF:
            break
        if base is None:
            base = addr
        msgs.append((tid, tp, addr - base))
    return msgs


def msg_end(b, i):
    while i < len(b):
        c = b[i]
        if c == 0x02:
            return i + 1
        if c < 0x20 or c == 0x7F:
            i += 1 + ARGS.get(c, 0)
            continue
        i += 1
    return -1


def main():
    ROM11, ROM10, PPF, OUT = sys.argv[1:5]
    a = load_z64(ROM11)
    assert a[0x3B:0x40] == b'CZLE\x01', 'a primeira ROM tem de ser a USA 1.1 (Rev A)'
    r10 = load_z64(ROM10)
    assert r10[0x3B:0x40] == b'CZLE\x00', 'a segunda ROM tem de ser a USA 1.0'
    fa = files(a)
    f10 = files(r10)
    g = apply_ppf_files(r10, f10, open(PPF, 'rb').read())

    # texto: a ordem da tabela (deslocamentos crescentes), mensagem atras de mensagem no bloco novo
    msgs = msg_table(f10[CODE])
    offs = sorted(set(o for _, _, o in msgs))
    blob = g[TEXT]
    newoff = {}
    p = 0
    for o in offs:
        newoff[o] = p
        e = msg_end(blob, p)
        assert e > 0, 'mensagem sem fim no texto espanhol'
        p = e
        while p < len(blob) and blob[p] == 0:
            p += 1
    out_blob = bytearray()
    remap = {}
    for k, o in enumerate(offs):
        s = newoff[o]
        e = msg_end(blob, s)
        remap[o] = len(out_blob)
        out_blob += blob[s:e]
        while len(out_blob) % 4:
            out_blob.append(0)
    table = [(tid, tp, remap[o]) for tid, tp, o in msgs]

    # dados: fonte, creditos e graficos (so trechos iguais na 1.0 e na 1.1)
    data = []
    skipped = 0
    for k in sorted(g):
        if k in (TEXT, CODE, N64DD):
            continue
        if len(fa[k]) != len(f10[k]):
            print('arquivo %d com outro tamanho na 1.1: fica de fora' % k)
            continue
        rr = ranges(f10[k], g[k])
        skipped += sum(1 for s, e in rr if fa[k][s:e] != f10[k][s:e])
        if not rr:
            continue
        y = bytearray(fa[k])
        for s, e in rr:
            y[s:e] = g[k][s:e]
        data.append((k, bytes(y)))
    print('arquivos:', len(data), '; trechos onde a 1.1 e diferente da 1.0 (vale o espanhol):', skipped)

    out = bytearray(b'OOTLNG1\0' + struct.pack('<I', 1))
    out += 'Español'.encode('utf-8').ljust(32, b'\0')
    out += 'eduardo_a2j v2.2 (Dorando); Patcher64+ (Admentus, GhostlyDark)'.encode().ljust(224, b'\0')[:224]
    out += struct.pack('<I', len(data))
    tot = 0
    for k, y in data:
        x = fa[k]
        rr = ranges(x, y)
        out += struct.pack('<III', k, len(x), len(rr))
        for s, e in rr:
            seg = y[s:e]
            out += struct.pack('<III', s, e - s, zlib.crc32(x[s:e]) & 0xffffffff) + seg + b'\0' * ((-len(seg)) % 4)
            tot += e - s
    out += struct.pack('<I', len(table))
    for tid, tp, off in table:
        assert off < len(out_blob)
        out += struct.pack('<HBBI', tid, tp, 0, off)
    out += struct.pack('<I', len(out_blob)) + out_blob
    open(OUT, 'wb').write(out)
    print('dados', tot, 'bytes; mensagens', len(table), '; texto', len(out_blob), '; pacote', len(out))


if __name__ == '__main__':
    main()
