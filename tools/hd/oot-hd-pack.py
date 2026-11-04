#!/usr/bin/env python3
# oot-hd-pack.py -- converte pacotes de texturas do Ship of Harkinian (.otr/.o2r, ou .zip com eles dentro)
# no arquivo de texturas do port (texturas_hd.pak). Roda NO WSL; precisa de python3-pil e python3-numpy.
#
#   python3 oot-hd-pack.py --rom ROM_1.1(.z64 ou .zip) --saida texturas_hd.pak [--escala 2] \
#                          [--amostras PASTA] pacote1 [pacote2 ...]
#
# - So entram texturas que existem no jogo original (nome igual ao dos mapas de assets do Shipwright para
#   a NTSC 1.1, em ntsc11_texturas.tsv). Modelos novos (listas de desenho) ficam de fora.
# - Cada textura fica com no maximo ESCALA vezes o tamanho original (2 = 32x32 -> 64x64), em RGBA8888
#   comprimido (deflate). Pacotes depois na linha de comando passam por cima dos anteriores (como no SoH).
# - Guarda o CRC32 dos bytes originais: o jogo so troca a textura se a da memoria for igual (uma textura
#   mudada pela traducao, por exemplo, continua a do jogo).
import sys, os, io, struct, zipfile, zlib, bz2, argparse, collections, tempfile, shutil

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("ERRO: falta python3-pil ou python3-numpy (sudo apt install python3-pil python3-numpy)")
    sys.exit(1)

AQUI = os.path.dirname(os.path.abspath(__file__))
FMTS = {"RGBA32": (0, 3), "RGBA16": (0, 2), "CI4": (2, 0), "CI8": (2, 1), "IA4": (3, 0), "IA8": (3, 1),
        "IA16": (3, 2), "I4": (4, 0), "I8": (4, 1)}  # (fmt, siz) do RDP
BITS = {0: 4, 1: 8, 2: 16, 3: 32}
OTEXFMT = {1: "RGBA32", 2: "RGBA16", 3: "CI4", 4: "CI8", 5: "I4", 6: "I8", 7: "IA4", 8: "IA8", 9: "IA16"}


# ------------------------------------------------------------------------------------------- ROM
def le_rom(path):
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path)
        nomes = [n for n in z.namelist() if n.lower().endswith((".z64", ".n64", ".v64"))]
        if not nomes:
            raise SystemExit("ERRO: nenhuma ROM (.z64/.n64/.v64) dentro de " + path)
        d = bytearray(z.read(nomes[0]))
    else:
        d = bytearray(open(path, "rb").read())
    h = bytes(d[:4])
    if h == b"\x37\x80\x40\x12":  # v64: pares de bytes trocados
        d[0::2], d[1::2] = d[1::2], d[0::2]
    elif h == b"\x40\x12\x37\x80":  # n64: palavras invertidas
        a = np.frombuffer(bytes(d), dtype="<u4").byteswap()
        d = bytearray(a.tobytes())
    if bytes(d[:4]) != b"\x80\x37\x12\x40":
        raise SystemExit("ERRO: a ROM nao parece ser de N64")
    nome = bytes(d[0x20:0x34]).decode("latin1").strip()
    print("ROM: %s, versao %d, %.1f MB" % (nome, d[0x3F], len(d) / 1e6))
    if d[0x3F] != 1 or "ZELDA" not in nome:
        print("AVISO: o port usa a Ocarina of Time USA 1.1 (Rev A); esta ROM pode nao bater")
    return bytes(d)


def yaz0(src):
    if src[:4] != b"Yaz0":
        return src
    n = struct.unpack(">I", src[4:8])[0]
    dst = bytearray(n)
    si, di = 16, 0
    while di < n:
        code = src[si]
        si += 1
        for bit in range(8):
            if di >= n:
                break
            if code & (0x80 >> bit):
                dst[di] = src[si]
                di += 1
                si += 1
            else:
                b1, b2 = src[si], src[si + 1]
                si += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                ln = b1 >> 4
                if ln == 0:
                    ln = src[si] + 0x12
                    si += 1
                else:
                    ln += 2
                s = di - dist
                if dist >= ln:
                    dst[di:di + ln] = dst[s:s + ln]
                    di += ln
                else:
                    for _ in range(ln):
                        dst[di] = dst[s]
                        di += 1
                        s += 1
    return bytes(dst)


class Rom:
    def __init__(self, data):
        self.d = data
        pat = bytes.fromhex("00000000" "00001060" "00000000" "00000000")
        off = data.find(pat)
        if off < 0:
            raise SystemExit("ERRO: nao achei a dmadata na ROM")
        self.files = {}
        self.por_rom = {}  # posicao fisica (comprimida) -> vrom
        i = off
        while True:
            vs, ve, rs, re_ = struct.unpack(">IIII", data[i:i + 16])
            if ve == 0:
                break
            self.files[vs] = (ve, rs, re_)
            if rs != 0xFFFFFFFF:
                self.por_rom[rs] = vs
            i += 16
        self.cache = {}

    def arquivo(self, vs):
        if vs in self.cache:
            return self.cache[vs]
        if vs not in self.files:
            return None
        ve, rs, re_ = self.files[vs]
        if rs == 0xFFFFFFFF:
            return None
        raw = self.d[rs:re_] if re_ else self.d[rs:rs + (ve - vs)]
        d = yaz0(raw) if re_ else raw
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[vs] = d
        return d


# ------------------------------------------------------------------------- leitor MPQ (.otr)
def mpq_table():
    ct = [0] * 0x500
    seed = 0x00100001
    for i1 in range(0x100):
        i2 = i1
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            t1 = (seed & 0xFFFF) << 0x10
            seed = (seed * 125 + 3) % 0x2AAAAB
            ct[i2] = t1 | (seed & 0xFFFF)
            i2 += 0x100
    return ct


CT = mpq_table()


def mpq_hash(s, kind, slash):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    s = s.upper()
    if slash:
        s = s.replace("/", "\\")
    for ch in s.encode("latin1"):
        s1 = (CT[(kind << 8) + ch] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1


def mpq_decrypt(data, key):
    out = bytearray()
    seed = 0xEEEEEEEE
    n = len(data) // 4
    for v in struct.unpack("<%dI" % n, data[:n * 4]):
        seed = (seed + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        ch = (v ^ (key + seed)) & 0xFFFFFFFF
        key = ((((~key) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (ch + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out += struct.pack("<I", ch)
    return bytes(out)


class MPQ:
    def __init__(self, path):
        self.f = open(path, "rb")
        data = self.f.read(0x200)
        off = data.find(b"MPQ\x1a")
        self.base = off
        (hsize, asize, ver, sshift, htpos, btpos, htn, btn) = struct.unpack("<IIHHIIII", data[off + 4:off + 32])
        self.sector = 512 << sshift
        self.f.seek(self.base + htpos)
        self.ht = mpq_decrypt(self.f.read(htn * 16), mpq_hash("(hash table)", 3, True))
        self.f.seek(self.base + btpos)
        self.bt = mpq_decrypt(self.f.read(btn * 16), mpq_hash("(block table)", 3, True))
        self.htn = htn

    def _find(self, name, slash):
        a, b, i0 = mpq_hash(name, 1, slash), mpq_hash(name, 2, slash), mpq_hash(name, 0, slash)
        for k in range(self.htn):
            i = (i0 + k) % self.htn
            ha, hb, loc, plat, blk = struct.unpack("<IIHHI", self.ht[i * 16:i * 16 + 16])
            if blk == 0xFFFFFFFF:
                return None
            if ha == a and hb == b:
                return blk
        return None

    def read(self, name):
        blk = self._find(name, False)
        if blk is None:
            blk = self._find(name, True)
        if blk is None:
            return None
        pos, csize, fsize, flags = struct.unpack("<IIII", self.bt[blk * 16:blk * 16 + 16])
        if flags & 0x10000:
            return None
        self.f.seek(self.base + pos)
        raw = self.f.read(csize)
        if not (flags & 0x300):
            return raw[:fsize]
        if flags & 0x1000000:
            return self._dec(raw) if csize < fsize else raw[:fsize]
        ns = (fsize + self.sector - 1) // self.sector
        offs = struct.unpack("<%dI" % (ns + 1), raw[:4 * (ns + 1)])
        out = bytearray()
        for i in range(ns):
            chunk = raw[offs[i]:offs[i + 1]]
            want = min(self.sector, fsize - i * self.sector)
            out += self._dec(chunk) if len(chunk) < want else chunk
        return bytes(out)

    @staticmethod
    def _dec(d):
        m = d[0]
        if m & 0x02:
            return zlib.decompress(d[1:])
        if m & 0x10:
            return bz2.decompress(d[1:])
        return d[1:]

    def names(self):
        lst = self.read("(listfile)")
        if lst is None:
            return []
        return [x for x in lst.decode("latin1").replace("\r", "").split("\n") if x and not x.startswith("(")]


def recursos(path, tmp):
    """gera (nome, bytes) de um .otr/.o2r/.zip (zip com .otr/.o2r dentro: na ordem dos nomes)"""
    with open(path, "rb") as f:
        head = f.read(0x200)
    if head.find(b"MPQ\x1a") >= 0:
        m = MPQ(path)
        for n in m.names():
            try:
                d = m.read(n)
            except Exception:
                d = None
            if d is not None:
                yield n, d
        return
    z = zipfile.ZipFile(path)
    dentro = sorted([i for i in z.infolist() if i.filename.lower().endswith((".otr", ".o2r"))], key=lambda i: i.filename)
    if dentro:
        for i in dentro:
            out = os.path.join(tmp, "pacote_interno")
            with z.open(i) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            print("  %s" % i.filename)
            for r in recursos(out, tmp):
                yield r
            os.remove(out)
        return
    for i in z.infolist():
        if not i.is_dir():
            yield i.filename, z.read(i)


# --------------------------------------------------------------------------- texturas
def decodifica(fmtname, w, h, data):
    """bytes no formato do N64 -> matriz h x w x 4 (RGBA8888), igual ao decodificador do port"""
    if fmtname == "RGBA32":
        a = np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
        return a.copy()
    if fmtname == "RGBA16":
        c = np.frombuffer(data[:w * h * 2], ">u2").reshape(h, w).astype(np.uint32)
        r, g, b = (c >> 11) & 31, (c >> 6) & 31, (c >> 1) & 31
        out = np.empty((h, w, 4), np.uint8)
        out[..., 0] = (r << 3) | (r >> 2)
        out[..., 1] = (g << 3) | (g >> 2)
        out[..., 2] = (b << 3) | (b >> 2)
        out[..., 3] = np.where(c & 1, 255, 0)
        return out
    if fmtname == "IA16":
        c = np.frombuffer(data[:w * h * 2], np.uint8).reshape(h, w, 2)
        i, a = c[..., 0], c[..., 1]
        return np.stack([i, i, i, a], -1)
    if fmtname in ("IA8", "I8"):
        b = np.frombuffer(data[:w * h], np.uint8).reshape(h, w)
        if fmtname == "I8":
            return np.stack([b, b, b, b], -1)
        i, a = (b >> 4) * 17, (b & 15) * 17
        return np.stack([i, i, i, a], -1).astype(np.uint8)
    if fmtname in ("IA4", "I4"):
        b = np.frombuffer(data[:(w * h + 1) // 2], np.uint8)
        n = np.empty(b.size * 2, np.uint8)
        n[0::2], n[1::2] = b >> 4, b & 15
        n = n[:w * h].reshape(h, w)
        if fmtname == "I4":
            i = n * 17
            return np.stack([i, i, i, i], -1).astype(np.uint8)
        i3 = (n >> 1) & 7
        i = (i3 << 5) | (i3 << 2) | (i3 >> 1)
        return np.stack([i, i, i, np.where(n & 1, 255, 0)], -1).astype(np.uint8)
    return None


def reduz(img, W, H):
    """reduz com o alfa pre-multiplicado (sem halo escuro em volta das partes transparentes)"""
    h, w = img.shape[:2]
    if (w, h) == (W, H):
        return img
    f = img.astype(np.float32) / 255.0
    rgb = f[..., :3] * f[..., 3:4]
    pm = np.concatenate([rgb, f[..., 3:4]], -1)
    chans = []
    for k in range(4):
        im = Image.fromarray(pm[..., k], mode="F")
        chans.append(np.asarray(im.resize((W, H), Image.BOX if (w % W == 0 and h % H == 0) else Image.LANCZOS)))
    pm = np.clip(np.stack(chans, -1), 0.0, 1.0)
    a = pm[..., 3:4]
    rgb = np.where(a > 1e-6, pm[..., :3] / np.maximum(a, 1e-6), 0.0)
    out = np.concatenate([rgb, a], -1)
    return np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8)


AUTO_MAX = 2.0  # --brilho auto clareia no maximo 2x (as cenas do mod para 3DS chegam a ser 1.5-1.8x mais escuras)


def arruma_bordas(img):
    """ceu (vr_*): a arte nova tem a 1a linha/coluna pela metade (escura) e preto depois da parte usada;
    nas emendas entre as faces do ceu isso vira linhas e faixas escuras. Repete a ultima linha/coluna boa."""
    img = img.copy()
    for eixo in (1, 0):  # colunas, depois linhas
        lum = img[..., :3].astype(np.float32).sum(-1).mean(axis=0 if eixo == 1 else 1)
        n = lum.size
        lim = max(2, n // 16)
        e = 0
        while e < lim and lum[e] < 0.75 * lum[e + 1:e + 1 + lim].max():
            e += 1
        d = n - 1
        while d > n - 1 - lim and lum[d] < 0.75 * lum[d - lim:d].max():
            d -= 1
        if eixo == 1:
            img[:, :e] = img[:, e:e + 1]
            img[:, d + 1:] = img[:, d:d + 1]
        else:
            img[:e] = img[e:e + 1]
            img[d + 1:] = img[d:d + 1]
    return img


def gera_indice(tsv):
    """monta ntsc11_texturas.tsv a partir dos mapas de assets do Shipwright (baixa so a pasta da NTSC 1.1)"""
    import re, subprocess
    rep = os.path.join(os.path.expanduser("~"), ".cache", "oot-hd-shipwright")
    yml = os.path.join(rep, "soh", "assets", "yml", "ntsc_1-1")
    if not os.path.isdir(yml):
        print("baixando os mapas de assets do Shipwright (so a NTSC 1.1)...")
        shutil.rmtree(rep, ignore_errors=True)
        os.makedirs(os.path.dirname(rep), exist_ok=True)
        subprocess.check_call(["git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
                               "https://github.com/HarbourMasters/Shipwright.git", rep])
        subprocess.check_call(["git", "-C", rep, "sparse-checkout", "set", "soh/assets/yml/ntsc_1-1"])
    out = []
    for root, ds, fs in os.walk(yml):
        for f in fs:
            if not f.endswith(".yml"):
                continue
            rel = os.path.relpath(os.path.join(root, f), yml)[:-4].replace(os.sep, "/")
            parts = rel.split("/")
            if parts[0] == "scenes" and len(parts) >= 3:
                m = re.match(r"(.+)_room_\d+$", parts[2])
                d = "scenes/%s/%s" % (parts[1], (m.group(1) + "_scene") if m else parts[2])
            elif parts[0] in ("objects", "textures", "overlays", "misc", "code"):
                d = rel
            else:
                continue
            txt = open(os.path.join(root, f), encoding="utf-8").read()
            m = re.search(r"segments:\s*\n\s*- \[\s*(\d+),\s*(0x[0-9A-Fa-f]+)\s*\]", txt)
            if not m:
                continue
            base = int(m.group(2), 16)
            for blk in re.split(r"\n(?=\S)", txt):
                mm = re.match(r"(\S+):\s*\n", blk)
                if not mm or "type: TEXTURE" not in blk:
                    continue
                def g(k):
                    r = re.search(r"\n\s+%s:\s*(\S+)" % k, blk)
                    return r.group(1) if r else None
                off, fmt, w, h, tl = g("offset"), g("format"), g("width"), g("height"), g("tlut")
                if off and fmt and w and h:
                    out.append("%s/%s\t%08X\t%X\t%s\t%s\t%s\t%s" % (d, mm.group(1), base, int(off, 16), fmt, w, h,
                                                                  "%X" % int(tl, 16) if tl else "-"))
    out.sort()
    with open(tsv, "w", encoding="utf-8") as f:
        f.write("# nome\tbase_vrom\toffset\tformato\tlargura\taltura\tpaleta (de Shipwright soh/assets/yml/ntsc_1-1)\n")
        f.write("\n".join(out) + "\n")
    print("indice gerado: %d texturas" % len(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--saida", required=True)
    ap.add_argument("--escala", type=int, default=2)
    ap.add_argument("--amostras", default=None)
    ap.add_argument("--brilho", default="1",
                    help="auto = cada nova com o brilho medio da original; numero = multiplica as cores (1.15 = 15%% mais claro); "
                         "auto*1.1 = as duas coisas")
    ap.add_argument("--sem-ceu", action="store_true", help="deixa o ceu (vr_*) com as texturas originais")
    ap.add_argument("--so", default=None,
                    help="so entram texturas cujo nome comeca com um destes prefixos (separados por virgula), "
                         "ex.: objects/object_mag/")
    ap.add_argument("--sem", default=None,
                    help="deixa de fora (com a textura original) as que comecam com um destes prefixos (virgula), "
                         "ex.: objects/object_mag/gTitleCopyright1998Tex")
    ap.add_argument("pacotes", nargs="+")
    a = ap.parse_args()

    idx = {}
    tsv = os.path.join(AQUI, "ntsc11_texturas.tsv")
    if not os.path.exists(tsv) or "paleta" not in open(tsv, encoding="utf-8").readline():
        gera_indice(tsv)  # (indice antigo, sem a coluna da paleta: refaz)
    paleta = {}
    for l in open(tsv, encoding="utf-8"):
        if l.startswith("#"):
            continue
        p = l.rstrip("\n").split("\t")
        idx[p[0]] = (int(p[1], 16), int(p[2], 16), p[3], int(p[4]), int(p[5]))
        if len(p) > 6 and p[6] != "-":
            paleta[p[0]] = int(p[6], 16)
    rom = Rom(le_rom(a.rom))
    print("indice: %d texturas do jogo; dmadata: %d arquivos" % (len(idx), len(rom.files)))
    # a base dos mapas do Shipwright pode ser a posicao virtual (vrom) ou a fisica (na ROM comprimida)
    bases = set(v[0] for v in idx.values())
    nv = sum(1 for b in bases if b in rom.files)
    nr = sum(1 for b in bases if b in rom.por_rom)
    print("arquivos do indice que batem com a dmadata da ROM: %d pela posicao virtual, %d pela fisica, de %d" % (nv, nr, len(bases)))
    usa_rom = nr > nv
    def vbase(b):
        if usa_rom:
            return rom.por_rom.get(b)
        return b if b in rom.files else None

    def original(n, ofmt, ow, oh, orig_file, off, obytes):
        """a original decodificada (as de paleta com a paleta delas, RGBA16, do mesmo arquivo)"""
        if not ofmt.startswith("CI"):
            return decodifica(ofmt, ow, oh, orig_file[off:off + obytes])
        tl = paleta.get(n)
        if tl is None:
            return None
        bits = 4 if ofmt == "CI4" else 8
        b = np.frombuffer(orig_file[off:off + obytes], np.uint8)
        if bits == 4:
            ix = np.empty(b.size * 2, np.uint8)
            ix[0::2], ix[1::2] = b >> 4, b & 15
        else:
            ix = b
        ix = ix[:ow * oh].astype(np.int32)
        nc = min(1 << bits, max(0, (len(orig_file) - tl) // 2))
        if nc == 0:
            return None
        pal = decodifica("RGBA16", nc, 1, orig_file[tl:tl + nc * 2]).reshape(nc, 4)
        return pal[np.minimum(ix, nc - 1)].reshape(oh, ow, 4)

    # brilho medio (das partes visiveis) da original e da nova, por area do jogo
    areas = collections.defaultdict(lambda: [0, 0.0, 0.0])

    def area_de(n):
        p = n.split("/")
        return "/".join(p[:3]) if p[0] == "scenes" else "/".join(p[:2])

    entradas = {}  # vrom -> (crc, fmt, siz, ow, oh, hw, hh, dados)
    brilho_tsv = []  # por textura: nome, formato, brilho da original, da nova, razao, ajuste aplicado
    ceu_pend = {}  # pasta do ceu -> {vrom: (crc, fmt, siz, ow, oh, k, img, brilho original, brilho novo)}
    cont = collections.Counter()
    amostras = 0
    tmp = tempfile.mkdtemp(prefix="oothd_", dir=os.path.expanduser("~"))
    try:
        for pac in a.pacotes:
            print("pacote:", pac)
            for nome, d in recursos(pac, tmp):
                if len(d) < 0x40 + 28 or d[4:8] != b"XETO":
                    continue
                cont["texturas no pacote"] += 1
                n = nome[4:] if nome.startswith("alt/") else nome
                info = idx.get(n)
                if info is None:
                    cont["sem par no jogo (nova ou de outra versao)"] += 1
                    continue
                base, off, ofmt, ow, oh = info
                if a.so and not any(n.startswith(q) for q in a.so.split(",")):
                    cont["fora do --so"] += 1
                    continue
                if a.sem and any(n.startswith(q) for q in a.sem.split(",")):
                    cont["deixada de fora (--sem)"] += 1
                    continue
                ceu = n.startswith("textures/vr_")
                if ceu and a.sem_ceu:
                    cont["ceu deixado de fora (--sem-ceu)"] += 1
                    continue
                ver = struct.unpack("<I", d[8:12])[0]
                body = d[0x40:]
                t, hw, hh = struct.unpack("<III", body[:12])
                if ver >= 1:
                    size = struct.unpack("<I", body[24:28])[0]
                    pix = body[28:28 + size]
                else:
                    size = struct.unpack("<I", body[12:16])[0]
                    pix = body[16:16 + size]
                hfmt = OTEXFMT.get(t)
                if len(pix) == hw * hh * 4:
                    # as texturas grandes do SoH vem sempre em RGBA de 32 bits (o tipo e o da original)
                    hfmt = "RGBA32"
                if hfmt is None or hfmt.startswith("CI"):
                    cont["formato da nova nao suportado (paleta)"] += 1
                    continue
                if hw % ow or hh % oh or hw // ow != hh // oh:
                    cont["escala nao inteira ou diferente em x e y"] += 1
                    continue
                base = vbase(base)
                orig_file = rom.arquivo(base) if base is not None else None
                fmt, siz = FMTS[ofmt]
                obytes = ow * oh * BITS[siz] // 8
                if orig_file is None or off + obytes > len(orig_file):
                    cont["original fora da ROM"] += 1
                    continue
                crc = zlib.crc32(orig_file[off:off + obytes]) & 0xFFFFFFFF
                img = decodifica(hfmt, hw, hh, pix)
                if img is None:
                    cont["nao decodificou"] += 1
                    continue
                k = min(a.escala, hw // ow)
                img = reduz(img, ow * k, oh * k)
                if ceu:
                    img = arruma_bordas(img)
                    cont["ceu: bordas arrumadas"] += 1
                o = original(n, ofmt, ow, oh, orig_file, off, obytes)
                if o is not None:
                    mo, mh = o[..., 3] > 0, img[..., 3] > 0
                    if mo.any() and mh.any():
                        ar = areas[area_de(n)]
                        ar[0] += 1
                        ar[1] += float(o[mo][:, :3].mean())
                        ar[2] += float(img[mh][:, :3].mean())
                # 2D (HUD, menus, letreiros, o logo da abertura): a arte nova ja vem no brilho certo; o ajuste
                # automatico e so para o que e 3D (cenarios e objetos), que o mod pintou para a luz do 3DS
                eh3d = n.startswith("scenes/") or (n.startswith("objects/") and not n.startswith("objects/object_mag/")
                                                    and "TitleCard" not in n)
                if a.brilho != "1" and (eh3d or not a.brilho.startswith("auto")):
                    f = 1.0
                    if a.brilho.startswith("auto"):
                        o = original(n, ofmt, ow, oh, orig_file, off, obytes)
                        if o is not None:
                            mo = o[..., 3] > 0
                            mh = img[..., 3] > 0
                            lo = o[mo][:, :3].mean() if mo.any() else 0
                            lh = img[mh][:, :3].mean() if mh.any() else 0
                            if lo > 8 and lh > 8:
                                f = min(AUTO_MAX, max(0.8, lo / lh))
                            brilho_tsv.append("%s\t%s\t%.1f\t%.1f\t%.2f\t%.2f" % (n, ofmt, lo, lh, lo / lh if lh > 0 else 0, f))
                            if ceu:
                                # ceu: um brilho so para todas as faces do mesmo ceu (senao uma face fica mais
                                # escura que a vizinha: um "retangulo" no ceu). Termina depois de ler todas.
                                ceu_pend.setdefault(n.split("/")[1], {})[base + off] = \
                                    (crc, fmt, siz, ow, oh, k, img, float(lo), float(lh))
                                continue
                        if "*" in a.brilho:
                            f *= float(a.brilho.split("*", 1)[1])  # auto*1.1: igual a original e mais 10%
                    else:
                        f = float(a.brilho)
                    if f != 1.0:
                        img = img.copy()
                        img[..., :3] = np.clip(img[..., :3].astype(np.float32) * f + 0.5, 0, 255).astype(np.uint8)
                        cont["brilho ajustado"] += 1
                if k == 1:
                    cont["mesma resolucao (arte nova)"] += 1
                else:
                    cont["reduzida para %dx" % k] += 1
                if a.amostras and amostras < 12 and k > 1:
                    os.makedirs(a.amostras, exist_ok=True)
                    o = original(n, ofmt, ow, oh, orig_file, off, obytes)
                    nm = n.replace("/", "_")
                    Image.fromarray(img, "RGBA").save(os.path.join(a.amostras, nm + "_nova.png"))
                    if o is not None:
                        Image.fromarray(o, "RGBA").resize((ow * k, oh * k), Image.NEAREST).save(
                            os.path.join(a.amostras, nm + "_original.png"))
                    amostras += 1
                c = zlib.compressobj(9, zlib.DEFLATED, -15)
                dados = c.compress(np.ascontiguousarray(img).tobytes()) + c.flush()
                entradas[base + off] = (crc, fmt, siz, ow, oh, ow * k, oh * k, dados)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for pasta, faces in sorted(ceu_pend.items()):
        so = sum(v[7] for v in faces.values())
        sh = sum(v[8] for v in faces.values())
        f = min(AUTO_MAX, max(0.8, so / sh)) if so > 8 and sh > 8 else 1.0
        if "*" in a.brilho:
            f *= float(a.brilho.split("*", 1)[1])
        print("  ceu %-40s %d faces, brilho x%.2f" % (pasta, len(faces), f))
        for kk, (crc, fmt, siz, ow, oh, k, img, lo, lh) in faces.items():
            if f != 1.0:
                img = img.copy()
                img[..., :3] = np.clip(img[..., :3].astype(np.float32) * f + 0.5, 0, 255).astype(np.uint8)
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            entradas[kk] = (crc, fmt, siz, ow, oh, ow * k, oh * k, c.compress(np.ascontiguousarray(img).tobytes()) + c.flush())

    # arquivo: cabecalho, indice ordenado pelo endereco (vrom), dados
    chaves = sorted(entradas)
    hdr = b"OOTHD01\0" + struct.pack("<II", len(chaves), 0)
    ent_size = 32
    pos = len(hdr) + ent_size * len(chaves)
    ind = bytearray()
    blob = bytearray()
    for kk in chaves:
        crc, fmt, siz, ow, oh, hw, hh, dados = entradas[kk]
        ind += struct.pack("<IIBBHHHHHII", kk, crc, fmt, siz, ow, oh, hw, hh, 0, pos + len(blob), len(dados))
        ind += struct.pack("<I", hw * hh * 4)
        blob += dados
    with open(a.saida, "wb") as f:
        f.write(hdr)
        f.write(ind)
        f.write(blob)
    for k, v in cont.most_common():
        print("  %-45s %6d" % (k, v))
    if areas:
        print("brilho medio por area (original / nova, antes do ajuste; > 1 = a nova e mais escura):")
        lst = sorted(areas.items(), key=lambda kv: -kv[1][0])
        mostra = lst[:25] + [kv for kv in lst[25:] if "spot00" in kv[0]]
        for nome_a, (nn, so, sh) in mostra:
            print("  %-48s %5d texturas  %6.1f / %6.1f  = %.2f" % (nome_a, nn, so / nn, sh / nn, so / max(sh, 1e-6)))
        tot = [sum(v[i] for v in areas.values()) for i in range(3)]
        print("  %-48s %5d texturas  %6.1f / %6.1f  = %.2f" % ("(tudo)", tot[0], tot[1] / tot[0], tot[2] / tot[0],
                                                              tot[1] / max(tot[2], 1e-6)))
    if brilho_tsv:
        tsvb = os.path.splitext(a.saida)[0] + "_brilho.tsv"
        with open(tsvb, "w", encoding="utf-8") as fb:
            fb.write("# nome\tformato\tbrilho original\tbrilho nova\toriginal/nova\tajuste\n")
            fb.write("\n".join(sorted(brilho_tsv)) + "\n")
        print("brilho de cada textura: %s" % tsvb)
    print("gravado: %s (%d texturas, %.1f MB)" % (a.saida, len(chaves), os.path.getsize(a.saida) / 1e6))


main()
