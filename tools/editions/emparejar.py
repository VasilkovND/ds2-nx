# -*- coding: utf-8 -*-
# Translates addresses of the Spanish PAL executable to those of another edition (first, the USA one).
#
# .text: PowerPC instructions are normalized by removing what depends on position (branch offsets, 16-bit
# immediates of lis/addi/ori and of loads and stores), runs of 12 instructions that appear only once in
# each edition (anchors) are searched for, those in the same order are kept, and each address is
# translated with the offset of its previous anchor. Each translation is verified by comparing 16
# normalized instructions.
# .rdata: the contents (32 bytes) are searched for in the other edition; they must appear exactly once.
# .data: same address if the surrounding bytes match (the section does not move between PAL and USA).
#
# Usage: emparejar.py <PAL image> <other image> <output.tsv> [addresses...]
import bisect, struct, sys
import numpy as np

BASE = 0x82000000
K = 12
VERIFICAR = 16


def secciones(img):
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    n = struct.unpack_from('<H', img, pe + 6)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    out = []
    for i in range(n):
        at = pe + 24 + opt + i * 40
        nombre = img[at:at + 8].rstrip(b'\0').decode()
        vsize, vaddr = struct.unpack_from('<II', img, at + 8)
        out.append((nombre, BASE + vaddr, BASE + vaddr + vsize))
    return out


def normalizar(words):
    op = words >> 26
    w = words.copy()
    w[op == 18] &= np.uint32(0xFC000003)
    w[op == 16] &= np.uint32(0xFFFF0003)
    imm = np.isin(op, [14, 15, 24, 25] + list(range(32, 56)))
    w[imm] &= np.uint32(0xFFFF0000)
    return w


def huellas(w):
    # fingerprint of K consecutive instructions, per position
    h = np.zeros(len(w) - K + 1, dtype=np.uint64)
    for j in range(K):
        h = h * np.uint64(1000003) ^ w[j:len(w) - K + 1 + j].astype(np.uint64)
    return h


def anclas(ha, hb):
    ua, ia, ca = np.unique(ha, return_index=True, return_counts=True)
    ub, ib, cb = np.unique(hb, return_index=True, return_counts=True)
    ua, ia = ua[ca == 1], ia[ca == 1]
    ub, ib = ub[cb == 1], ib[cb == 1]
    comunes, pa, pb = np.intersect1d(ua, ub, return_indices=True)
    pares = sorted(zip(ia[pa].tolist(), ib[pb].tolist()))
    # the longest increasing subsequence in the other edition: anchors in the same order
    colas, prev, idx = [], [-1] * len(pares), []
    for i, (_, b) in enumerate(pares):
        k = bisect.bisect_left(colas, b)
        if k == len(colas):
            colas.append(b); idx.append(i)
        else:
            colas[k] = b; idx[k] = i
        prev[i] = idx[k - 1] if k else -1
    cadena, i = [], idx[-1] if idx else -1
    while i >= 0:
        cadena.append(pares[i]); i = prev[i]
    return cadena[::-1]


def main():
    img_a = open(sys.argv[1], 'rb').read()
    img_b = open(sys.argv[2], 'rb').read()
    salida = sys.argv[3]
    pedidas = [int(x, 16) for x in sys.argv[4:]] if len(sys.argv) > 4 else [int(l.split()[0], 16) for l in sys.stdin if l.strip()]
    sa, sb = secciones(img_a), secciones(img_b)
    ta = next(s for s in sa if s[0] == '.text')
    tb = next(s for s in sb if s[0] == '.text')
    wa = normalizar(np.frombuffer(img_a[ta[1] - BASE:ta[2] - BASE], dtype='>u4').astype(np.uint32))
    wb = normalizar(np.frombuffer(img_b[tb[1] - BASE:tb[2] - BASE], dtype='>u4').astype(np.uint32))
    cadena = anclas(huellas(wa), huellas(wb))
    pos_a = [a for a, _ in cadena]
    print('anclas en .text: %d (de %d instrucciones)' % (len(cadena), len(wa)))

    def en(secs, dir_):
        return next((s for s in secs if s[1] <= dir_ < s[2]), None)

    filas, cuenta = [], {}
    for d in sorted(set(pedidas)):
        s = en(sa, d)
        nombre = s[0] if s else 'fuera'
        destino, estado = None, 'sin traducir'
        if nombre == '.text':
            i = (d - ta[1]) // 4
            k = bisect.bisect_right(pos_a, i) - 1
            if k >= 0:
                j = i + (cadena[k][1] - cadena[k][0])
                if 0 <= j < len(wb):
                    destino = tb[1] + 4 * j
                    n = min(VERIFICAR, len(wa) - i, len(wb) - j)
                    estado = 'exacta' if np.array_equal(wa[i:i + n], wb[j:j + n]) else 'REVISAR'
        elif nombre == '.rdata':
            # 32 bytes and, if they appear several times (similar shader headers), larger windows
            rb = next(s for s in sb if s[0] == '.rdata')
            zona = img_b[rb[1] - BASE:rb[2] - BASE]
            estado = 'REVISAR (no aparece)'
            for ventana in (32, 64, 128, 256, 512, 1024):
                trozo = img_a[d - BASE:d - BASE + ventana]
                p = zona.find(trozo)
                if p < 0:
                    break
                if zona.find(trozo, p + 1) < 0:
                    destino, estado = rb[1] + p, 'exacta'
                    break
                estado = 'REVISAR (aparece varias veces)'
        elif nombre in ('.embsec_', '.no_bbt'):
            # embedded code: the section with the same order and size in the other edition; it must be identical
            orden = [x for x in sa if x[0] == nombre].index(s)
            t_b = [x for x in sb if x[0] == nombre][orden]
            if t_b[2] - t_b[1] == s[2] - s[1]:
                destino = t_b[1] + (d - s[1])
                na = normalizar(np.frombuffer(img_a[d - BASE:d - BASE + 4 * VERIFICAR], dtype='>u4').astype(np.uint32))
                nb = normalizar(np.frombuffer(img_b[destino - BASE:destino - BASE + 4 * VERIFICAR], dtype='>u4').astype(np.uint32))
                estado = 'exacta' if np.array_equal(na, nb) else 'REVISAR (instrucciones distintas)'
        elif d in (0x82000000, 0x82CD0000) or d >= 0x82D20000:
            destino, estado = d, 'constante (misma en las dos)'
        elif nombre == '.data' or nombre.startswith('.tls') or nombre.startswith('.idata'):
            igual = img_a[d - BASE - 16:d - BASE + 16] == img_b[d - BASE - 16:d - BASE + 16]
            destino, estado = d, 'misma direccion' + ('' if igual else ' (contenido distinto alrededor)')
        cuenta[nombre + ' ' + estado.split(' (')[0]] = cuenta.get(nombre + ' ' + estado.split(' (')[0], 0) + 1
        filas.append('%08X\t%s\t%s\t%s' % (d, nombre, '%08X' % destino if destino else '-', estado))
    open(salida, 'w', encoding='utf-8').write('pal\tseccion\totra\testado\n' + '\n'.join(filas) + '\n')
    for k, v in sorted(cuenta.items()):
        print('  %-40s %d' % (k, v))


if __name__ == '__main__':
    main()
