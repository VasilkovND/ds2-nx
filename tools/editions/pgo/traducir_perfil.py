# -*- coding: utf-8 -*-
# Translates the PGO profile (the .gcda files) to another edition of the game.
#
# GCC 16 identifies each function in the profile by a hash of its name (crc32 of the assembler name) and
# checks a line checksum (line, file path and name) and another of the control flow graph. Recompiled
# functions are named after their address (__imp__sub_823B5A40), as are our hooks (sub_824455B8) and five
# C++ functions with the address in the name. In another edition they change address and name: without
# translating the profile, PGO would find almost nothing.
#
# Here every record of those functions is moved to the other edition's name: new hash and new line
# checksum. The control flow graph is not touched (if the function really changed, GCC detects it and
# drops its profile with a warning). Records of functions that no longer exist are removed. Everything
# else (headers, local functions) is copied as is: the other edition's build is compiled with the same
# paths as the Spanish one, so their hashes do not change.
#
# Usage: traducir_perfil.py <PAL profile> <new profile> <table.tsv> <parejas.json> <PAL generated>
#                          <new generated> <app names with an address (gcc-nm)> <root of the PAL sources>
import glob
import json
import os
import re
import shutil
import struct
import sys

import gcda

# app/ of this repository, spelled as CMake passes it to GCC (forward slashes). The profile of local functions
# only matches when the sources are compiled from this same path (see docs/toolchain.md).
RAIZ_APP = os.environ.get('NFSMW_APP_DIR') or os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', '..', 'app')).replace(os.sep, '/')
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
DIR = re.compile(r'82[0-9A-Fa-f]{6}')


def lineas_de_funciones(ruta):
    texto = open(ruta, encoding='utf-8').read()
    return {m.group(1): texto.count('\n', 0, m.start()) + 1 for m in DEF.finditer(texto)}


def lineno(linea, fichero, nombre):
    return gcda.crc32_gcc(gcda.crc32_gcc(linea, fichero), nombre)


def main():
    origen, destino, tabla, parejas, gen_pal, gen_otra, nombres_app, fuentes = sys.argv[1:9]
    mapa = {}
    for l in open(tabla, encoding='utf-8').read().splitlines()[1:]:
        p, s, o, e = l.split('\t')
        if o != '-':
            mapa[p.upper()] = o.upper()
    mapa.update({p.upper(): o.upper() for p, o in json.load(open(parejas, encoding='utf-8')).get('parejas', {}).items()})

    if os.path.exists(destino):
        shutil.rmtree(destino)
    os.makedirs(destino)
    cuentas = {'traducidas': 0, 'quitadas': 0, 'ganchos': 0, 'sin_linea': 0, 'ficheros': 0}

    # app names with an address inside: the source tool changes the address unless it is glued to another
    # hexadecimal digit (Suma825FDFB0 stays the same)
    def traducir_nombre(nombre):
        def cambiar(m):
            antes = nombre[m.start() - 1] if m.start() else ''
            if antes and antes in '0123456789abcdefABCDEF':
                return m.group()
            nuevo = mapa.get(m.group().upper())
            if nuevo is None:
                return m.group()
            return nuevo.lower() if any(c in 'abcdef' for c in m.group()) else nuevo
        return DIR.sub(cambiar, nombre)

    con_direccion = [n.strip() for n in open(nombres_app, encoding='utf-8') if n.strip()]
    por_hash_app = {gcda.ident_publica(n): n for n in con_direccion if traducir_nombre(n) != n}

    for ruta in sorted(glob.glob(os.path.join(origen, '*.gcda'))):
        base = os.path.basename(ruta)
        cab, regs = gcda.leer(ruta)
        m = re.search(r'#generated#default#(nfsmw_recomp\.\d+\.cpp)\.gcda$', base)
        quitar = set()
        if m:
            fichero = RAIZ_APP + '/generated/default/' + m.group(1)
            pal = lineas_de_funciones(os.path.join(gen_pal, m.group(1)))
            otra = lineas_de_funciones(os.path.join(gen_otra, m.group(1)))
            por_hash = {gcda.ident_publica('__imp__sub_' + a): a for a in pal}
            for i, ident, lin, cfg, contadores in gcda.funciones(regs):
                a = por_hash.get(ident)
                if a is None:
                    continue
                b = mapa.get(a)
                if b is None or b not in otra:
                    quitar.add(i)
                    quitar.update(contadores)
                    cuentas['quitadas'] += 1
                    continue
                nombre = '__imp__sub_' + b
                regs[i][2] = struct.pack('<3I', gcda.ident_publica(nombre), lineno(otra[b], fichero, nombre), cfg)
                cuentas['traducidas'] += 1
        else:
            # app source: the file is deduced from the .gcda name (CMakeFiles#nfsmw.dir#src#x.cpp.gcda)
            fuente = None
            n = re.search(r'#nfsmw\.dir#(src#.+)\.gcda$', base)
            if n:
                fuente = RAIZ_APP + '/' + n.group(1).replace('#', '/')
            for i, ident, lin, cfg, contadores in gcda.funciones(regs):
                viejo = por_hash_app.get(ident)
                if viejo is None:
                    continue
                nuevo = traducir_nombre(viejo)
                linea = None
                if fuente:
                    linea = next((k for k in range(1, 40000) if lineno(k, fuente, viejo) == lin), None)
                if linea is None:
                    cuentas['sin_linea'] += 1
                    nueva_lin = lin
                else:
                    nueva_lin = lineno(linea, fuente, nuevo)
                regs[i][2] = struct.pack('<3I', gcda.ident_publica(nuevo), nueva_lin, cfg)
                cuentas['ganchos'] += 1
        # two records with the same hash in one file: GCC treats it as a corrupt profile and stops.
        # The first one is kept.
        vistos = set()
        for i, ident, lin, cfg, contadores in gcda.funciones(regs):
            if i in quitar:
                continue
            nuevo_ident = struct.unpack('<3I', regs[i][2])[0]
            if nuevo_ident in vistos:
                quitar.add(i)
                quitar.update(contadores)
                cuentas['repetidas'] = cuentas.get('repetidas', 0) + 1
            vistos.add(nuevo_ident)
        regs = [r for k, r in enumerate(regs) if k not in quitar]
        gcda.escribir(os.path.join(destino, base), cab, regs)
        cuentas['ficheros'] += 1
    print(cuentas)


if __name__ == '__main__':
    main()
