# -*- coding: utf-8 -*-
# Strict check of the hooked functions in another edition: every "lis rX,HIGH" + instruction pair with
# base rX (addi, ori, loads and stores) must compute, in the other edition, the exact translation of the
# Spanish address. verificar_ganchos.py accepts any value in those pairs (they are addresses); here it is
# checked that they are the right address:
#   .text  -> the translation by anchors;
#   .rdata -> the same 16 bytes in both editions;
#   .data  -> the translation by references (the edition's desplazamientos_datos.json) or the same
#             address if the edition does not have it (.data unmoved).
# Usage: verificar_parejas.py <PAL image> <other image> <table.tsv> <PAL partition> <addresses>
#        [desplazamientos.json]
import bisect, json, struct, sys
import numpy as np
from emparejar import BASE, anclas, huellas, normalizar, secciones


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
    datos = {int(k, 16): v for k, v in json.load(open(sys.argv[6])).items()} if len(sys.argv) > 6 else {}
    claves_datos = sorted(datos)
    sa, sb = secciones(img_a), secciones(img_b)
    ta = next(s for s in sa if s[0] == '.text')
    tb = next(s for s in sb if s[0] == '.text')
    wa = np.frombuffer(img_a[ta[1] - BASE:ta[2] - BASE], dtype='>u4').astype(np.uint32)
    wb = np.frombuffer(img_b[tb[1] - BASE:tb[2] - BASE], dtype='>u4').astype(np.uint32)
    cadena = anclas(huellas(normalizar(wa)), huellas(normalizar(wb)))
    pos = [a for a, _ in cadena]

    def seccion(secs, d):
        return next((s[0] for s in secs if s[1] <= d < s[2]), 'fuera')

    def traducir(d):
        s = seccion(sa, d)
        if s == '.text':
            i = (d - ta[1]) // 4
            k = bisect.bisect_right(pos, i) - 1
            return tb[1] + 4 * (i + cadena[k][1] - cadena[k][0]) + (d & 3) if k >= 0 else None
        if s == '.data':
            if not datos:
                return d
            if d in datos:
                return d + datos[d]
            k = bisect.bisect_right(claves_datos, d)
            if 0 < k < len(claves_datos) and datos[claves_datos[k - 1]] == datos[claves_datos[k]]:
                return d + datos[claves_datos[k - 1]]
            return None
        return None  # .rdata and the rest: by contents

    def palabra(img, d):
        return struct.unpack_from('>I', img, d - BASE)[0]

    def baja(x):
        op = x >> 26
        if op == 24:
            return x & 0xFFFF
        if op in (58, 62):
            return struct.unpack('>h', struct.pack('>H', x & 0xFFFC))[0]
        return struct.unpack('>h', struct.pack('>H', x & 0xFFFF))[0]

    malas = parejas = 0
    for d in pedidas:
        if not (ta[1] <= d < ta[2]) or d not in mapa:
            continue
        i = bisect.bisect_right(inicios, d)
        ini = inicios[i - 1] if i else d
        fin = inicios[i] if i < len(inicios) else ta[2]
        e = mapa[d] - (d - ini)
        alta_a, alta_b = {}, {}
        problemas = []
        for k in range(0, fin - ini, 4):
            a, b = palabra(img_a, ini + k), palabra(img_b, e + k)
            op = a >> 26
            if op == 15 and (a >> 16) & 31 == 0:
                alta_a[(a >> 21) & 31] = (a & 0xFFFF) << 16
                if b >> 26 == 15 and (b >> 16) & 31 == 0:
                    alta_b[(b >> 21) & 31] = (b & 0xFFFF) << 16
                continue
            if op in (14, 24, 58, 62) or 32 <= op <= 55:
                base = (a >> 21) & 31 if op == 24 else (a >> 16) & 31
                if base in alta_a and base in alta_b and base != 0:
                    da = (alta_a[base] + baja(a)) & 0xFFFFFFFF
                    db = (alta_b[base] + baja(b)) & 0xFFFFFFFF
                    parejas += 1
                    s = seccion(sa, da)
                    if s in ('.text', '.data'):
                        esperada = traducir(da)
                        if esperada is None:
                            problemas.append('+%X: %08X (%s) sin traduccion segura; la otra usa %08X' % (k, da, s, db))
                        elif esperada != db:
                            problemas.append('+%X: %08X (%s) deberia ser %08X y la otra usa %08X' % (k, da, s, esperada, db))
                    elif s != 'fuera':
                        # constants: the same contents
                        if img_a[da - BASE:da - BASE + 16] != img_b[db - BASE:db - BASE + 16]:
                            problemas.append('+%X: %08X (%s) con otro contenido en %08X' % (k, da, s, db))
            # any write to the base register ends the pair (approximate: destination in bits 21-25)
            if op not in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62) and op != 24:
                rd = (a >> 21) & 31
                alta_a.pop(rd, None)
                alta_b.pop((b >> 21) & 31, None)
            elif op == 24:
                alta_a.pop((a >> 16) & 31, None)
                alta_b.pop((b >> 16) & 31, None)
        if problemas:
            malas += 1
            print('%08X (funcion %08X -> %08X): %s' % (d, ini, e, '; '.join(problemas[:5])))
    print('parejas comprobadas: %d; funciones con problemas: %d de %d' % (parejas, malas, len(pedidas)))


if __name__ == '__main__':
    main()
