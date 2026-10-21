#!/usr/bin/env python3
"""Make the SD card folder for deadspace2 from your own discs and title update.

    python3 stage_sd.py --disc1 "Dead Space 2 (Disc 1).iso" [--disc2 "Dead Space 2 (Disc 2).iso"] \
        --tu <TU2 package or its disc001/default.xexp> -o <out>/ds2-nx

Writes, beside where the NRO goes (src/ds2_app.h reads this layout):
    ds2-nx/disc1/   the extracted Disc 1 (bigfile0.viv 3.3 GB and bigfile1.viv 3.4 GB: under FAT32's 4 GB)
    ds2-nx/disc2/   the extracted Disc 2, if given
    ds2-nx/tu/      default.xex (Disc 1) + default.xexp (Title Update 2)

Every input is checked against the hashes the recompiled code was made from (the same pins as
Dead-Space-2-Recomp's launcher): the code is TU2, so any other executable or patch would not match it.
Only the standard library (the title update package is read by the small STFS reader below).

Two editions are accepted, each as its own disc and its own TU2 package (the patch only fits the disc it was made for):
    World (En/Fr/Es)   Media ID 2915C8F8   TU_12K226V_000000C000000.00000000000G3
    Russia (En/Ru)     Media ID 2C199AEB   TU_12K226V_000000K000000.00000000000G5
After their TU2 the two executables recompile to the same code, so one NRO runs both.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

# TU2 disc001/default.xexp -> (edition, Media ID, Disc 1 default.xex)
EDITIONS = {
    "8d386d37e8f71937b9d9c4b34bdbf85240101479c8aaece1002d94e7cdc01ec2":
        ("World, En/Fr/Es", "2915C8F8", "928597ddb80240b8aefeef0a70a9476ee85d6de733035b326dd155c2094a3408"),
    "6eb88610317fddb84984a9d8cf8eadaffe7d02ff867aa67e7dc70ecccd4c3b5c":
        ("Russia, En/Ru", "2C199AEB", "7fac7f815aaccdfedfd1ef7c2b6e6e43cc50af72d46ac22776a7e8a0daed6cc8"),
}
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
class Stfs:
    """The parts of an Xbox 360 STFS package (CON/LIVE/PIRS) needed to read one file from it."""

    BLOCK = 0x1000

    def __init__(self, path: Path):
        self.f = open(path, "rb")
        head = self.read(0, 0x3A0)
        if head[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            sys.exit(f"{path} is not an Xbox 360 package (STFS)")
        self.title_id = struct.unpack_from(">I", head, 0x360)[0]
        header_size = struct.unpack_from(">I", head, 0x340)[0]
        self.data = (header_size + 0xFFF) & ~0xFFF
        volume = head[0x379:0x379 + 0x24]
        # Read-only packages keep one hash table per level, the others two.
        self.tables = 1 if volume[2] & 1 else 2
        self.table_blocks = struct.unpack_from("<H", volume, 3)[0]
        self.table_start = int.from_bytes(volume[5:8], "little")

    def read(self, offset: int, n: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(n)

    def block(self, b: int) -> bytes:
        # Data blocks are numbered without the hash table blocks between them: one level 0 table every 0xAA blocks,
        # one level 1 every 0x70E4, one level 2 every 0x4AF768.
        backing = b + (b + 0xAA) // 0xAA * self.tables
        if b >= 0xAA:
            backing += (b + 0x70E4) // 0x70E4 * self.tables
            if b >= 0x70E4:
                backing += (b + 0x4AF768) // 0x4AF768 * self.tables
        return self.read(self.data + backing * self.BLOCK, self.BLOCK)

    def entries(self) -> list:
        out = []
        for k in range(self.table_blocks):
            table = self.block(self.table_start + k)
            for i in range(0, self.BLOCK, 0x40):
                e = table[i:i + 0x40]
                if not e[0]:
                    continue
                flags = e[0x28]
                out.append({"name": e[:flags & 0x3F].decode("latin-1"), "dir": bool(flags & 0x80),
                            "contig": bool(flags & 0x40), "blocks": int.from_bytes(e[0x29:0x2C], "little"),
                            "start": int.from_bytes(e[0x2F:0x32], "little"),
                            "parent": struct.unpack_from(">h", e, 0x32)[0],
                            "size": struct.unpack_from(">I", e, 0x34)[0]})
        return out


def read_xexp(path: Path) -> bytes:
    if path.stat().st_size < 16 << 20:
        data = path.read_bytes()
        if sha256(data) in EDITIONS:
            return data
    pkg = Stfs(path)
    try:
        if pkg.title_id != TITLE_ID:
            sys.exit(f"{path} is not a Dead Space 2 package")
        entries = pkg.entries()
        for e in entries:
            parent = entries[e["parent"]]["name"] if 0 <= e["parent"] < len(entries) else ""
            if e["dir"] or f"{parent}/{e['name']}".casefold() != "disc001/default.xexp":
                continue
            if not e["contig"]:
                sys.exit("disc001/default.xexp is fragmented in this package: extract it with another tool and pass "
                         "the .xexp file to --tu")
            data = b"".join(pkg.block(e["start"] + k) for k in range(e["blocks"]))[:e["size"]]
            if sha256(data) not in EDITIONS:
                sys.exit("disc001/default.xexp is not a Title Update 2 this port supports (hash mismatch): use the "
                         "package with Media ID 2915C8F8 (World) or 2C199AEB (Russia)")
            return data
        sys.exit(f"{path} has no disc001/default.xexp")
    finally:
        pkg.f.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc1", type=Path, required=True)
    ap.add_argument("--disc2", type=Path)
    ap.add_argument("--tu", type=Path, required=True, help="TU2 package, or its disc001/default.xexp")
    ap.add_argument("--ds2-recomp", type=Path, help=argparse.SUPPRESS)  # no longer needed, accepted as before
    ap.add_argument("-o", "--out", type=Path, required=True)
    a = ap.parse_args()
    for name, path in (("--disc1", a.disc1), ("--disc2", a.disc2), ("--tu", a.tu)):
        if path is not None and not path.is_file():
            sys.exit(f"{name}: {path} not found")

    xexp = read_xexp(a.tu)
    edition, media_id, xex_sha = EDITIONS[sha256(xexp)]
    print(f"  TU2 default.xexp: ok ({edition}, Media ID {media_id})")
    extract(a.disc1, a.out / "disc1")
    xex = (a.out / "disc1" / "default.xex").read_bytes()
    if sha256(xex) != xex_sha:
        other = [e for e in EDITIONS.values() if e[2] == sha256(xex)]
        sys.exit(f"Disc 1 default.xex is from the {other[0][0]} edition, but the TU2 is for {edition}: use the TU2 "
                 f"package with Media ID {other[0][1]}" if other else
                 f"Disc 1 default.xex does not match the TU2's edition (Dead Space 2, {edition})")
    if a.disc2:
        extract(a.disc2, a.out / "disc2")
    tu = a.out / "tu"
    tu.mkdir(parents=True, exist_ok=True)
    (tu / "default.xex").write_bytes(xex)
    (tu / "default.xexp").write_bytes(xexp)
    print(f"Ready: copy the contents of {a.out} into sdmc:/switch/ds2-nx/ (next to ds2-nx.nro).")


if __name__ == "__main__":
    main()
