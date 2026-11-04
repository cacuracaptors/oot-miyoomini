"""elfmini.py: leitor minimo de ELF32 (big ou little endian), so o que o port precisa:
secoes, simbolos e relocacoes (REL e RELA). Sem dependencias."""
import struct

SHT_PROGBITS, SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 1, 2, 4, 8, 9
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4
SHN_ABS = 0xFFF1


class Section:
    __slots__ = ("idx", "name", "type", "flags", "addr", "off", "size", "link", "info", "entsize")


class Elf:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 1:
            raise ValueError("nao e ELF32: " + path)
        self.path = path
        self.data = d
        self.e = e = ">" if d[5] == 2 else "<"
        shoff, = struct.unpack_from(e + "I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", d, 0x2E)
        self.sections = []
        for i in range(shnum):
            f = struct.unpack_from(e + "IIIIIIIIII", d, shoff + i * shentsize)
            s = Section()
            s.idx = i
            s.name = f[0]
            s.type, s.flags, s.addr, s.off, s.size, s.link, s.info, _, s.entsize = f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]
            self.sections.append(s)
        if shnum:
            stroff = self.sections[shstrndx].off
            for s in self.sections:
                s.name = self._cstr(stroff + s.name)
        self._syms = None

    def _cstr(self, off):
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def section_bytes(self, s):
        if s.type == SHT_NOBITS:
            return bytes(s.size)
        return self.data[s.off:s.off + s.size]

    def symbols(self):
        """lista de (nome, valor, tamanho, tipo, bind, shndx)"""
        if self._syms is not None:
            return self._syms
        e, d = self.e, self.data
        out = []
        for s in self.sections:
            if s.type != SHT_SYMTAB:
                continue
            stroff = self.sections[s.link].off
            for k in range(s.size // 16):
                n, val, size, info, other, shndx = struct.unpack_from(e + "IIIBBH", d, s.off + k * 16)
                if n == 0:
                    continue
                out.append((self._cstr(stroff + n), val, size, info & 0xF, info >> 4, shndx))
        self._syms = out
        return out

    def relocs_for(self, sec):
        """deslocamentos (dentro da secao `sec`) que tem relocacao"""
        e, d = self.e, self.data
        offs = []
        for s in self.sections:
            if s.type in (SHT_REL, SHT_RELA) and s.info == sec.idx:
                ent = 8 if s.type == SHT_REL else 12
                for k in range(s.size // ent):
                    off, = struct.unpack_from(e + "I", d, s.off + k * ent)
                    offs.append(off)
        return offs
