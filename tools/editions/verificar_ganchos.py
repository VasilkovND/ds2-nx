# -*- coding: utf-8 -*-
# Checks that the functions we hook or reimplement natively are identical in another edition, from start
# to end and without normalizing structure offsets.
#
# emparejar.py only compares 16 instructions and removes every 16-bit immediate, including the one in
# "lwz r3,0x854(r31)", which is a structure field. A native function depends on those fields. Here whole
# functions are compared (up to the next function in the codegen partition) with these rules:
#   - identical words: fine;
#   - branches (b/bl/bc): the translated target must match the other edition's;
#   - lis: same register (the high part of an address);
#   - addi/ori/loads/stores with a different immediate: only if the base register comes from a lis in
#     the same function (the low part of an absolute address);
#   - data words inside the function (jump tables): if they are .text addresses, translated.
# Everything else is reported.
#
# Usage: verificar_ganchos.py <PAL image> <other image> <table.tsv of all addresses> <PAL partition>
#        <addresses>
import bisect, json, struct, sys
import numpy as np
from emparejar import secciones, BASE


def main():
    img_a = open(sys.argv[1], 'rb').read()
    img_b = open(sys.argv[2], 'rb').read()
    mapa = {}
    for linea in open(sys.argv[3], encoding='utf-8').read().splitlines()[1:]:
        pal, sec, otra, est = linea.split('\t')
        if otra != '-':
            mapa[int(pal, 16)] = int(otra, 16)
    inicios = sorted(int(a, 16) for a in json.load(open(sys.argv[4]))['assignments'])
    pedidas = sorted({int(l.split()[0], 16) for l in open(sys.argv[5]) if l.strip()})
    ta = next(s for s in secciones(img_a) if s[0] == '.text')
    tb = next(s for s in secciones(img_b) if s[0] == '.text')

    # translation of any .text address with the nearest anchor (the one of the function that contains it)
    claves = sorted(k for k in mapa if ta[1] <= k < ta[2])

    def traducir(d):
        k = bisect.bisect_right(claves, d) - 1
        return mapa[claves[k]] + (d - claves[k]) if k >= 0 else None

    def palabra(img, d):
        return struct.unpack_from('>I', img, d - BASE)[0]

    def destino(w, d):
        op = w >> 26
        if op == 18:
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            return (li if w & 2 else d + li) & 0xFFFFFFFF
        bd = w & 0xFFFC
        bd = bd - 0x10000 if bd & 0x8000 else bd
        return (bd if w & 2 else d + bd) & 0xFFFFFFFF

    malas = 0
    for d in pedidas:
        if not (ta[1] <= d < ta[2]) or d not in mapa:
            continue
        i = bisect.bisect_right(inicios, d)
        fin = inicios[i] if i < len(inicios) else ta[2]
        # the function that contains the address (a hook can be in the middle of a function)
        ini = inicios[i - 1] if i else d
        e = mapa[d] - (d - ini)
        lis = set()
        problemas = []
        for k in range(0, fin - ini, 4):
            a, b = palabra(img_a, ini + k), palabra(img_b, e + k)
            op = a >> 26
            if a == b:
                if op == 15 and (a >> 16) & 31 == 0:
                    lis.add((a >> 21) & 31)
                continue
            if op != b >> 26:
                # data word: a translated .text address
                if ta[1] <= a < ta[2] and traducir(a) == b:
                    continue
                problemas.append('+%X: %08X / %08X' % (k, a, b))
                continue
            if op in (16, 18):
                if (a & ~0x03FFFFFC & 0xFFFFFFFF) == (b & ~0x03FFFFFC & 0xFFFFFFFF) if op == 18 else (a & 0xFFFF0003) == (b & 0xFFFF0003):
                    if traducir(destino(a, ini + k)) == destino(b, e + k):
                        continue
                problemas.append('+%X: salto %08X / %08X' % (k, a, b))
                continue
            if (a & 0xFFFF0000) == (b & 0xFFFF0000):
                if op == 15 and (a >> 16) & 31 == 0:
                    lis.add((a >> 21) & 31)
                    continue
                base = (a >> 16) & 31
                if op in (14, 24) or 32 <= op <= 55:
                    if base in lis:
                        continue
                problemas.append('+%X: inmediato %08X / %08X' % (k, a, b))
                continue
            problemas.append('+%X: %08X / %08X' % (k, a, b))
        if problemas:
            malas += 1
            print('%08X (funcion %08X, %d bytes) -> %08X: %d diferencias; %s' % (
                d, ini, fin - ini, mapa[d], len(problemas), '; '.join(problemas[:6])))
    print('funciones comprobadas: %d, con diferencias: %d' % (len(pedidas), malas))


if __name__ == '__main__':
    main()
