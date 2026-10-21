#!/usr/bin/env python3
"""Draw the neutral Homebrew Menu icon (deadspace2/res/icon_neutral.jpg, 256x256 JPEG): "DS2" and "NX" in block
letters on a dark background, nothing from the game. The build uses it with -DDS2_ICON_NEUTRAL=ON (by default it
uses the game's, res/icon.jpg from make_icon.py). Uses macOS's sips to write the JPEG.

Usage: make_release_icon.py [<out.jpg>]
"""
import os
import struct
import subprocess
import sys
import tempfile

# 5x7 block letters, rows top to bottom, bit 4 = left column.
GLYPHS = {
    "D": [0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E],
    "S": [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E],
    "2": [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
    "N": [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
    "X": [0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11],
}
SIZE = 256
BACKGROUND = (14, 18, 22)
TEXT = (210, 216, 220)
ACCENT = (40, 200, 190)


def draw(pixels, text, scale, top, color):
    width = len(text) * 6 * scale - scale
    left = (SIZE - width) // 2
    for n, ch in enumerate(text):
        for row, bits in enumerate(GLYPHS[ch]):
            for col in range(5):
                if bits >> (4 - col) & 1:
                    for y in range(top + row * scale, top + (row + 1) * scale):
                        for x in range(left + (n * 6 + col) * scale, left + (n * 6 + col + 1) * scale):
                            pixels[y][x] = color


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "res", "icon_neutral.jpg")
    pixels = [[BACKGROUND] * SIZE for _ in range(SIZE)]
    draw(pixels, "DS2", 12, 44, TEXT)
    for y in range(140, 146):  # a thin accent line between the two words
        for x in range(56, 200):
            pixels[y][x] = ACCENT
    draw(pixels, "NX", 8, 166, ACCENT)
    row = SIZE * 3
    header = b"BM" + struct.pack("<IHHI", 54 + row * SIZE, 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, SIZE, SIZE, 1, 24, 0, row * SIZE, 2835, 2835, 0, 0)
    body = b"".join(bytes(c for (r, g, b) in pixels[y] for c in (b, g, r)) for y in reversed(range(SIZE)))
    with tempfile.TemporaryDirectory() as tmp:
        bmp = os.path.join(tmp, "icon.bmp")
        open(bmp, "wb").write(header + body)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "95", bmp, "--out", out],
                       capture_output=True, check=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
