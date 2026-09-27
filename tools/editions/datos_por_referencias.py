# -*- coding: utf-8 -*-
# Translates .data (and .rdata) addresses to another edition by following the instructions that compute
# them.
#
# emparejar.py assumes .data does not move, and for Spanish PAL, USA and English PAL that holds (same
# address and same size). In the Japanese edition the section grows by 0x5A0 bytes, so that no longer
# works. Here, for each address D, the Spanish code is searched for "lis rX,HIGH" + instruction pairs
# with base rX and immediate LOW (addi, ori, loads and stores) that compute D; they are located in the
# other edition with the emparejar.py anchors, and the address the same pair computes there is read. The
# majority wins. Without direct references, the offset of the nearest neighbors is used.
#
# Usage: datos_por_referencias.py <PAL image> <other image> <addresses...>   (or from stdin, one per line)
import bisect, collections, struct, sys
import numpy as np
from emparejar import BASE, anclas, huellas, normalizar, secciones

CARGAS = set(range(32, 56)) | {14}  # addi and the D-form loads/stores


def palabras(img, sec):
    return np.frombuffer(img[sec[1] - BASE:sec[2] - BASE], dtype='>u4').astype(np.uint32)


def referencias(w, t0):
    """{address: [(lis index, low part index)]} for all of .text"""
    out = collections.defaultdict(list)
    n = len(w)
    for i in np.nonzero(((w >> 26) == 15) & (((w >> 16) & 31) == 0))[0]:
        rd = int((w[i] >> 21) & 31)
        alta = int(w[i] & 0xFFFF) << 16
        for j in range(i + 1, min(i + 9, n)):
            x = int(w[j])
            op = x >> 26
            if op in CARGAS or op == 24 or op in (58, 62):
                base = (x >> 21) & 31 if op == 24 else (x >> 16) & 31
                if base == rd:
                    if op == 24:
                        baja = x & 0xFFFF
                    elif op in (58, 62):
                        baja = struct.unpack('>h', struct.pack('>H', x & 0xFFFC))[0]
                    else:
                        baja = struct.unpack('>h', struct.pack('>H', x & 0xFFFF))[0]
                    out[(alta + baja) & 0xFFFFFFFF].append((int(i), j))
            # if the instruction writes rd (destination in bits 21-25 for almost all of them), the pair ends
            if op in (14, 15, 24, 31) or op in range(32, 48, 2):
                if (x >> 21) & 31 == rd and not (op == 24 and (x >> 21) & 31 == rd and (x >> 16) & 31 != rd):
                    if not (op in CARGAS and op >= 36 and op not in (40, 41, 42, 43, 46, 48, 49, 50, 51)):
                        break
    return out


def direccion_en(w, i, j):
    """the address computed by the pair lis w[i] + w[j] (or None if it is not a pair)"""
    if i < 0 or j >= len(w):
        return None
    a, x = int(w[i]), int(w[j])
    if a >> 26 != 15 or (a >> 16) & 31:
        return None
    rd = (a >> 21) & 31
    op = x >> 26
    base = (x >> 21) & 31 if op == 24 else (x >> 16) & 31
    if base != rd:
        return None
    if op == 24:
        baja = x & 0xFFFF
    elif op in (58, 62):
        baja = struct.unpack('>h', struct.pack('>H', x & 0xFFFC))[0]
    else:
        baja = struct.unpack('>h', struct.pack('>H', x & 0xFFFF))[0]
    return (((a & 0xFFFF) << 16) + baja) & 0xFFFFFFFF


def main():
    img_a = open(sys.argv[1], 'rb').read()
    img_b = open(sys.argv[2], 'rb').read()
    pedidas = [int(x, 16) for x in sys.argv[3:]] or [int(l.split()[0], 16) for l in sys.stdin if l.strip()]
    ta = next(s for s in secciones(img_a) if s[0] == '.text')
    tb = next(s for s in secciones(img_b) if s[0] == '.text')
    wa, wb = palabras(img_a, ta), palabras(img_b, tb)
    cadena = anclas(huellas(normalizar(wa)), huellas(normalizar(wb)))
    pos = [a for a, _ in cadena]

    def traducir_indice(i):
        k = bisect.bisect_right(pos, i) - 1
        return i + (cadena[k][1] - cadena[k][0]) if k >= 0 else None

    refs = referencias(wa, ta[1])
    conocidas = sorted(refs)
    for d in pedidas:
        votos = collections.Counter()
        for i, j in refs.get(d, [])[:64]:
            i2, j2 = traducir_indice(i), traducir_indice(j)
            if i2 is None or j2 is None or j2 - i2 != j - i:
                continue
            otra = direccion_en(wb, i2, j2)
            if otra is not None:
                votos[otra] += 1
        if votos:
            mejor, n = votos.most_common(1)[0]
            total = sum(votos.values())
            print('%08X\t%08X\t%+d\tdirecta %d/%d%s' % (d, mejor, mejor - d, n, total,
                                                       '' if n == total else ' (otras: %s)' % dict(votos)))
            continue
        # no direct references: the offset of the neighbors that have references
        k = bisect.bisect_left(conocidas, d)
        vecinas = [v for v in conocidas[max(0, k - 3):k + 3] if abs(v - d) < 0x400]
        despl = collections.Counter()
        for v in vecinas:
            for i, j in refs[v][:16]:
                i2, j2 = traducir_indice(i), traducir_indice(j)
                if i2 is not None and j2 is not None and j2 - i2 == j - i:
                    o = direccion_en(wb, i2, j2)
                    if o is not None:
                        despl[o - v] += 1
        if despl:
            s, n = despl.most_common(1)[0]
            print('%08X\t%08X\t%+d\tvecinas %d/%d (%s)' % (d, d + s, s, n, sum(despl.values()),
                                                         ', '.join('%08X' % v for v in vecinas)))
        else:
            print('%08X\t-\t-\tsin referencias' % d)


if __name__ == '__main__':
    main()
