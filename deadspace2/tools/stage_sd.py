#!/usr/bin/env python3
"""Make the SD card folder for deadspace2 from your own discs and title update.

    python3 stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" [--disc2 "Dead Space 2 (Disc 2).iso"] \
        --tu <TU2 package or its disc001/default.xexp> --ds2-recomp <Dead-Space-2-Recomp checkout> \
        -o <out>/ds2-nx

Writes, beside where the NRO goes (src/ds2_app.h reads this layout):
    ds2-nx/disc1/   the extracted Disc 1 (bigfile0.viv 3.3 GB and bigfile1.viv 3.4 GB: under FAT32's 4 GB)
    ds2-nx/disc2/   the extracted Disc 2, if given
    ds2-nx/tu/      default.xex (Disc 1) + default.xexp (Title Update 2)

Every input is checked against the hashes the recompiled code was made from (the same pins as
Dead-Space-2-Recomp's launcher): the code is TU2, so any other executable or patch would not match it.
Only the standard library; the title update package is read with Dead-Space-2-Recomp's tools/extract_dlc.py.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

XEX_SHA = "928597ddb80240b8aefeef0a70a9476ee85d6de733035b326dd155c2094a3408"   # Disc 1 default.xex
XEXP_SHA = "8d386d37e8f71937b9d9c4b34bdbf85240101479c8aaece1002d94e7cdc01ec2"  # TU2 disc001/default.xexp
TITLE_ID = 0x454108DF
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- XDVDFS (Xbox 360 disc) -------------------------------------------------------------------------------
def partition(f) -> int:
    for base in (0x0, 0x2080000, 0xFD90000, 0x18300000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    sys.exit("Not an Xbox 360 disc image (no XDVDFS volume found)")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    pending, seen = [0], set()
    while pending:
        o = pending.pop()
        p = o * 4
        if o in seen or p + 14 > len(table):
            continue
        seen.add(o)
        left, right, start, length, attr, nlen = struct.unpack("<HHIIBB", table[p:p + 14])
        if left == 0xFFFF:
            continue
        name = table[p + 14:p + 14 + nlen].decode("latin1")
        if left:
            pending.append(left)
        if right:
            pending.append(right)
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            sys.exit(f"Unsafe name on disc: {name!r}")
        path = f"{prefix}{name}"
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length


def extract(iso: Path, out: Path) -> None:
    with iso.open("rb") as f:
        base = partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root, root_size = struct.unpack("<II", f.read(8))
        files = sorted(walk(f, base, root, root_size))
        total = sum(n for _, _, n in files)
        done = 0
        for path, start, length in files:
            dest = out / path
            if dest.is_file() and dest.stat().st_size == length:
                done += length
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            f.seek(base + start * SECTOR)
            left = length
            with dest.open("wb") as o:
                while left:
                    chunk = f.read(min(8 << 20, left))
                    if not chunk:
                        sys.exit(f"{iso}: image ends inside {path}")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    print(f"\r  {iso.name}: {done * 100 // max(total, 1)}%", end="", flush=True)
        print(f"\r  {iso.name}: {len(files)} files, {total / 2**30:.1f} GiB -> {out}")


# --- Title update ------------------------------------------------------------------------------------------
def read_xexp(path: Path, ds2_recomp: Path) -> bytes:
    if path.stat().st_size < 16 << 20:
        data = path.read_bytes()
        if sha256(data) == XEXP_SHA:
            return data
    sys.path.insert(0, str(ds2_recomp / "tools"))
    from extract_dlc import Package  # Dead-Space-2-Recomp's STFS reader

    pkg = Package(path)
    try:
        if pkg.title_id != TITLE_ID:
            sys.exit(f"{path} is not a Dead Space 2 package")
        entries = pkg.entries()
        names = {}
        for i, e in enumerate(entries):
            if e is None:
                continue
            parts, j = [e["name"]], e["parent"]
            while 0 <= j < len(entries) and entries[j] and len(parts) < 16:
                parts.insert(0, entries[j]["name"])
                j = entries[j]["parent"]
            names[i] = "/".join(parts).casefold()
        for i, name in names.items():
            e = entries[i]
            if name == "disc001/default.xexp" and not e["dir"]:
                data = b"".join(pkg.blocks(e["start"], e["blocks"], e["contig"]))[:e["size"]]
                if sha256(data) != XEXP_SHA:
                    sys.exit("disc001/default.xexp is not Title Update 2 (hash mismatch)")
                return data
        sys.exit(f"{path} has no disc001/default.xexp")
    finally:
        pkg.f.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc1", type=Path, required=True)
    ap.add_argument("--disc2", type=Path)
    ap.add_argument("--tu", type=Path, required=True, help="TU2 package, or its disc001/default.xexp")
    ap.add_argument("--ds2-recomp", type=Path, default=Path("."), help="Dead-Space-2-Recomp checkout")
    ap.add_argument("-o", "--out", type=Path, required=True)
    a = ap.parse_args()

    xexp = read_xexp(a.tu, a.ds2_recomp)
    print("  TU2 default.xexp: ok")
    extract(a.disc1, a.out / "disc1")
    xex = (a.out / "disc1" / "default.xex").read_bytes()
    if sha256(xex) != XEX_SHA:
        sys.exit("Disc 1 default.xex does not match the supported release (Dead Space 2, World, En/Fr/Es)")
    if a.disc2:
        extract(a.disc2, a.out / "disc2")
    tu = a.out / "tu"
    tu.mkdir(parents=True, exist_ok=True)
    (tu / "default.xex").write_bytes(xex)
    (tu / "default.xexp").write_bytes(xexp)
    print(f"Ready: copy {a.out} to sdmc:/switch/ds2-nx/ and put ds2-nx.nro in it.")


if __name__ == "__main__":
    main()
