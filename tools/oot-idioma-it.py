#!/usr/bin/env python3
"""oot-idioma-it.py: gera o pacote de idioma italiano do port (idiomas/it.oot).

uso: python3 oot-idioma-it.py ROM_1.1(.z64/.n64/.v64)  ROM_1.0(.z64/.n64/.v64)  ZELDAITA.IPS  SAIDA.oot

Traducao: "Zelda 64 ITA" v1.00 (2001), de XXcentury (patch IPS para a ROM USA 1.0). O patch so muda o
texto do jogo (nes_message_data_static) e os creditos (staff_message_data_static); a tabela de
mensagens do codigo nao muda (cada mensagem italiana ocupa o lugar da inglesa).

O que este script faz, alem de aplicar o patch (so na memoria):
  - acentos: em 2001 a traducao usou o apostrofo no lugar do acento ("piu'", "citta'", "E'") e o "e"
    agudo (0x96) para o verbo "e" (e) e para o fim de "Rupie". Aqui eles viram as letras acentuadas da
    fonte do jogo (a 0x90, e 0x95, e 0x96 em "perche", u 0x9C, E 0x85). O texto fica mais curto, por isso
    o bloco de texto e a tabela de mensagens sao montados de novo.
  - a fonte do jogo (ingles) nao tem "i" nem "o" com acento grave: o "i" com trema (0x99) e o "o" com
    trema (0x9B), que o italiano nao usa, sao redesenhados como "i" e "o" com o acento grave do "e" (0x95)
    so dentro deste pacote (trechos do arquivo da fonte).
Os creditos ficam como no patch (o nome do tradutor no lugar dos tradutores ingleses).
"""
import struct
import sys
import zlib


# ---- ROM (dmadata / Yaz0), como em oot-idioma.py
def yaz0(d):
    assert d[:4] == b'Yaz0', d[:4]
    n = struct.unpack('>I', d[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < n:
        c = d[i]
        i += 1
        for b in range(8):
            if len(out) >= n:
                break
            if c & 0x80:
                out.append(d[i])
                i += 1
            else:
                b1, b2 = d[i], d[i + 1]
                i += 2
                dist = ((b1 & 0xf) << 8 | b2) + 1
                ln = b1 >> 4
                if ln == 0:
                    ln = d[i] + 0x12
                    i += 1
                else:
                    ln += 2
                for k in range(ln):
                    out.append(out[-dist])
            c <<= 1
    return bytes(out)


def files(rom):
    off = rom.find(struct.pack('>IIII', 0, 0x1060, 0, 0))
    ents = []
    i = off
    while True:
        vs, ve, ps, pe = struct.unpack('>IIII', rom[i:i + 16])
        i += 16
        if vs == 0 and ve == 0 and ents:
            break
        ents.append((vs, ve, ps, pe))
    out = []
    for vs, ve, ps, pe in ents:
        if ps == 0xffffffff:
            out.append(None)
        elif pe == 0:
            out.append(rom[ps:ps + ve - vs])
        else:
            out.append(yaz0(rom[ps:pe]))
    return out


def load_z64(p):
    d = open(p, 'rb').read()
    if p.lower().endswith('.zip'):
        import io, zipfile
        z = zipfile.ZipFile(io.BytesIO(d))
        d = z.read([n for n in z.namelist() if n.lower().endswith(('.z64', '.n64', '.v64'))][0])
    if d[:4] == bytes.fromhex('37804012'):
        x = bytearray(d)
        x[0::2], x[1::2] = d[1::2], d[0::2]
        d = bytes(x)
    elif d[:4] == bytes.fromhex('40123780'):
        x = bytearray(d)
        x[0::4], x[1::4], x[2::4], x[3::4] = d[3::4], d[2::4], d[1::4], d[0::4]
        d = bytes(x)
    return d


def apply_ips(rom, ips):
    assert ips[:5] == b'PATCH', 'nao e um IPS'
    r = bytearray(rom)
    i = 5
    while ips[i:i + 3] != b'EOF':
        off = int.from_bytes(ips[i:i + 3], 'big')
        n = int.from_bytes(ips[i + 3:i + 5], 'big')
        i += 5
        if n == 0:
            rl = int.from_bytes(ips[i:i + 2], 'big')
            r[off:off + rl] = bytes([ips[i + 2]]) * rl
            i += 3
        else:
            r[off:off + n] = ips[i:i + n]
            i += n
    return bytes(r)


# ---- texto (codigos de controle e quantos bytes de argumento cada um tem; tools/msgdis do decomp)
ARGS = {0x05: 1, 0x06: 1, 0x07: 2, 0x0C: 1, 0x0E: 1, 0x11: 2, 0x12: 2, 0x13: 1, 0x14: 1, 0x15: 3, 0x1E: 1}


def split_runs(seg):
    """[('t', texto) | ('c', controle)]; depois do fim (0x02) ou de 0x07 (outra mensagem), o resto vai inteiro"""
    out = []
    cur = bytearray()
    i = 0
    while i < len(seg):
        c = seg[i]
        if c < 0x20 or c == 0x7F:
            if cur:
                out.append(('t', bytes(cur)))
                cur = bytearray()
            if c == 0x02:
                out.append(('c', bytes(seg[i:])))
                return out
            n = ARGS.get(c, 0)
            out.append(('c', bytes(seg[i:i + 1 + n])))
            i += 1 + n
            if c == 0x07:
                out.append(('c', bytes(seg[i:])))
                return out
            continue
        cur.append(c)
        i += 1
    if cur:
        out.append(('t', bytes(cur)))
    return out


A_GRAVE, E_GRAVE, E_ACUTE, I_GRAVE, O_GRAVE, U_GRAVE, CAP_E_GRAVE = 0x90, 0x95, 0x96, 0x99, 0x9B, 0x9C, 0x85
LETTER = set(range(ord('A'), ord('Z') + 1)) | set(range(ord('a'), ord('z') + 1)) | set(range(0x80, 0x9F))
# palavras com apostrofo final que estao certas assim (imperativos e truncamentos), ou duvidosas
KEEP = {b'po', b'di', b'da', b'va', b'fa', b'sta', b'su', b'ti', b'sia', b'Di', b'Da', b'Va', b'Fa', b'Su',
        b'Sala', b'chiedela'}


def accents(t, stats):
    """converte os acentos de um trecho de texto (sem codigos de controle)"""
    out = bytearray()
    i = 0
    n = len(t)
    while i < n:
        c = t[i]
        # palavra: letras seguidas
        if c in LETTER:
            j = i
            while j < n and t[j] in LETTER:
                j += 1
            w = bytes(t[i:j])
            nxt = t[j] if j < n else None
            after = t[j + 1] if j + 1 < n else None
            if nxt == 0x27 and (after is None or after not in LETTER) and w[-1:] in (b'a', b'e', b'i', b'o', b'u', b'E') \
                    and w not in KEEP:
                last = w[-1]
                stem = w[:-1]
                if last == ord('a'):
                    acc = A_GRAVE
                elif last == ord('e'):
                    acc = E_ACUTE if (stem.endswith(b'ch') or stem.endswith(b'tr') or w in (b'ne', b'se')) else E_GRAVE
                elif last == ord('i'):
                    acc = I_GRAVE
                elif last == ord('o'):
                    acc = O_GRAVE
                elif last == ord('u'):
                    acc = U_GRAVE
                else:  # 'E'
                    acc = CAP_E_GRAVE if not stem else None
                if acc is not None:
                    out += stem + bytes([acc])
                    stats['apostrofo'] += 1
                    i = j + 1
                    continue
            if w == bytes([E_ACUTE]):
                # o verbo "e" escrito com o agudo (c'e, e solto): grave
                out.append(E_GRAVE)
                stats['e verbo'] += 1
                i = j
                continue
            if w.endswith(b'pi' + bytes([E_ACUTE])):
                # "Rupie"/"Lupie" com o agudo no fim: sem acento
                out += w[:-1] + b'e'
                stats['rupie'] += 1
                i = j
                continue
            if w in (b'Rupia', b'Rupie') and nxt == 0x27:
                out += w
                stats['rupia'] += 1
                i = j + 1
                continue
            out += w
            i = j
            continue
        out.append(c)
        i += 1
    return bytes(out)


def build_text(code, blob):
    """(tabela [(id, tipo, deslocamento)], bloco novo) com os acentos convertidos"""
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
    offs = sorted(set(o for _, _, o in msgs)) + [len(blob)]
    stats = {'apostrofo': 0, 'e verbo': 0, 'rupie': 0, 'rupia': 0}
    newoff = {}
    out = bytearray()
    for k in range(len(offs) - 1):
        seg = blob[offs[k]:offs[k + 1]]
        newoff[offs[k]] = len(out)
        for kind, part in split_runs(seg):
            out += accents(part, stats) if kind == 't' else part
        while len(out) % 4:
            out.append(0)
    table = [(tid, tp, newoff[o]) for tid, tp, o in msgs]
    return table, bytes(out), stats


# ---- fonte: "i" e "o" com acento grave no lugar do "i" e do "o" com trema
FONT = 21


def glyph(F, c):
    o = (c - 0x20) * 128
    px = []
    for b in F[o:o + 128]:
        px += [b >> 4, b & 15]
    return [px[r * 16:(r + 1) * 16] for r in range(16)]


def put(F, c, G):
    o = (c - 0x20) * 128
    px = [v for row in G for v in row]
    F[o:o + 128] = bytes((px[2 * i] << 4) | px[2 * i + 1] for i in range(128))


def grave_from(F, base, dx, top=4):
    """a letra 'base' sem o trema (linhas de cima) e com o acento do 'e' grave deslocado dx colunas"""
    B = [r[:] for r in glyph(F, base)]
    E = glyph(F, E_GRAVE)
    for r in range(top):
        B[r] = [0] * 16
        for c in range(16):
            if E[r][c] and 0 <= c + dx < 16:
                B[r][c + dx] = E[r][c]
    return B


def ranges(x, y, gap=16):
    r = []
    for i in range(len(x)):
        if x[i] != y[i]:
            if r and i - r[-1][1] <= gap:
                r[-1][1] = i + 1
            else:
                r.append([i, i + 1])
    return r


def main():
    ROM11, ROM10, IPS, OUT = sys.argv[1:5]
    a = load_z64(ROM11)
    assert a[0x3B:0x40] == b'CZLE\x01', 'a primeira ROM tem de ser a USA 1.1 (Rev A)'
    r10 = load_z64(ROM10)
    assert r10[0x3B:0x40] == b'CZLE\x00', 'a segunda ROM tem de ser a USA 1.0'
    b = apply_ips(r10, open(IPS, 'rb').read())
    fa = files(a)
    fb = files(b)
    f10 = files(r10)

    # dados: a fonte (redesenhada aqui) e os creditos (do patch); os arquivos sao iguais na 1.0 e na 1.1
    font = bytearray(fa[FONT])
    assert f10[FONT] == fa[FONT]
    put(font, I_GRAVE, grave_from(fa[FONT], I_GRAVE, -2))
    put(font, O_GRAVE, grave_from(fa[FONT], O_GRAVE, -1))
    STAFF = 23
    assert f10[STAFF] == fa[STAFF], 'creditos diferentes entre 1.0 e 1.1'
    data = [(FONT, bytes(font)), (STAFF, fb[STAFF])]
    for k in range(len(fb)):
        if k not in (22, STAFF) and fb[k] != f10[k]:
            sys.exit('o patch muda o arquivo %d, que este script nao conhece' % k)

    table, blob, stats = build_text(fb[27], fb[22])
    used = set(blob)
    assert I_GRAVE in used and O_GRAVE in used

    out = bytearray(b'OOTLNG1\0' + struct.pack('<I', 1))
    out += b'Italiano'.ljust(32, b'\0')
    out += 'Zelda 64 ITA v1.00 (2001): XXcentury'.encode().ljust(224, b'\0')[:224]
    out += struct.pack('<I', len(data))
    tot = 0
    for k, y in data:
        x = fa[k]
        assert len(x) == len(y), k
        rr = ranges(x, y)
        out += struct.pack('<III', k, len(x), len(rr))
        for s, e in rr:
            seg = y[s:e]
            out += struct.pack('<III', s, e - s, zlib.crc32(x[s:e]) & 0xffffffff) + seg + b'\0' * ((-len(seg)) % 4)
            tot += e - s
    out += struct.pack('<I', len(table))
    for tid, tp, off in table:
        assert off < len(blob)
        out += struct.pack('<HBBI', tid, tp, 0, off)
    out += struct.pack('<I', len(blob)) + blob
    open(OUT, 'wb').write(out)
    print('acentos:', ', '.join('%s %d' % kv for kv in stats.items()))
    print('fonte/creditos', tot, 'bytes; mensagens', len(table), '; texto', len(blob), '(antes', len(fb[22]), '); pacote', len(out))


if __name__ == '__main__':
    main()
