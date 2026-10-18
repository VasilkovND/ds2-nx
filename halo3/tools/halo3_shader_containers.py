#!/usr/bin/env python3
"""Finds Halo 3's shader containers in its map files, for the shader library.

Halo 3 keeps its compiled shaders inside the maps' tag data as Xbox 360 shader containers in the 2008 format
(signature 10 2A 11 00 for pixel shaders, 10 2A 11 01 for vertex shaders), the format XenosRecomp reads as it is,
but without a constant table (its offset is 0): the names of the constants are in Bungie's own tags.
This is the first step of the shader library (see shaders/README.md and halo3/PORTING.md): the same as NFS's
nfsmw_buscar_contenedores, for the newer format, and reading the maps either from a game folder or straight out of
the disc image.

A signature with header sizes that make sense is a candidate, not a validated shader: the translator checks each
one again. The same container appears in many maps, so each distinct container is written once, named in the order
first found (p_000000.bin, v_000001.bin, one counter for both kinds) with its first source in procedencia.tsv. The
scan order (the maps sorted by name) must not change, or the names change.

Usage:
  halo3_shader_containers.py <game folder or .iso> <new output folder> [map name ...]

With map names (for example mainmenu), only those maps are scanned.
"""

import hashlib
import os
import struct
import sys

CHUNK = 64 << 20
MAX_CONTAINER = 1 << 20


def disc_maps(iso):
    """Yields (name, open file, offset, length) for every .map in an Xbox 360 (GDFX) disc image."""
    f = open(iso, 'rb')
    base = None
    for off in (0, 0xFB20, 0x20600, 0x2080000, 0xFD90000):  # byte offsets, as DiscImageDevice
        f.seek(off + 32 * 2048)
        if f.read(20) == b'MICROSOFT*XBOX*MEDIA':
            base = off
            break
    if base is None:
        sys.exit(f'{iso}: not an Xbox 360 disc image')
    root_sector, root_size = struct.unpack('<II', f.read(8))
    found = []

    def walk(sector, size, path):
        f.seek(base + sector * 2048)
        data = f.read(size)

        def node(o):
            if o * 4 + 14 > len(data):
                return
            left, right, sec, length, attr, name_len = struct.unpack('<HHIIBB', data[o * 4:o * 4 + 14])
            if left == 0xFFFF and right == 0xFFFF:
                return
            name = data[o * 4 + 14:o * 4 + 14 + name_len].decode('latin1')
            if left:
                node(left)
            p = path + '/' + name
            if attr & 0x10:
                if length:
                    walk(sec, length, p)
            elif p.lower().endswith('.map'):
                found.append((p, base + sec * 2048, length))
            if right:
                node(right)

        node(0)

    walk(root_sector, root_size, '')
    for p, off, length in sorted(found):
        yield p, f, off, length


def folder_maps(folder):
    maps = os.path.join(folder, 'maps')
    for name in sorted(os.listdir(maps)):
        if name.lower().endswith('.map'):
            path = os.path.join(maps, name)
            yield '/maps/' + name, open(path, 'rb'), 0, os.path.getsize(path)


def candidates(data, start):
    """Containers starting in data[start:]: (offset, kind, length)."""
    i = start
    while True:
        i = data.find(b'\x10\x2a\x11', i)
        if i < 0 or i + 36 > len(data):
            return
        kind = data[i + 3]
        if kind <= 1:
            virtual, physical, _, ctab, defs, shader = struct.unpack('>IIIIII', data[i + 4:i + 28])
            if (36 <= virtual and 0 < physical and virtual + physical <= MAX_CONTAINER and
                    (ctab == 0 or 36 <= ctab < virtual) and 36 <= shader < virtual and defs < virtual):
                yield i, kind, virtual + physical
        i += 1


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    source, out, only = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    if os.path.exists(out):
        sys.exit(f'{out} already exists')
    os.makedirs(out)
    maps = disc_maps(source) if os.path.isfile(source) else folder_maps(source)
    seen = {}
    counter = 0
    with open(os.path.join(out, 'procedencia.tsv'), 'w') as index:
        index.write('archivo\torigen\tdesplazamiento\tlongitud\n')
        for name, f, offset, length in maps:
            if only and os.path.splitext(os.path.basename(name))[0] not in only:
                continue
            new = total = 0
            done = 0
            carry = b''
            while done < length:
                f.seek(offset + done)
                block = f.read(min(CHUNK, length - done))
                data = carry + block
                base = done - len(carry)
                limit = len(data) if done + len(block) >= length else len(data) - MAX_CONTAINER
                last = 0
                for i, kind, size in candidates(data, 0):
                    if i >= limit:
                        break
                    last = i + 1
                    if i + size > len(data):
                        continue  # runs past the end of the map
                    blob = data[i:i + size]
                    total += 1
                    key = hashlib.sha1(blob).digest()
                    if key in seen:
                        continue
                    file = f"{'v' if kind else 'p'}_{counter:06d}.bin"
                    counter += 1
                    seen[key] = file
                    with open(os.path.join(out, file), 'wb') as w:
                        w.write(blob)
                    index.write(f'{file}\t{name}\t{base + i}\t{size}\n')
                    new += 1
                done += len(block)
                carry = data[max(limit, last):] if done < length else b''
            print(f'{name}: {total} containers, {new} new', flush=True)
    print(f'{counter} distinct containers in {out}')


if __name__ == '__main__':
    main()
