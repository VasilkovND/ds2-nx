#!/usr/bin/env python3
"""Mirror Dead Space 2's recompiled tree (src_tu2/) for the Switch build.

The generated files include "ds2_rex_diagnostic_pch.h" with quotes, which always finds
the copy next to the includer first. So the Switch build compiles a mirror of the tree
made of symlinks, where only the PCH is replaced by the Switch one (make_nx_pch.py).
Nothing from the game is copied: the links point at your checkout of Dead-Space-2-Recomp.

Usage: prepare_tree.py <Dead-Space-2-Recomp/src_tu2> <out_dir>
"""
import os, subprocess, sys
from pathlib import Path

src = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
here = Path(__file__).resolve().parent
PCH = "ds2_rex_diagnostic_pch.h"

n = 0
for root, dirs, files in os.walk(src):
    rel = Path(root).relative_to(src)
    (out / rel).mkdir(parents=True, exist_ok=True)
    for f in files:
        if rel == Path(".") and f == PCH:
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
