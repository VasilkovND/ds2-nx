#!/usr/bin/env python3
"""Make the Homebrew Menu icon (deadspace2/assets/icon.jpg, 256x256 JPEG) from your own disc.

The disc's nxeart package (the Xbox 360 dashboard art) holds a 1920x1080 background and a 420x320 picture of
Isaac's helmet. The helmet, cropped to its centre square, is the icon. Nothing from the disc is committed
(assets/ is ignored). Uses macOS's sips for the crop and the resize.

Usage: make_icon.py <disc1/nxeart> [<out.jpg>]
"""
import os
import subprocess
import sys
import tempfile


def jpegs(data):
    """(start, end) of each complete JPEG stream in data (SOI ... EOI)."""
    out, pos = [], 0
    while (start := data.find(b"\xff\xd8\xff", pos)) >= 0:
        end, depth, i = -1, 0, start
        # Thumbnails nest inside the big pictures: count SOI/EOI pairs.
        while i < len(data) - 1:
            if data[i] == 0xFF and data[i + 1] == 0xD8:
                depth += 1
                i += 2
            elif data[i] == 0xFF and data[i + 1] == 0xD9:
                depth -= 1
                i += 2
                if depth == 0:
                    end = i
                    break
            else:
                i += 1
        if end < 0:
            break
        out.append((start, end))
        pos = end
    return out


def size(path):
    info = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", path], capture_output=True, text=True,
                          check=True).stdout.split()
    return int(info[-3]), int(info[-1])


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join(os.path.dirname(__file__), "..", "assets", "icon.jpg")
    data = open(sys.argv[1], "rb").read()
    with tempfile.TemporaryDirectory() as tmp:
        helmet = None
        for n, (a, b) in enumerate(jpegs(data)):
            path = os.path.join(tmp, f"{n}.jpg")
            open(path, "wb").write(data[a:b])
            if size(path) == (420, 320):
                helmet = path
        if not helmet:
            sys.exit("no 420x320 picture in that nxeart")
        square = os.path.join(tmp, "square.png")
        subprocess.run(["sips", "-c", "320", "320", helmet, "--out", square], capture_output=True, check=True)
        subprocess.run(["sips", "-z", "256", "256", square], capture_output=True, check=True)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "92", square, "--out", out],
                       capture_output=True, check=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
