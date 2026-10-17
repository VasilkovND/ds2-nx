#!/usr/bin/env python3
# From fc3-nx (barbaruiva25/fc3-nx, GPL-3.0), adapted for xbox360-nx's game folders.
"""Classify gap candidates (tools/find_gaps.py) from a scratch ReXGlue codegen listing.

Accept only gaps whose generated function is a complete, self-contained leaf or
thunk: every instruction fits inside the gap, it ends in blr/bctr or a tail
branch, and it contains no padding/invalid words. Everything else is rejected.
"""
import glob, re, sys
from collections import Counter

def tier1_main():
    gen_dir, selected, codegen_log = sys.argv[1], sys.argv[2], sys.argv[3]
    stubs = set(re.findall(r"Function (0x[0-9A-F]+) has no blocks", open(codegen_log).read()))
    gaps = {a: int(s) for a, s in (l.split() for l in open(selected))}
    listings = {}
    for path in glob.glob(f"{gen_dir}/*recomp*.cpp"):
        cur = None
        for line in open(path, errors="replace"):
            m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)", line)
            if m:
                a = "0x" + m.group(1)
                cur = a if a in gaps else None
                if cur: listings[cur] = []
                continue
            if cur is None: continue
            if line.startswith("}"): cur = None; continue
            m = re.match(r"\t// (\S.*)$", line)
            if m and not m.group(1).startswith(("PPC_", "0x")): listings[cur].append(m.group(1).strip())
    BAD_OPS = {"nop", "trap", "tw", "twi", "td", "tdi"}
    TERM = re.compile(r"^(blr|bctr|b\s+0x|b\s+sub_|beqlr|bnelr)")
    res = Counter(); accepted = []
    for a, size in gaps.items():
        ins = listings.get(a)
        if a in stubs or not ins: res["no code / stub"] += 1; continue
        if len(ins) * 4 > size: res["extends beyond gap"] += 1; continue
        if any(i.split()[0] in BAD_OPS or i.startswith((".long", "invalid", "unknown", "illegal")) for i in ins): res["padding/invalid"] += 1; continue
        if not TERM.match(ins[-1]): res["no terminator"] += 1; continue
        res["ACCEPT"] += 1; accepted.append((a, size, ins))
    for k, v in res.most_common(): print(f"{v:6d}  {k}")
    shape = Counter(" / ".join(i.split()[0] for i in ins) for _, _, ins in accepted)
    print("\nmost common accepted shapes:")
    for k, v in shape.most_common(15): print(f"{v:6d}  {k}")
    with open(sys.argv[4], "w") as f:
        for a, s, ins in accepted: f.write(f"{a} {s} | " + " ; ".join(ins) + "\n")



def reads_undefined_at_entry(instructions):
    """True if the code reads a register that is undefined at a function entry
    (r0, r11, r12, f0, a CR field) before writing it: a fragment, not a function."""
    scratch = {"r0", "r11", "r12", "f0"}
    defined = set()
    for ins in instructions:
        op, _, rest = ins.partition(" ")
        ops = [o.strip() for o in rest.split(",") if o.strip()]
        # r0 as the base of an address is the constant 0, not a read of r0: d(r0) in D-form loads and stores, the
        # rA operand of indexed ones (lvx128 v1,r0,r11; stwx r3,r0,r4) and of the cache ops (dcbt r0,r3). Reading
        # it as r0 rejected fc3's sub_82851BD0 / sub_82851BE0 (lvx128 v1,r0,r11) as fragments.
        ops = ["0" if re.fullmatch(r"-?\d+\(r0\)", o) else o for o in ops]
        if re.fullmatch(r"(l|st)\w*x(128)?|dcb\w*|icbi", op):
            k = 0 if op.startswith(("dcb", "icbi")) else 1
            if len(ops) > k and ops[k] == "r0":
                ops[k] = "0"
        regs = [re.sub(r"^-?\d+\((r\d+)\)$", r"\1", o) for o in ops]
        is_store = op.startswith("st")
        is_cmp = op.startswith("cmp")
        is_branch = op.startswith("b") and op not in ("bl",)
        dest = None
        if not (is_store or is_branch or op in ("mtctr", "mtlr")):
            dest = regs[0] if regs else None
            srcs = regs[1:]
        else:
            srcs = regs
        if is_cmp:
            dest, srcs = (regs[0] if regs and regs[0].startswith("cr") else "cr0"), [r for r in regs if not r.startswith("cr")]
        if op.endswith(".") or op in ("or.",):
            defined.add("cr0")
        if op in ("subfe", "adde", "addze", "subfze"):
            srcs = srcs + ["ca"]
        for r in srcs:
            if (r in scratch or r.startswith("cr") or r == "ca") and r not in defined:
                return True
        if is_branch and op not in ("b", "bctr", "blr") and not re.fullmatch(r"bdn?z(lr|ctr)?l?", op):
            # The CR field a conditional branch tests: "cr6" or the bit form "4*cr6+eq" (bdzf/bdnzt); bdz/bdnz
            # alone test only CTR.
            m = re.search(r"cr(\d)", rest)
            cr = f"cr{m.group(1)}" if m else "cr0"
            if cr not in defined:
                return True
        if dest:
            defined.add(dest)
        if op in ("addic", "addic.", "subfic"):
            defined.add("ca")
    return False


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] != "--tier2":
    tier1_main()

if __name__ == "__main__" and len(sys.argv) > 5 and sys.argv[1] != "--tier2":
    keep_in, keep_out = sys.argv[5], sys.argv[6]
    final, frag = [], []
    for l in open(keep_in):
        a, s, rest = l.split(" ", 2)
        ins = [i.strip() for i in rest.lstrip("| ").strip().split(" ; ")]
        (frag if reads_undefined_at_entry(ins) else final).append(l)
    open(keep_out, "w").writelines(final)
    print(f"\nfinal: {len(final)} accepted, {len(frag)} rejected as fragments")
    for l in frag: print("  frag", l.strip()[:120])


def tier2(gen_dir, selected, codegen_log, existing_overrides):
    """Candidates that extend past the gap only because they branch to the gap end,
    i.e. the next 'function' is really this function's tail (confirmed pattern)."""
    stubs = set(re.findall(r"Function (0x[0-9A-F]+) has no blocks", open(codegen_log).read()))
    gaps = {a: int(s) for a, s in (l.split() for l in open(selected))}
    have = set(re.findall(r'"(0x[0-9A-Fa-f]+)"\s*=', open(existing_overrides).read()))
    listings = {}
    for path in glob.glob(f"{gen_dir}/*recomp*.cpp"):
        cur = None
        for line in open(path, errors="replace"):
            m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)", line)
            if m:
                a = "0x" + m.group(1); cur = a if a in gaps else None
                if cur: listings[cur] = []
                continue
            if cur is None: continue
            if line.startswith("}"): cur = None; continue
            m = re.match(r"\t// (\S.*)$", line)
            if m and not m.group(1).startswith(("PPC_", "0x")): listings[cur].append(m.group(1).strip())
    out = []
    for a, size in gaps.items():
        ins = listings.get(a)
        if a in have or a in stubs or not ins or len(ins) * 4 <= size: continue
        end = int(a, 16) + size
        targets = {int(t, 16) for i in ins for t in re.findall(r"0x[0-9a-fA-F]+", i) if i.split()[0].startswith("b")}
        if end not in targets: continue
        ops = [i.split()[0] for i in ins]
        if any(re.search(r"\br1\b", i) for i in ins) or any(o in ("mtlr", "mflr", "ld", "std", "stwu", "lfd", "stfd", "nop") for o in ops): continue
        if len(ins) > 40 or reads_undefined_at_entry(ins): continue
        if not re.match(r"^(blr|bctr|b\s+0x|beqlr|bnelr)", ins[-1]): continue
        out.append((a, size, ins))
    return out


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--tier2":
    res = tier2(*sys.argv[2:6])
    with open(sys.argv[6], "w") as f:
        for a, s, ins in res: f.write(f"{a} {s} | " + " ; ".join(ins) + "\n")
    print(len(res), "tier-2 candidates")
    for a, s, ins in res[:40]: print(" ", a, s, "|", " ; ".join(ins)[:150])
