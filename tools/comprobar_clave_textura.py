# -*- coding: utf-8 -*-
# Check of the already built ELF, without running anything. It looks in the machine code for the texture
# key of PrepararTextura (claves[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9} and
# its XXH3) and checks whether any read of those 20 stack bytes comes before the store of what it reads.
# That is the bug that broke texture keys:
#     ldr x3, [sp+548]      <- reads claves[3] and claves[4]
#     str w2, [sp, #552]    <- writes claves[4] afterwards
# Usage:  py comprobar_clave_textura.py <elf> [<elf> ...]     (the .stripped.elf works; no symbols needed)
# Output: one line per site found and a verdict per ELF: BIEN, MAL or NO ENCONTRADO. Exit code 1 if any
# ELF is MAL or NO ENCONTRADO.
#
# How it finds the site: the only instruction "and wD, wN, #0xffc003ff" followed by "str wD, [sp, #K]"
# and by a 20-byte XXH3, either inlined (constant 20 * PRIME64_1 = 0x5c5581de766bd28c) or called out
# of line with x1 = 20.
# Limits: it follows program order in a straight window (it does not follow branches); that is why it
# also lists the instructions it used, so they can be checked by hand.

import os
import re
import struct
import subprocess
import sys

OBJDUMP = os.environ.get('OBJDUMP') or os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'), 'devkitA64',
                                                     'bin', 'aarch64-none-elf-objdump')
if not os.path.exists(OBJDUMP):
    OBJDUMP = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump'

ANTES = 64      # instructions before the AND that are examined (some key words are written earlier)
DESPUES = 200   # instructions after the AND


def seccion_texto(ruta):
    with open(ruta, 'rb') as f:
        datos = f.read()
    assert datos[:4] == b'\x7fELF' and datos[4] == 2 and datos[5] == 1, ruta + ': no es un ELF64 little-endian'
    e_shoff, = struct.unpack_from('<Q', datos, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', datos, 0x3A)
    def cabecera(i):
        return struct.unpack_from('<IIQQQQIIQQ', datos, e_shoff + i * e_shentsize)
    nombres = cabecera(e_shstrndx)
    for i in range(e_shnum):
        c = cabecera(i)
        ini = nombres[4] + c[0]
        nombre = datos[ini:datos.index(b'\x00', ini)].decode()
        if nombre == '.text':
            return datos, c[3], c[4], c[5]  # address, file offset, size
    raise SystemExit(ruta + ': sin seccion .text')


def ands_de_la_mascara(datos, direccion, desplazamiento, tamano):
    # and wD, wN, #0xffc003ff = 0x120a4c00 | (N << 5) | D: bytes LE xx [4c-4f] 0a 12
    trozo = datos[desplazamiento:desplazamiento + tamano]
    for m in re.finditer(rb'[\x00-\xff][\x4c-\x4f]\x0a\x12', trozo, re.S):
        if m.start() % 4 == 0:
            palabra, = struct.unpack_from('<I', trozo, m.start())
            if palabra & 0xFFFFFC00 == 0x120A4C00:
                yield direccion + m.start()


LINEA = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')


def desensamblar(ruta, desde, hasta):
    salida = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--start-address=' + hex(desde),
                             '--stop-address=' + hex(hasta), ruta], capture_output=True, text=True).stdout
    instr = []
    for l in salida.splitlines():
        m = LINEA.match(l)
        if m:
            ops = m.group(3).split('//')[0].strip()
            instr.append((int(m.group(1), 16), m.group(2), ops))
    return instr


TAM = {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}
MEM = re.compile(r'\[(sp|x\d+)(?:,\s*#(-?(?:0x[0-9a-f]+|\d+)))?\](!?)')


def acceso(mnem, ops, bases):
    """(kind, start relative to sp, bytes) for loads and stores based on sp (or on a register = sp + imm)."""
    es_carga = mnem.startswith('ld')
    es_alm = mnem.startswith('st')
    if not (es_carga or es_alm) or mnem.startswith(('ldax', 'stlx', 'ldxr', 'stxr', 'ldar', 'stlr')):
        return None
    m = MEM.search(ops)
    if not m:
        return None
    base = m.group(1)
    imm = int(m.group(2), 0) if m.group(2) else 0
    if ', #' in ops[m.end():]:
        imm = 0  # post-indexed: accesses at the base
    if base == 'sp':
        inicio = imm
    elif base in bases:
        inicio = bases[base] + imm
    else:
        return None
    regs = [r.strip() for r in ops[:m.start()].rstrip(', ').split(',')]
    par = mnem in ('ldp', 'stp', 'ldnp', 'stnp')
    if mnem in ('ldrb', 'strb', 'ldrsb', 'ldurb', 'sturb'):
        tam = 1
    elif mnem in ('ldrh', 'strh', 'ldrsh', 'ldurh', 'sturh'):
        tam = 2
    elif mnem == 'ldrsw':
        tam = 4
    else:
        tam = TAM.get(regs[0][0], 8) if regs and regs[0] else 8
        if regs[0] in ('xzr',):
            tam = 8
        if regs[0] in ('wzr',):
            tam = 4
    if par:
        tam *= 2
    return ('carga' if es_carga else 'escritura', inicio, tam)


def destino(ops):
    return ops.split(',')[0].strip() if ops else ''


def analizar(ruta, direccion_and):
    instr = desensamblar(ruta, direccion_and - 4 * ANTES, direccion_and + 4 * DESPUES)
    i_and = next((i for i, x in enumerate(instr) if x[0] == direccion_and), None)
    if i_and is None:
        return None
    rd = destino(instr[i_and][2])
    # K: the first store of wD to the stack after the AND (claves[0])
    k = None
    for a, mnem, ops in instr[i_and + 1:]:
        if mnem == 'str' and ops.startswith(rd + ',') and '[sp' in ops:
            m = MEM.search(ops)
            k = int(m.group(2), 0) if m.group(2) else 0
            break
        if destino(ops) in (rd, 'x' + rd[1:]) and not mnem.startswith('st'):
            break
    if k is None:
        return None
    zona = range(k, k + 20)
    bases = {}
    primera_escritura = {}
    malas = []
    usadas = []
    firma = None
    llamada_tras_escrituras = None
    for idx, (a, mnem, ops) in enumerate(instr):
        if mnem == 'mov' and re.match(r'x\d+, #0xd28c$', ops):
            firma = 'XXH3 integrado (0x...d28c = 20 * PRIME64_1)'
        if mnem == 'mov' and ops == 'x1, #0x14' and idx > i_and:
            firma = firma or 'XXH3 fuera de linea (x1 = 20)'
        acc = acceso(mnem, ops, bases)
        if acc:
            tipo, ini, tam = acc
            toca = [b for b in range(ini, ini + tam) if b in zona]
            if toca:
                usadas.append((a, mnem, ops, tipo))
                if tipo == 'escritura':
                    for b in toca:
                        primera_escritura.setdefault(b, idx)
                elif idx > i_and and llamada_tras_escrituras is None:
                    faltan = [b for b in toca if primera_escritura.get(b, 10 ** 9) > idx]
                    if faltan:
                        malas.append((a, mnem, ops, min(faltan), max(faltan)))
        # registers that point into the stack: add xT, sp, #imm
        m = re.match(r'(x\d+), sp, #(0x[0-9a-f]+|\d+)$', ops) if mnem == 'add' else None
        d = destino(ops)
        if m:
            bases[m.group(1)] = int(m.group(2), 0)
        elif d in bases and not mnem.startswith(('st', 'cmp', 'tst', 'cb', 'tb', 'b.')):
            bases.pop(d, None)
        if idx > i_and and mnem in ('bl', 'blr') and all(b in primera_escritura for b in zona):
            if llamada_tras_escrituras is None:
                llamada_tras_escrituras = a
        if idx > i_and and mnem in ('ret', 'b', 'br'):
            break
    if firma is None:
        return None
    return {'and': direccion_and, 'k': k, 'firma': firma, 'malas': malas, 'usadas': usadas,
            'escritas': sorted(set(b - k for b in primera_escritura)), 'llamada': llamada_tras_escrituras}


def main():
    if len(sys.argv) < 2:
        print('uso: py comprobar_clave_textura.py <elf> [<elf> ...]')
        return 2
    fallo = False
    for ruta in sys.argv[1:]:
        datos, direccion, desplazamiento, tamano = seccion_texto(ruta)
        sitios = []
        for a in ands_de_la_mascara(datos, direccion, desplazamiento, tamano):
            r = analizar(ruta, a)
            if r:
                sitios.append(r)
        print('== ' + ruta)
        if not sitios:
            print('   NO ENCONTRADO: ningun "and wD, wN, #0xffc003ff" con la clave en la pila y un XXH3 de 20 bytes')
            fallo = True
            continue
        for s in sitios:
            print('   sitio: AND en {:#x}; claves en [sp+{}, sp+{}); {}'.format(s['and'], s['k'], s['k'] + 20, s['firma']))
            for a, mnem, ops, tipo in s['usadas']:
                print('      {:#x}  {:8} {:30} {}'.format(a, mnem, ops, tipo))
            if s['malas']:
                fallo = True
                for a, mnem, ops, b0, b1 in s['malas']:
                    print('   MAL: {:#x} {} {} lee sp+{}..sp+{} ANTES de escribirlo (claves[{}])'.format(
                        a, mnem, ops, b0, b1, (b0 - s['k']) // 4))
            elif len(s['escritas']) != 20:
                fallo = True
                print('   MAL: solo se ven escritas {} de los 20 bytes de la clave en la ventana'.format(len(s['escritas'])))
            else:
                extra = ' (XXH3 llamado en {:#x}, despues de las 5 escrituras)'.format(s['llamada']) if s['llamada'] else ''
                print('   BIEN: toda lectura de la clave va despues de su escritura' + extra)
    return 1 if fallo else 0


if __name__ == '__main__':
    sys.exit(main())
