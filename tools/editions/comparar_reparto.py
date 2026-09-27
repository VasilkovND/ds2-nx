# -*- coding: utf-8 -*-
# Compares another edition's function partition with the Spanish one and proposes corrected pairs: every
# Spanish function whose seeded pair does not exist is paired, in order, with the most similar nearby new
# function (whole function, normalized). Usage: comparar_reparto.py <edition> <PAL image> <other image>
import bisect, json, os, sys
import numpy as np
from emparejar import BASE, normalizar

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + '/'
ed, img_pal, img_otra = sys.argv[1:4]
pal = json.load(open(R + 'app/generated/default/codegen.partition.json'))['assignments']
otra = json.load(open(R + 'app_%s/generated/default/codegen.partition.json' % ed))['assignments']
mapa = {}
for l in open(ed + '/todas.tsv', encoding='utf-8').read().splitlines()[1:]:
    p, s, o, e = l.split('\t')
    if o != '-':
        mapa[p] = o
try:
    mapa.update(json.load(open(ed + '/parejas_corregidas.json'))['parejas'])
except (OSError, KeyError):
    pass
seed = {}
origen = {}
for a, f in pal.items():
    o = mapa.get(a)
    if o and o not in seed:
        seed[o] = f
        origen[o] = a
nuevas = sorted(a for a in otra if a not in seed)
perdidas = sorted(a for a in seed if a not in otra)
ia, ib = open(img_pal, 'rb').read(), open(img_otra, 'rb').read()
pi, oi = sorted(int(a, 16) for a in pal), sorted(int(a, 16) for a in otra)


def largo(inis, d):
    i = bisect.bisect_right(inis, d)
    return (inis[i] - d) // 4 if i < len(inis) else 64


def cod(img, d, n):
    return normalizar(np.frombuffer(img[d - BASE:d - BASE + 4 * n], dtype='>u4').astype(np.uint32))


usadas = set()
parejas = {}
for L in perdidas:
    P = int(origen[L], 16)
    n = largo(pi, P)
    cp = cod(ia, P, n)
    mejor = None
    for N in nuevas:
        if N in usadas:
            continue
        d = int(N, 16)
        if abs(d - int(L, 16)) > 0x400:
            continue
        m = largo(oi, d)
        cn = cod(ib, d, m)
        k = min(n, m)
        igual = int((cp[:k] == cn[:k]).sum()) / max(n, m)
        if not mejor or igual > mejor[1] + 1e-9:
            mejor = (N, igual)
    if mejor and mejor[1] >= 0.6:
        parejas[origen[L]] = mejor[0]
        usadas.add(mejor[0])
    print('PAL %s (fichero %d) sembrada %s -> %s' % (origen[L], pal[origen[L]], L,
          '%s parecido %.2f' % mejor if mejor else 'sin candidata'))
sin = sorted(set(nuevas) - usadas)
print('parejas: %d; nuevas sin pareja: %s; perdidas sin pareja: %s' % (
    len(parejas), sin, sorted(origen[L] for L in perdidas if origen[L] not in parejas)))
json.dump({'parejas': parejas, 'nuevas_sin_pareja': sin}, open(ed + '/parejas_propuestas.json', 'w'), indent=1)
