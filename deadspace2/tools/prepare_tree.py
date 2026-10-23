#!/usr/bin/env python3
"""Mirror Dead Space 2's recompiled tree (src_tu2/) for the Switch build.

The generated files include "ds2_rex_diagnostic_pch.h" with quotes, which always finds
the copy next to the includer first. So the Switch build compiles a mirror of the tree
made of symlinks, where only the PCH is replaced by the Switch one (make_nx_pch.py).
Nothing from the game is copied: the links point at your checkout of Dead-Space-2-Recomp.

With --local-regs on (the default) or check, the files that define functions are not links but the output of
patch_local_regs.py (guest registers in host locals, see that script): it re-runs only when one of its inputs changed
(the tree, the script, the hooks and names in the source dirs, the mode), and rewrites only the files whose text
changed, so Ninja rebuilds only those.

Usage: prepare_tree.py <Dead-Space-2-Recomp/src_tu2> <out_dir> [--local-regs on|off|check <source dir>...]
"""
import hashlib, os, re, subprocess, sys
from pathlib import Path

src = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
mode, sources = "off", []
if len(sys.argv) > 3:
    if sys.argv[3] != "--local-regs" or len(sys.argv) < 5 or sys.argv[4] not in ("on", "off", "check"):
        sys.exit(__doc__)
    mode, sources = sys.argv[4], sys.argv[5:]
here = Path(__file__).resolve().parent
PCH = "ds2_rex_diagnostic_pch.h"
STAMP = out / "patch_local_regs.stamp"
MANIFEST = out / "patch_local_regs.files"  # written by patch_local_regs.py: the files it owns in the mirror

patched = set()
if mode != "off" and STAMP.exists() and MANIFEST.exists():
    patched = {Path(l) for l in MANIFEST.read_text().split("\n") if l}

n = 0
for root, dirs, files in os.walk(src):
    rel = Path(root).relative_to(src)
    (out / rel).mkdir(parents=True, exist_ok=True)
    for f in files:
        if rel == Path(".") and f == PCH:
            continue
        if rel / f in patched:
            continue
        link = out / rel / f
        target = Path(root) / f
        if link.is_symlink() or link.exists():
            if link.is_symlink() and Path(os.readlink(link)) == target:
                continue
            link.unlink()
        link.symlink_to(target)
        n += 1
subprocess.check_call([sys.executable, str(here / "make_nx_pch.py"), str(src / PCH), str(out / PCH)])
print(f"{n} links in {out}")


def inputs():
    """What patch_local_regs.py's output depends on, as one hash: the tree (names, sizes, dates), the script, the
    mode, and in the source dirs the hooks and the guest-looking names (with an address suffix or a '__' scope)."""
    h = hashlib.sha256()
    h.update(mode.encode())
    h.update((here / "patch_local_regs.py").read_bytes())
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for f in sorted(files):
            st = os.stat(os.path.join(root, f))
            h.update(f"{os.path.relpath(os.path.join(root, f), src)} {st.st_size} {st.st_mtime_ns}\n".encode())
    names = set()
    for d in sources:
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith((".cpp", ".h", ".hpp", ".cc", ".inc")):
                    text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                    names.update(re.findall(r"REX_EXTERN\([^)]*\)\s*[{;]|#define \w+\(.*REX_EXTERN.*", text))
                    names.update(re.findall(r"^\w+\(\w+, \d+\)$", text, re.M))  # hook macros' uses (DS2_ETAPA)
                    names.update(re.findall(r"\b\w*(?:__\w+|_[0-9A-F]{8})\b", text))
    h.update("\n".join(sorted(names)).encode())
    return h.hexdigest()


if mode == "off":
    for p in (STAMP, MANIFEST):
        if p.exists():
            p.unlink()
else:
    stamp = inputs()
    if not STAMP.exists() or STAMP.read_text().strip() != stamp or not patched:
        env = dict(os.environ, PLR_CHECK="1" if mode == "check" else "0")
        subprocess.check_call([sys.executable, str(here / "patch_local_regs.py"), str(src), str(out)] + sources,
                              env=env)
        STAMP.write_text(stamp + "\n")
    else:
        print("patch_local_regs: up to date")
