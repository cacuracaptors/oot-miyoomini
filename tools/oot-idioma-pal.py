#!/usr/bin/env python3
"""oot-idioma-pal.py: gera port/lang_pal.h, a tabela que o jogo usa para tirar o alemao e o frances da ROM europeia.

uso: python3 tools/oot-idioma-pal.py CAMINHO_DO_DECOMP > port/lang_pal.h

O jogo (port/lang_pal.c), na primeira vez que acha a ROM europeia (PAL 1.1, "Europe (En,Fr,De) (Rev A)", pode ser o
.zip) na pasta, monta idiomas/de.oot e idiomas/fr.oot no mesmo formato do pacote do portugues (port/lang.c):
  - texturas com texto: cada textura em ingles do cartucho americano recebe a alema/francesa da ROM europeia quando
    elas tem o mesmo formato e tamanho (nomes, offsets e tamanhos dos XML do decomp: assets/xml/textures e objects);
    nomes de itens e botao de acao (A) vao em blocos inteiros; titulos dos chefes (128x40 por idioma) a parte;
  - a fonte da ROM europeia (2 letras redesenhadas: I com circunflexo e o sz alemao);
  - os textos das falas (tabelas de mensagens do codigo europeu, achadas na hora pelo padrao da primeira entrada);
  - 3 rotulos do menu de pausa que sao mais largos em alemao/frances ("To Equip", "To Decide", "To Play Melody"):
    vao como texturas extras, com a largura certa (o z_kaleido_scope.c ajustado pede ao pacote).
Texturas que nao existem na mesma medida (algumas da tela de opcoes da escolha de arquivo) ficam em ingles.
Nenhum byte do jogo vai neste arquivo: so posicoes e tamanhos.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

BPP = {'ia4': 0.5, 'i4': 0.5, 'ci4': 0.5, 'ia8': 1, 'i8': 1, 'ci8': 1, 'ia16': 2, 'rgba16': 2, 'rgba32': 4}
# nomes que o decomp escreve diferente na versao europeia
ALIAS = {('gBombItemNameENGTex', 'GER'): 'gBombItemNameItemNameGERTex',
         ('gFireArrowItemNameENGTex', 'GER'): 'gFireArrowItemNameUnk2GERTex'}
# arquivos trocados em blocos (o ingles do cartucho americano e o 2o bloco; na europeia: ingles, alemao, frances)
BLOCKS = ('item_name_static', 'do_action_static')
EXTRA = ('gPauseToEquip', 'gPauseToDecide', 'gPauseToPlayMelody')


def off(o):
    try:
        return int(o, 16)
    except (TypeError, ValueError):
        return -1


def parse(path, ver):
    out = {}
    root = ET.parse(path).getroot()

    def walk(el, fname):
        for c in el:
            if c.tag == 'File':
                out.setdefault(c.get('Name'), [])
                walk(c, c.get('Name'))
            elif c.tag == 'Version':
                if re.fullmatch(c.get('Pattern'), ver):
                    walk(c, fname)
            elif c.tag == 'Texture':
                out[fname].append((c.get('Name'), c.get('Format'), int(c.get('Width')), int(c.get('Height')),
                                   off(c.get('Offset'))))
            else:
                walk(c, fname)
    walk(root, None)
    return out


def collect(X, ver, pal):
    res = {}
    for sub in ('textures', 'objects'):
        d = os.path.join(X, sub)
        for b in sorted(os.listdir(d)):
            if not b.endswith('.xml'):
                continue
            p = os.path.join(d, b)
            ispal = '_pal' in b
            if 'ique' in b or 'v2' in b or '_gc' in b or b.startswith('icon_item_jpn') or b.startswith('kanji'):
                continue
            if not pal and ispal:
                continue
            if pal and not ispal and (os.path.exists(p.replace('.xml', '_pal.xml')) or
                                      os.path.exists(p.replace('.xml', '_pal_n64.xml'))):
                continue
            for f, l in parse(p, ver).items():
                res.setdefault(f, []).extend(l)
    return res


def names(oot, ver):
    rows = open(os.path.join(oot, 'baseroms', ver, 'segments.csv')).read().split('\n')[1:]
    return [r.split(',')[0] for r in rows if r.strip()]


def build(oot):
    X = os.path.join(oot, 'assets', 'xml')
    N = collect(X, 'ntsc-1.1', False)
    P = collect(X, 'pal-1.1', True)
    nn, pn = names(oot, 'ntsc-1.1'), names(oot, 'pal-1.1')
    nidx = {n: i for i, n in enumerate(nn)}
    pidx = {n: i for i, n in enumerate(pn)}
    ptex = {}
    for f, l in P.items():
        for t in l:
            ptex[t[0]] = (f,) + t[1:]
    copies = []   # (arquivo americano, offset, tamanho, arquivo europeu, offset alemao, offset frances, nome)
    for f in BLOCKS:
        nf = [t for t in N[f]]
        size = max(t[4] + int(t[2] * t[3] * BPP[t[1]]) for t in nf) // 2
        copies.append((f, size, size, f, size, 2 * size, f + ' (bloco)'))
    for f, l in sorted(N.items()):
        if f not in nidx or f in BLOCKS:
            continue
        for name, fmt, w, h, o in l:
            if 'TitleCard' in name and f.startswith('object_'):
                if h == 80:  # japones e ingles (128x40 cada); na europeia 128x120: ingles, alemao, frances
                    pf, _, _, _, po = ptex[name]
                    blk = 128 * 40
                    copies.append((f, o + blk, blk, pf, po + blk, po + 2 * blk, name))
                continue
            if 'ENG' not in name or name in [e + 'ENGTex' for e in EXTRA]:
                continue
            size = int(w * h * BPP[fmt])
            lo = []
            for L in ('GER', 'FRA'):
                g = ptex.get(ALIAS.get((name, L), name.replace('ENG', L)))
                lo.append(g if g is not None and g[1:4] == (fmt, w, h) else None)
            if lo[0] is None and lo[1] is None:
                continue
            pf = (lo[0] or lo[1])[0]
            if lo[0] is not None and lo[1] is not None and lo[0][0] != lo[1][0]:
                # alemao e frances em arquivos diferentes (icon_item_ger_static / icon_item_fra_static): 2 linhas
                copies.append((f, o, size, lo[0][0], lo[0][4], -1, name))
                copies.append((f, o, size, lo[1][0], -1, lo[1][4], name))
                continue
            copies.append((f, o, size, pf, lo[0][4] if lo[0] else -1, lo[1][4] if lo[1] else -1, name))
    # fonte inteira
    copies.append(('nes_font_static', 0, 17792, 'nes_font_static', 0, 0, 'fonte'))
    extras = []
    for e in EXTRA:
        g, fr = ptex[e + 'GERTex'], ptex[e + 'FRATex']
        extras.append((e, g, fr))
    return copies, extras, nidx, pidx


def emit(copies, extras, nidx, pidx):
    o = []
    o.append('/* gerado por tools/oot-idioma-pal.py (nao editar): posicoes das texturas/textos alemaes e franceses da ROM')
    o.append(' * europeia (PAL 1.1) que entram no lugar do ingles do cartucho americano (port/lang_pal.c). */')
    o.append('#define LANG_PAL_DMADATA 0x7950u')
    for n in ('code', 'ger_message_data_static', 'fra_message_data_static'):
        o.append('#define LANG_PAL_F_%s %d' % (n.upper(), pidx[n]))
    o.append('typedef struct {')
    o.append('    uint16_t nf, pf;  /* arquivo (indice na dmadata): americano, europeu */')
    o.append('    uint32_t noff, len;')
    o.append('    int32_t poff[2];  /* no europeu: alemao, frances (-1 = nao tem: fica o ingles) */')
    o.append('} LangPalCopy;')
    o.append('static const LangPalCopy k_langPalCopy[] = {')
    for f, no, ln, pf, g, fr, name in copies:
        o.append('    { %d, %d, 0x%X, 0x%X, { %d, %d } }, /* %s */' % (nidx[f], pidx[pf], no, ln, g, fr, name))
    o.append('};')
    o.append('/* rotulos do menu de pausa mais largos que o ingles (ia8, 16 de altura): Port_LangExtraTex(0..2) */')
    o.append('typedef struct {')
    o.append('    uint16_t pf[2];')
    o.append('    uint32_t poff[2];')
    o.append('    uint16_t w[2];')
    o.append('} LangPalExtra;')
    o.append('static const LangPalExtra k_langPalExtra[] = {')
    for e, g, fr in extras:
        o.append('    { { %d, %d }, { 0x%X, 0x%X }, { %d, %d } }, /* %s */' % (pidx[g[0]], pidx[fr[0]], g[4], fr[4], g[2], fr[2], e))
    o.append('};')
    return '\n'.join(o) + '\n'


if __name__ == '__main__':
    copies, extras, nidx, pidx = build(sys.argv[1])
    sys.stdout.write(emit(copies, extras, nidx, pidx))
