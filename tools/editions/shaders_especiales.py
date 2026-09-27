# -*- coding: utf-8 -*-
# Finds in another edition the three shaders the renderer recognizes by fingerprint (bright pass, sky and
# composition) by comparing the microcode, and prints their XXH3 fingerprints (computed by xxh3.exe).
# Usage: shaders_especiales.py <PAL containers> <other containers> <xxh3 PAL> <xxh3 other>
import os, sys


def be(b, o):
    return int.from_bytes(b[o:o + 4], 'big')


def huellas(fichero):
    out = {}
    for linea in open(fichero, encoding='utf-8'):
        h, ruta = linea.split(None, 1)
        out[os.path.basename(ruta.strip())] = h
    return out


pal_dir, otra_dir, hx_pal, hx_otra = sys.argv[1:5]
h_pal, h_otra = huellas(hx_pal), huellas(hx_otra)
otra = {n: open(os.path.join(otra_dir, n), 'rb').read() for n in os.listdir(otra_dir) if n.endswith('.bin')}
for nombre, rol in [('p_000094.bin', 'resplandor'), ('p_000117.bin', 'cielo'), ('p_000139.bin', 'composicion')]:
    b = open(os.path.join(pal_dir, nombre), 'rb').read()
    v = be(b, 4)
    identicos = sorted(n for n, u in otra.items() if u == b)
    mismo = sorted(n for n, u in otra.items() if be(u, 4) == v and u[v:] == b[v:])
    print('%-12s %s PAL %s | identicos %s | mismo microcodigo %s' % (
        rol, nombre, h_pal[nombre], identicos, ', '.join('%s=%s' % (n, h_otra[n]) for n in mismo)))
