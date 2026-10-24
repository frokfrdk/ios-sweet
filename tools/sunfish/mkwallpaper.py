#!/usr/bin/env python3
"""Draw a bokeh-style wallpaper PNG (vertical gradient + soft circles), no dependencies.

    python3 -I tools/sunfish/mkwallpaper.py OUT.png [WIDTH HEIGHT]

Defaults to 828x1792, the 414w-896h@2x screen of the 828xr ADT. Colours follow WallpaperKit's
"Bokeh Blue" parameters (gradient #67C7F0 -> #2F82BA, pale cyan bubbles).
"""
import random
import struct
import sys
import zlib

out = sys.argv[1]
W, H = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (828, 1792)
TOP, BOTTOM = (0x67, 0xC7, 0xF0), (0x2F, 0x82, 0xBA)
BUBBLES = [(0xBF, 0xFF, 0xFA), (0xCC, 0xFF, 0xFB), (0xB2, 0xFF, 0xF9)]

img = bytearray(W * H * 3)
for y in range(H):
    t = y / (H - 1)
    px = bytes(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
    img[y * W * 3:(y + 1) * W * 3] = px * W

rng = random.Random(16)
for _ in range(34):
    r = rng.uniform(0.05, 0.19) * W
    cx, cy = rng.uniform(-0.1, 1.1) * W, rng.uniform(-0.05, 1.05) * H
    col = rng.choice(BUBBLES)
    alpha = rng.uniform(0.10, 0.30)
    soft = r * 0.18
    for y in range(max(0, int(cy - r)), min(H, int(cy + r) + 1)):
        dy2 = (y - cy) ** 2
        for x in range(max(0, int(cx - r)), min(W, int(cx + r) + 1)):
            d = ((x - cx) ** 2 + dy2) ** 0.5
            if d >= r:
                continue
            a = alpha * min(1.0, (r - d) / soft)
            i = (y * W + x) * 3
            for c in range(3):
                img[i + c] = round(img[i + c] + (col[c] - img[i + c]) * a)


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


raw = b''.join(b'\0' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
with open(out, 'wb') as f:
    f.write(png)
print(f'{out}: {W}x{H}, {len(png)} bytes')
