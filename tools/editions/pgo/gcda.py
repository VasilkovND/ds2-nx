# -*- coding: utf-8 -*-
# Reads and writes GCC 16 .gcda files (the PGO profile): header, function records and counters.
import struct

TAG_FUNCION = 0x01000000
TAG_RESUMEN = 0xA1000000


def leer(ruta):
    b = open(ruta, 'rb').read()
    magia, version, sello, suma = struct.unpack_from('<4I', b, 0)
    assert magia == 0x67636461, 'no es un .gcda'
    o = 16
    registros = []
    while o + 8 <= len(b):
        tag, largo = struct.unpack_from('<Ii', b, o)
        o += 8
        if tag == 0:
            break
        datos = b[o:o + max(largo, 0)]
        o += max(largo, 0)
        registros.append([tag, largo, datos])
    return (magia, version, sello, suma), registros


def escribir(ruta, cabecera, registros):
    partes = [struct.pack('<4I', *cabecera)]
    for tag, largo, datos in registros:
        partes.append(struct.pack('<Ii', tag, largo) + datos)
    partes.append(struct.pack('<I', 0))
    open(ruta, 'wb').write(b''.join(partes))


def funciones(registros):
    # [(function record index, ident, lineno, cfg, [counter records])]
    out = []
    for i, (tag, largo, datos) in enumerate(registros):
        if tag == TAG_FUNCION and largo == 12:
            ident, lineno, cfg = struct.unpack('<3I', datos)
            out.append([i, ident, lineno, cfg, []])
        elif out and tag not in (TAG_FUNCION, TAG_RESUMEN):
            out[-1][4].append(i)
    return out


def crc32_gcc(chksum, texto):
    # GCC's crc32_string: CRC-32 0x04C11DB7 without reflection, byte by byte, including the final 0
    for c in texto.encode() + b'\0':
        valor = c << 24
        for _ in range(8):
            realimentacion = 0x04C11DB7 if (valor ^ chksum) & 0x80000000 else 0
            chksum = ((chksum << 1) & 0xFFFFFFFF) ^ realimentacion
            valor = (valor << 1) & 0xFFFFFFFF
    return chksum


def ident_publica(nombre_ensamblador):
    # GCC's coverage_compute_profile_id for a visible symbol: crc32 of the name, 31 bits and never 0
    c = crc32_gcc(0, nombre_ensamblador) & 0x7FFFFFFF
    return c + (not c)
