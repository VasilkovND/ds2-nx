#!/usr/bin/env python3
# From fc3-nx (barbaruiva25/fc3-nx, GPL-3.0), adapted for xbox360-nx's game folders.
"""Finds code gaps with no function assigned in the codegen output.

Adapted from the gap scanner of the reference ReXGlue Switch port by StevenSND (GPL-3.0).

The problem
-----------
The game dies with:
    [FATAL] Call to invalid or unregistered function at guest address 0xXXXXXXXX

That happens when something makes an indirect call (through a pointer or vtable) to an address the analysis did
not mark as a function start. The Validate phase does not detect them: it only checks direct branches (b / bl).
Indirect ones only show up at run time, one at a time.

How it finds them
-----------------
Every PowerPC instruction takes 4 bytes and the codegen emits exactly one comment line "\\t// <asm>" per
instruction. So:

    function_end = start + 4 * number_of_comments

With the start of each function (codegen.partition.json) and its computed end, every stretch between the end of
one and the start of the next is a gap with no owner. Gaps that contain real code are the candidates for
breaking the game at run time.

Usage:
    python3 tools/find_gaps.py
    python3 tools/find_gaps.py --min 8
    python3 tools/find_gaps.py --check 0x8215FEA8 0x826BE258

Output: a text report (default out/gaps/report.txt) and a TOML block of candidate declarations (default
out/gaps/candidates.toml) in the format of app/overrides.toml. Do not include the TOML as it is.
"""


# ============================================================================
#  Known limitation - a gap is not always one function
#
#  This tool emits one declaration per gap, at its start address. That assumes each gap contains exactly one
#  function, and it is false.
#
#  A gap is simply space that automatic discovery did not claim. It can contain several small functions in a row,
#  typically tables of 8- and 16-byte thunks. By declaring only the start, the Discover phase swallows the whole
#  gap as one single function and the other entry points become unreachable. The symptom is:
#
#     [FATAL] Call to invalid or unregistered function at guest address 0x...
#
#  with an address that falls inside a gap that is already declared. The crash seems to "move": each fix
#  uncovers the next entry of the same gap.
#
#  Splitting every gap every 8 bytes is not the solution: that would be thousands of declarations, and most gaps
#  do contain a single function, so entry points would be declared in the middle of functions.
#
#  What does work: split only the gaps proven to have several entries (where a crash already happened inside),
#  see tools/runtime_gap_resolver.py, or run the structural scan of tools/gap_fixpoint.py.
# ============================================================================

import argparse
import bisect
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')


def measure_functions(gen_dir):
    """Returns {start: instruction_count} by walking the generated C++."""
    sizes = {}
    files = sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp'))
    for idx, name in enumerate(files, 1):
        path = os.path.join(gen_dir, name)
        current = None
        count = 0
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(line)
                    if m:
                        current = int(m.group(1), 16)
                        count = 0
                elif current is not None:
                    if line.startswith('\t// '):
                        count += 1
                    elif line.startswith('}'):
                        sizes[current] = count
                        current = None
        sys.stdout.write("\r  read %d/%d files" % (idx, len(files)))
        sys.stdout.flush()
    print()
    return sizes


def main():
    p = argparse.ArgumentParser(description="Finds code gaps with no function assigned.")
    p.add_argument("--gen", default=os.path.join(ROOT, "app", "generated", "default"),
                   help="folder with the generated code (default: app/generated/default)")
    p.add_argument("--min", type=int, default=4, help="smallest gap size to list (default 4)")
    p.add_argument("--check", nargs="*", default=[],
                   help="specific addresses to locate, e.g. 0x8215FEA8")
    p.add_argument("--report", default=os.path.join(ROOT, "out", "gaps", "report.txt"))
    p.add_argument("--toml", default=os.path.join(ROOT, "out", "gaps", "candidates.toml"))
    args = p.parse_args()

    partition = os.path.join(args.gen, "codegen.partition.json")
    if not os.path.exists(partition):
        raise SystemExit("%s does not exist. Run tools/codegen.sh first." % partition)

    print("Reading the function partition...")
    with open(partition) as fh:
        assignments = json.load(fh)["assignments"]
    starts = sorted(int(k, 16) for k in assignments)
    print("  %d functions" % len(starts))

    print("Measuring every function in the generated C++...")
    sizes = measure_functions(args.gen)
    print("  %d measured" % len(sizes))

    missing = [a for a in starts if a not in sizes]
    if missing:
        print("  warning: %d functions could not be measured (ignored)" % len(missing))

    gaps = []
    for i, a in enumerate(starts[:-1]):
        n = sizes.get(a)
        if n is None:
            continue
        end = a + 4 * n
        following = starts[i + 1]
        if end < following:
            gaps.append((end, following - end, a, following))

    big = [g for g in gaps if g[1] >= args.min]

    distribution = {}
    for _, size, _, _ in gaps:
        distribution[size] = distribution.get(size, 0) + 1

    lines = []

    def w(s=""):
        print(s)
        lines.append(s)

    w()
    w("=" * 60)
    w("  CODE GAPS WITH NO FUNCTION ASSIGNED")
    w("=" * 60)
    w("Functions            : %d" % len(starts))
    w("Gaps in total        : %d" % len(gaps))
    w("Gaps >= %d bytes      : %d" % (args.min, len(big)))
    w("Bytes in gaps        : %d" % sum(g[1] for g in gaps))
    w()
    w("Distribution by gap size:")
    for size in sorted(distribution):
        w("   %5d bytes  x %d" % (size, distribution[size]))
    w()

    if args.check:
        w("-" * 60)
        w("Addresses queried:")
        for s in args.check:
            t = int(s, 16)
            pos = bisect.bisect_left(starts, t)
            is_start = pos < len(starts) and starts[pos] == t
            inside = None
            for end, size, prev_start, following in gaps:
                if end <= t < end + size:
                    inside = (end, size, prev_start, following)
                    break
            w("  0x%08X  function start: %s" % (t, "YES" if is_start else "NO"))
            if inside:
                w("      falls in a gap of %d bytes: 0x%08X - 0x%08X"
                  % (inside[1], inside[0], inside[0] + inside[1]))
                w("      (after function 0x%08X, before 0x%08X)" % (inside[2], inside[3]))
            elif not is_start:
                w("      does not fall in any known gap")
        w()

    w("-" * 60)
    w("First 60 gaps of >= %d bytes:" % args.min)
    for end, size, prev_start, following in big[:60]:
        w("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)" % (end, size, prev_start, following))
    if len(big) > 60:
        w("  ... and %d more" % (len(big) - 60))

    os.makedirs(os.path.dirname(args.report) or ".", exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
        fh.write("\n" + "-" * 60 + "\nALL gaps >= %d bytes:\n" % args.min)
        for end, size, prev_start, following in big:
            fh.write("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)\n" % (end, size, prev_start, following))
    print()
    print("Full report in %s" % args.report)

    os.makedirs(os.path.dirname(args.toml) or ".", exist_ok=True)
    with open(args.toml, "w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/find_gaps.py - DO NOT include as it is.\n")
        fh.write("# Declaring a function in a gap that is padding or data can break the codegen.\n")
        fh.write("# Copy only the entries that are needed.\n")
        fh.write("[functions]\n")
        for end, size, prev_start, following in big:
            fh.write('"0x%08X" = { }   # %d bytes, after 0x%08X\n' % (end, size, prev_start))
    print("Reference TOML block in %s" % args.toml)


if __name__ == "__main__":
    main()
