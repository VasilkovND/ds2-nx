#!/usr/bin/env python3
# From fc3-nx (barbaruiva25/fc3-nx, GPL-3.0), adapted for xbox360-nx's game folders.
"""Declares the functions that are only reachable through code pointers stored in data (vtables, callback tables,
the CRT's initializer table), repeating until no new one qualifies.

Static discovery follows calls and branches; a virtual function that no code calls directly (only the vtable holds
its address) stays in an unclaimed gap, and the game stops at run time with "Call to invalid or unregistered function"
the first time it is called. Each pass:

  1. tools/find_gaps.py on the current generated code -> the unclaimed gaps;
  2. every 4-aligned word of the image outside the code range whose value lands in a gap is a candidate entry
     (same rule as tools/pointer_scan.py);
  3. a scratch codegen with the candidates declared as functions;
  4. gap_classify.py's checks on each candidate's scratch listing: the code fits in what is left of its gap, ends in
     blr/bctr or a tail branch, has no padding or invalid words, and does not read a register that is undefined at a
     function entry (r0, r11, r12, f0, a CR field: a fragment, not a function);
  5. the accepted ones go to app/overrides.toml with their evidence (where the pointer is stored), and the real
     codegen runs again, so the next pass sees the code they exposed.

  python3 tools/pointer_fixpoint.py --game halo3 [--image out/halo3_image.bin] [--scratch out/ptrscan] [--dry-run]

Needs rexglue (sdk/out/<platform>/rexglue[.exe] or $REXGLUE), the game where the game's manifest points, an existing
generated tree and an image dump (REXGLUE_DUMP_IMAGE=out/<game>_image.bin during codegen writes one).
"""
import argparse
import bisect
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The game folder (halo3, nfsmw_pal, ...), read before argparse because the paths below depend on it.
GAME = sys.argv[sys.argv.index('--game') + 1] if '--game' in sys.argv else 'halo3'
APP = os.path.join(ROOT, GAME)
TOOLS = os.path.join(ROOT, 'tools')
GEN = os.path.join(APP, 'generated', 'default')
MANIFEST = os.path.join(APP, GAME.split('_')[0] + '_manifest.toml')
OVERRIDES = os.path.join(APP, 'overrides.toml')
PROJECT = os.path.basename(MANIFEST)[:-len('_manifest.toml')]
IMAGE_BASE = 0x82000000
CODE_LO, CODE_HI = 0x82120000, 0x82970600  # fallback only; code_range() below reads
# the real range (fc3: code 82340000-83DF2AF4 per boot log) — the generated init carries
# REX_CODE_BASE/REX_CODE_SIZE macros instead of literals, so parse those first.

gc = {'__name__': 'gc'}
exec(compile(open(os.path.join(TOOLS, 'gap_classify.py')).read(), 'gc', 'exec'), gc)


def find_rexglue():
    if os.environ.get('REXGLUE'):
        return os.environ['REXGLUE']
    found = sorted(glob.glob(os.path.join(ROOT, 'sdk', 'out', '*', 'rexglue*')), key=os.path.getmtime)
    found = [f for f in found if os.path.basename(f) in ('rexglue', 'rexglue.exe')]
    if not found:
        sys.exit('rexglue not found: build it or set REXGLUE')
    return found[-1]


def code_range():
    # The SDK writes the code range into the generated PCH (defines, not literals).
    try:
        text = open(os.path.join(GEN, PROJECT + '_pch.h'), errors='replace').read()
        base = re.search(r'#define\s+REX_CODE_BASE\s+0x([0-9A-Fa-f]+)', text)
        size = re.search(r'#define\s+REX_CODE_SIZE\s+0x([0-9A-Fa-f]+)', text)
        if base and size:
            lo = int(base.group(1), 16)
            return lo, lo + int(size.group(1), 16)
    except OSError:
        pass
    # Legacy literal form, then the stale fallback (fc3 code is 82340000-83DF2AF4).
    text = open(os.path.join(GEN, PROJECT + '_init.cpp'), errors='replace').read()
    base = re.search(r'code_base\s*=\s*0x([0-9A-Fa-f]+)', text)
    size = re.search(r'code_size\s*=\s*0x([0-9A-Fa-f]+)', text)
    if base and size:
        return int(base.group(1), 16), int(base.group(1), 16) + int(size.group(1), 16)
    return 0x82340000, 0x83DF2B00  # fc3 default.xex, from the runtime log of the first boot


def read_gaps(scratch):
    report, toml = os.path.join(scratch, 'gaps.txt'), os.path.join(scratch, 'gaps.toml')
    subprocess.run([sys.executable, os.path.join(TOOLS, 'find_gaps.py'), '--gen', GEN, '--report', report,
                    '--toml', toml], capture_output=True, check=True)
    gaps = []
    for line in open(toml):
        m = re.match(r'"(0x[0-9A-F]+)" = \{ \}\s*#\s*(\d+) bytes', line)
        if m:
            gaps.append((int(m.group(1), 16), int(m.group(2))))
    return sorted(gaps)


def built_addresses(image, lo, hi):
    """Code addresses the code builds as constants: `lis rX,hi` followed (within 8 instructions, before rX is written
    again) by `addi rY,rX,lo` or `ori rY,rX,lo`. That is how a function pointer is passed as a callback or stored in
    a table at run time; no data word holds it, so the plain pointer scan misses it (fc3's sub_829427B8: built at
    0x82942810, the 2-minute recording stopped at its first call). Yields (value, address of the addi/ori)."""
    for off in range(lo - IMAGE_BASE, hi - IMAGE_BASE - 3, 4):
        x = struct.unpack_from('>I', image, off)[0]
        if x >> 26 != 15 or (x >> 16) & 31:  # lis rX,imm is addis rX,0,imm
            continue
        rx, upper = (x >> 21) & 31, (x & 0xFFFF) << 16
        for k in range(1, 9):
            y = struct.unpack_from('>I', image, off + 4 * k)[0]
            op, rt, ra, imm = y >> 26, (y >> 21) & 31, (y >> 16) & 31, y & 0xFFFF
            if op in (14, 24) and ra == rx:  # addi / ori from rX
                value = (upper + (imm - 0x10000 if op == 14 and imm & 0x8000 else imm)) & 0xFFFFFFFF
                yield value, IMAGE_BASE + off + 4 * k
                if rt == rx:
                    break
                continue
            if op == 18 or (op == 19 and (y >> 1) & 0x3FF in (16, 528)):  # b/bl/blr/bctr: end of the sequence
                break
            if rt == rx and op not in (36, 37, 38, 44, 45, 47, 54, 55):  # rX written (stores only read rt)
                break


def pointer_candidates(image, gaps, lo, hi):
    starts = [g[0] for g in gaps]
    found = {}

    def add(v, where):
        if v & 3 or not lo <= v < hi:
            return
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and gaps[i][0] <= v < gaps[i][0] + gaps[i][1]:
            found.setdefault(v, (gaps[i][0] + gaps[i][1] - v, []))[1].append(where)

    for off in range(0, len(image) - 3, 4):
        addr = IMAGE_BASE + off
        if not lo <= addr < hi:
            add(struct.unpack_from('>I', image, off)[0], addr)
    for value, where in built_addresses(image, lo, hi):
        add(value, where)
    return found  # target -> (bytes left in its gap, [where the pointer is stored or built])


def scratch_codegen(rexglue, d, candidates):
    ov = open(OVERRIDES).read()
    m = re.search(r'^\[functions\]\s*$', ov, re.M)
    body = ''.join(f'"0x{a:08X}" = {{ }}\n' for a in sorted(candidates))
    open(os.path.join(d, 'overrides.toml'), 'w').write(ov[:m.end()] + '\n' + body + ov[m.end():])
    man = open(MANIFEST).read()
    root = re.search(r'^game_root = "([^"]*)"', man, re.M).group(1)
    game = os.path.realpath(os.path.join(APP, root)).replace('\\', '/')
    # Only the entrypoint: the candidates are in default.xex, and the [[modules]] entries (the stage-data DLL) would
    # otherwise get the same file and output paths from the substitutions below.
    man = man.split('[[modules]]', 1)[0]
    man = re.sub(r'^game_root = .*$', f'game_root = "{game}"', man, flags=re.M)
    man = re.sub(r'^file_path = .*$', f'file_path = "{game}/default.xex"', man, flags=re.M)
    man = re.sub(r'^out_directory_path = .*$', 'out_directory_path = "gen"', man, flags=re.M)
    man = re.sub(r'^includes = .*$', 'includes = ["overrides.toml"]', man, flags=re.M)
    open(os.path.join(d, PROJECT + '_manifest.toml'), 'w').write(man)
    log = os.path.join(d, 'codegen.log')
    with open(log, 'w') as f:
        subprocess.run([rexglue, '--force', 'codegen', PROJECT + '_manifest.toml'], cwd=d, stdout=f,
                       stderr=subprocess.STDOUT)
    return os.path.join(d, 'gen'), log


def function_starts():
    starts = set()
    for path in glob.glob(os.path.join(GEN, '*recomp*.cpp')):
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', open(path, errors='replace').read(), re.M):
            starts.add(int(m.group(1), 16))
    return starts


def listings_with_addresses(gen, want=None):
    """{start: [(address, instruction text)]} for the functions of a generated tree (all, or those in `want`).
    Addresses come from the labels plus one per instruction comment (tools/find_gaps.py relies on the same)."""
    out = {}
    for path in glob.glob(os.path.join(gen, '*recomp*.cpp')):
        cur = addr = None
        for line in open(path, errors='replace'):
            m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)', line)
            if m:
                start = int(m.group(1), 16)
                cur = start if want is None or start in want else None
                if cur is not None:
                    addr = start
                    out[cur] = []
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                cur = None
                continue
            m = re.match(r'loc_([0-9A-F]+):', line)
            if m:
                addr = int(m.group(1), 16)
                continue
            m = re.match(r'\t// (\S.*)$', line)
            if m and not m.group(1).startswith(('PPC_', '0x')):
                out[cur].append((addr, m.group(1).strip()))
                addr += 4
    return out


_owner_cache = {}


def owner_map():
    """address -> start of the function of the current generated tree that holds it."""
    if not _owner_cache:
        for start, ins in listings_with_addresses(GEN).items():
            for a, _ in ins:
                _owner_cache.setdefault(a, start)
    return _owner_cache


def shares_registered_tail(a, left, listing, starts):
    """True when the part of the listing past the gap is only code of functions that already exist and that the
    part inside the gap branches to: the generator split the function's tail off as a function of its own (often
    the instruction after an early blr) and copies it back in as labels. fc3's vtable entry sub_825A5E18 (null
    check, return E_POINTER) continues at the registered sub_825A5E2C; sub_8256F120 ends in 8 bdz to registered
    one-line functions."""
    inside = [(x, t) for x, t in listing if a <= x < a + left]
    outside = [x for x, t in listing if not a <= x < a + left]
    if not inside or not outside:
        return False
    targets = set()
    for x, t in inside:
        if t.split()[0].startswith('b'):
            targets |= {int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', t)}
    owners = owner_map()
    return all(owners.get(x) in targets and owners.get(x) in starts and owners[x] >= a + left for x in outside)


def classify(gen, log, candidates):
    starts = function_starts() | set(candidates)
    stubs = set(re.findall(r'Function (0x[0-9A-F]+) has no blocks', open(log, errors='replace').read()))
    with_addr = listings_with_addresses(gen, set(candidates))
    bad_ops = {'nop', 'trap', 'tw', 'twi', 'td', 'tdi'}
    term = re.compile(r'^(blr|bctr|b\s+0x|b\s+sub_|beqlr|bnelr)')
    accepted, rejected, notes = {}, {}, {}
    for a, (left, refs) in candidates.items():
        key = f'0x{a:08X}'
        listing = with_addr.get(a, [])
        ins = [t for _, t in listing]
        why = None
        if key in stubs or not ins:
            why = 'no code'
        elif len(ins) * 4 > left and not shares_registered_tail(a, left, listing, starts):
            why = 'extends beyond its gap'
        elif any(i.split()[0] in bad_ops or i.startswith(('.long', 'invalid', 'unknown', 'illegal')) for i in ins):
            why = 'padding/invalid'
        elif not term.match(ins[-1]):
            why = 'no terminator'
        elif gc['reads_undefined_at_entry'](ins):
            why = 'fragment (reads an undefined register at entry)'
        elif (m := re.match(r'b\s+0x([0-9A-Fa-f]+)', ins[-1])) and int(m.group(1), 16) not in starts:
            # A real tail call lands on a function start. Landing inside a body (the __restgprlr_N epilogue
            # entries, for example) means an exception-unwind funclet or a detached chunk of another function.
            why = 'tail branch into a function body (funclet/chunk)'
        if why:
            rejected[a] = why
        else:
            accepted[a] = ins
            if len(ins) * 4 > left:
                notes[a] = f'{left // 4} in the gap, then the registered tail ' + ', '.join(
                    f'sub_{o:08X}' for o in sorted({owner_map()[x] for x, _ in listing if not a <= x < a + left}))
    return accepted, rejected, notes


def append_overrides(n, accepted, candidates, notes):
    lines = [f'\n# Code pointers into unclaimed gaps, stored in data (vtables, callback tables) or built in code (lis/addi:\n'
             f'# callbacks passed at run time), accepted by tools/pointer_fixpoint.py pass {n}: the scratch listing fits\n'
             f'# in the gap (or continues only into a registered function it branches to: "then the registered tail"),\n'
             f'# ends in a terminator and reads no register that is undefined at a function entry. The comment says\n'
             f'# where the pointer is stored ("pointer at") or built ("built at", inside the code range).\n']
    for a in sorted(accepted):
        refs = candidates[a][1]
        more = f' (+{len(refs) - 1} more)' if len(refs) > 1 else ''
        how = 'built at' if CODE_LO <= refs[0] < CODE_HI else 'pointer at'
        size = notes.get(a) or f'{len(accepted[a])} instructions'
        lines.append(f'"0x{a:08X}" = {{ }}  # {size}, {how} 0x{refs[0]:08X}{more}\n')
    insert_in_functions(lines)


def insert_in_functions(block):
    """Puts the lines at the end of the [functions] table of overrides.toml. Appending them at the end of the file
    is wrong as soon as another table follows [functions]: under a [[switch_tables]] header the keys become fields
    of the last switch table, and the generator ignores them without a word (2026-10-05)."""
    raw = open(OVERRIDES, newline='').read()  # keep the file's own line ends (Windows Python would write CRLF)
    eol = '\r\n' if raw.count('\r\n') * 2 > raw.count('\n') else '\n'
    text = raw.replace('\r\n', '\n').split('\n')
    start = next(i for i, l in enumerate(text) if l.strip() == '[functions]')
    end = next((i for i in range(start + 1, len(text)) if text[i].lstrip().startswith('[')), len(text))
    while end - 1 > start and (not text[end - 1].strip() or text[end - 1].lstrip().startswith('#')):
        end -= 1  # keep the comment that introduces the next table with it
    new = ''.join(block).rstrip('\n').split('\n')
    text[end:end] = new + ([''] if end < len(text) and text[end].strip() else [])
    with open(OVERRIDES, 'w', newline='') as f:
        f.write(eol.join(text))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=GAME, help='game folder (halo3, nfsmw_pal, ...)')
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', GAME + '_image.bin'))
    ap.add_argument('--scratch', default=os.path.join(ROOT, 'out', 'ptrscan'))
    ap.add_argument('--dry-run', action='store_true', help='one pass, report only, do not touch overrides.toml')
    a = ap.parse_args()

    global CODE_LO, CODE_HI
    rexglue = find_rexglue()
    image = open(a.image, 'rb').read()
    lo, hi = code_range()
    CODE_LO, CODE_HI = lo, hi
    print(f'code range {lo:08X}-{hi:08X}, rexglue {rexglue}')
    examined = set()
    for n in range(1, 20):
        d = os.path.join(a.scratch, f'pass{n}')
        os.makedirs(d, exist_ok=True)
        gaps = read_gaps(d)
        candidates = {t: v for t, v in pointer_candidates(image, gaps, lo, hi).items() if t not in examined}
        examined |= set(candidates)
        if not candidates:
            print(f'pass {n}: no new candidates; fixpoint')
            break
        gen, log = scratch_codegen(rexglue, d, candidates)
        accepted, rejected, notes = classify(gen, log, candidates)
        reasons = {}
        for why in rejected.values():
            reasons[why] = reasons.get(why, 0) + 1
        print(f'pass {n}: {len(gaps)} gaps, {len(candidates)} candidates, {len(accepted)} accepted, rejected {reasons}')
        with open(os.path.join(d, 'accepted.txt'), 'w') as f:
            for t in sorted(accepted):
                f.write(f'0x{t:08X} refs={",".join(f"0x{r:08X}" for r in candidates[t][1][:4])} | '
                        + ' ; '.join(accepted[t]) + '\n')
        with open(os.path.join(d, 'rejected.txt'), 'w') as f:
            for t in sorted(rejected):
                f.write(f'0x{t:08X} {rejected[t]} refs={",".join(f"0x{r:08X}" for r in candidates[t][1][:4])}\n')
        if a.dry_run or not accepted:
            break
        append_overrides(n, accepted, candidates, notes)
        r = subprocess.run(['bash', os.path.join(TOOLS, 'codegen.sh'), GAME], capture_output=True, text=True,
                           env={**os.environ, 'REXGLUE': rexglue, 'PYTHON': sys.executable})
        if r.returncode != 0:
            sys.exit(f'codegen failed after pass {n}:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')


if __name__ == '__main__':
    main()
