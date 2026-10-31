#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png: binary PPM (P6) to PNG, standard library only."""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
magic, size, maxval, pixels = data.split(b"\n", 3)
if magic != b"P6" or maxval != b"255":
    sys.exit("ppm2png: only 8-bit P6 images")
w, h = map(int, size.split())
raw = b"".join(b"\0" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
with open(sys.argv[2], "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
