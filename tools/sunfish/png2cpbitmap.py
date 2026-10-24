#!/usr/bin/env python3
"""Convert a PNG into an AppSupport CPBitmap, the format of ~mobile/Library/SpringBoard/*Background.cpbitmap.

    python3 -I tools/sunfish/png2cpbitmap.py IN.png OUT.cpbitmap [--size BYTES]

PaperBoardUI's directory data store decodes those files with CPBitmapCreateImagesFromData (AppSupport),
which has no ImageIO fallback for plain images: without a valid trailer it fails ("Invalid magic in
trailer") and the wallpaper stays black. Layout read from the 20H392 dyld cache:

    image pixels | colour-space plist (A bytes) | B spare bytes | trailer
    trailer = 7 x u32 LE: A, B, width, height, format, count, magic 0xDCB543A2

Format 0 is 32-bit BGRX (kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst) in device RGB
(A = 0), rows padded to a multiple of 16 pixels (8 with the older 24-byte trailer, magic 0xCBA43291,
which has no A field). The file length must be exactly bytesPerRow * height + A + B + 28. Formats 4/6/7
wrap a KTX container for ImageIO instead.

--size BYTES: write exactly BYTES, using the largest 16-pixel-aligned image of the same aspect ratio that
fits and B for the rest. This allows an in-place rewrite of an existing file's data blocks (no HFS+
metadata change); the wallpaper view scales the smaller image up to the screen.

Reads 8-bit RGB/RGBA non-interlaced PNGs, no dependencies.
"""
import struct
import sys
import zlib

MAGIC = 0xDCB543A2
TRAILER = 28


def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, hdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _comp, _filt, interlace = hdr
    assert depth == 8 and ctype in (2, 6) and interlace == 0, 'need an 8-bit RGB/RGBA non-interlaced PNG'
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xff
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xff
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def resample(w, h, bpp, rows, nw, nh):
    """Box-filter downscale (or nearest upscale) to nw x nh, RGB out."""
    out = []
    for y in range(nh):
        y0, y1 = y * h // nh, max(y * h // nh + 1, (y + 1) * h // nh)
        line = bytearray(nw * 3)
        for x in range(nw):
            x0, x1 = x * w // nw, max(x * w // nw + 1, (x + 1) * w // nw)
            acc = [0, 0, 0]
            for sy in range(y0, y1):
                r = rows[sy]
                for sx in range(x0, x1):
                    i = sx * bpp
                    acc[0] += r[i]
                    acc[1] += r[i + 1]
                    acc[2] += r[i + 2]
            n = (y1 - y0) * (x1 - x0)
            line[x * 3:x * 3 + 3] = bytes(v // n for v in acc)
        out.append(bytes(line))
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[sys.argv.index('--size') + 1]) if '--size' in sys.argv else None
    w, h, bpp, rows = read_png(src)
    nw, nh = w, h
    if size is not None:
        nw = (w // 16) * 16
        while nw >= 16:
            nh = round(nw * h / w)
            if ((nw + 15) // 16) * 16 * 4 * nh + TRAILER <= size:
                break
            nw -= 16
        assert nw >= 16, f'{size} bytes is too small'
    if (nw, nh) != (w, h):
        rows, bpp = resample(w, h, bpp, rows, nw, nh), 3
    pw = (nw + 15) // 16 * 16
    pix = bytearray(pw * 4 * nh)
    for y, r in enumerate(rows):
        o = y * pw * 4
        for x in range(nw):
            i = x * bpp
            pix[o + x * 4:o + x * 4 + 4] = bytes((r[i + 2], r[i + 1], r[i], 0xff))
    spare = 0 if size is None else size - len(pix) - TRAILER
    out = bytes(pix) + bytes(spare) + struct.pack('<7I', 0, spare, nw, nh, 0, 1, MAGIC)
    assert size is None or len(out) == size
    with open(dst, 'wb') as f:
        f.write(out)
    print(f'{dst}: {nw}x{nh} (from {w}x{h}), bytesPerRow {pw * 4}, spare {spare}, {len(out)} bytes')


if __name__ == '__main__':
    main()
