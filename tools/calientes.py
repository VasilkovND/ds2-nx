"""Lists the functions with the most profiler samples, hottest first (the input for orden_funciones.ld).

Usage: python tools/calientes.py <rex_perfil.log> <nfsmw.elf> <output.txt>

rex_perfil.log is written by the sampling profiler of the app: every sample carries the addresses it saw (pc, lr
and stack) as imagen+0x.... Each address is resolved with addr2line against the unstripped ELF of the same build and
weighted by the share of samples it appeared in.
"""
import collections
import os
import re
import subprocess
import sys

if len(sys.argv) != 4:
    raise SystemExit(__doc__)
LOG, ELF, SALIDA = sys.argv[1:4]
A2L = os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'), 'devkitA64', 'bin', 'aarch64-none-elf-addr2line')
peso = collections.Counter()
ns = 0
for l in open(LOG, encoding='utf-8', errors='ignore'):
    m = re.match(r'^\s+(\d+) muestras', l)
    if m:
        ns = int(m.group(1))
        continue
    m = re.match(r'^\s+([\d.]+)%\s+(pc|lr|pila) (.*)', l)
    if not m:
        continue
    w = float(m.group(1)) * ns / 100
    for a in set(re.findall(r'imagen\+0x([0-9a-f]+)', m.group(3))):
        peso[int(a, 16)] += w
addrs = sorted(peso)
out = subprocess.run([A2L, '-f', '-e', ELF] + [hex(a) for a in addrs], capture_output=True, text=True).stdout.split('\n')
fn = collections.Counter()
for i, a in enumerate(addrs):
    fn[out[2 * i]] += peso[a]
fn.pop('??', None)
lista = [f for f, _ in fn.most_common()]
open(SALIDA, 'w').write('\n'.join(lista))
print(len(lista), 'hot functions;', sum(1 for f in lista if 'sub_8' in f), 'from the game')
print(lista[:15])
