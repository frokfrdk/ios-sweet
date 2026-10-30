"""Decode a grid HEIC (with optional alpha auxiliary image) to an RGBA PNG using ffmpeg's HEVC decoder.

usage: heic2png.py in.heic out.png
Only what Apple's wallpaper assets use: grid items of hvc1 tiles, irot, an auxl alpha grid.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib


def boxes(b, off, end):
    while off + 8 <= end:
        size, typ = struct.unpack('>I4s', b[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack('>Q', b[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        yield typ.decode('latin1'), off + hdr, off + size
        off += size


def child(b, body, end, name):
    for t, s, e in boxes(b, body, end):
        if t == name:
            return s, e
    raise KeyError(name)


def uint(b, o, n):
    return int.from_bytes(b[o:o + n], 'big') if n else 0


class Heif:
    def __init__(self, data):
        self.b = b = data
        ms, me = child(b, 0, len(b), 'meta')
        ms += 4
        self.primary = struct.unpack('>H', b[child(b, ms, me, 'pitm')[0] + 4:][:2])[0]
        # items
        self.types = {}
        s, e = child(b, ms, me, 'iinf')
        ver = b[s]
        o = s + (6 if ver == 0 else 8)
        for t, s2, e2 in boxes(b, o, e):
            if b[s2] >= 2:
                self.types[struct.unpack('>H', b[s2 + 4:s2 + 6])[0]] = b[s2 + 8:s2 + 12].decode('latin1')
        # references
        self.dimg, self.auxl = {}, {}
        s, e = child(b, ms, me, 'iref')
        for t, s2, e2 in boxes(b, s + 4, e):
            fr, cnt = struct.unpack('>HH', b[s2:s2 + 4])
            to = list(struct.unpack('>%dH' % cnt, b[s2 + 4:s2 + 4 + 2 * cnt]))
            if t == 'dimg':
                self.dimg[fr] = to
            elif t == 'auxl':
                self.auxl[fr] = to[0]          # alpha item -> its master
        # properties
        ps, pe = child(b, ms, me, 'iprp')
        cs, ce = child(b, ps, pe, 'ipco')
        self.props = [(t, s2, e2) for t, s2, e2 in boxes(b, cs, ce)]
        self.assoc = {}
        s, e = child(b, ps, pe, 'ipma')
        ver, flags = b[s], uint(b, s + 1, 3)
        n = struct.unpack('>I', b[s + 4:s + 8])[0]
        o = s + 8
        for _ in range(n):
            iid = uint(b, o, 4 if ver >= 1 else 2)
            o += 4 if ver >= 1 else 2
            k = b[o]
            o += 1
            lst = []
            for _ in range(k):
                if flags & 1:
                    v = struct.unpack('>H', b[o:o + 2])[0] & 0x7fff
                    o += 2
                else:
                    v = b[o] & 0x7f
                    o += 1
                lst.append(v)
            self.assoc[iid] = lst
        # data
        self.idat = None
        try:
            s, e = child(b, ms, me, 'idat')
            self.idat = (s, e)
        except KeyError:
            pass
        self.loc = {}
        s, e = child(b, ms, me, 'iloc')
        ver = b[s]
        o = s + 4
        osz, lsz = b[o] >> 4, b[o] & 15
        bsz, isz = b[o + 1] >> 4, (b[o + 1] & 15) if ver in (1, 2) else 0
        o += 2
        n = uint(b, o, 2 if ver < 2 else 4)
        o += 2 if ver < 2 else 4
        for _ in range(n):
            iid = uint(b, o, 2 if ver < 2 else 4)
            o += 2 if ver < 2 else 4
            cm = 0
            if ver in (1, 2):
                cm = uint(b, o, 2) & 15
                o += 2
            o += 2                              # data_reference_index
            base = uint(b, o, bsz)
            o += bsz
            ec = uint(b, o, 2)
            o += 2
            ext = []
            for _ in range(ec):
                o += isz
                off = uint(b, o, osz)
                o += osz
                ln = uint(b, o, lsz)
                o += lsz
                ext.append((base + off, ln))
            self.loc[iid] = (cm, ext)

    def data(self, iid):
        cm, ext = self.loc[iid]
        start = self.idat[0] if cm == 1 else 0
        return b''.join(self.b[start + o:start + o + n] for o, n in ext)

    def prop(self, iid, name):
        for i in self.assoc.get(iid, []):
            t, s, e = self.props[i - 1]
            if t == name:
                return s, e
        return None

    def annexb_params(self, iid):
        s, e = self.prop(iid, 'hvcC')
        b = self.b
        nal_len = (b[s + 21] & 3) + 1
        n = b[s + 22]
        o = s + 23
        out = b''
        for _ in range(n):
            o += 1
            cnt = struct.unpack('>H', b[o:o + 2])[0]
            o += 2
            for _ in range(cnt):
                ln = struct.unpack('>H', b[o:o + 2])[0]
                out += b'\0\0\0\1' + b[o + 2:o + 2 + ln]
                o += 2 + ln
        return out, nal_len

    def tile_stream(self, tiles):
        out = b''
        for t in tiles:
            params, nl = self.annexb_params(t)
            d = self.data(t)
            o = 0
            out += params
            while o < len(d):
                ln = uint(d, o, nl)
                out += b'\0\0\0\1' + d[o + nl:o + nl + ln]
                o += nl + ln
        return out

    def decode(self, iid, gray):
        """-> (w, h, bytes) of the item, rgb24 or gray, before irot."""
        if self.types[iid] == 'grid':
            g = self.data(iid)
            rows, cols = g[2] + 1, g[3] + 1
            fl = 4 if g[1] & 1 else 2
            ow, oh = uint(g, 4, fl), uint(g, 4 + fl, fl)
            tiles = self.dimg[iid]
        else:
            rows = cols = 1
            tiles = [iid]
            s, e = self.prop(iid, 'ispe')
            ow, oh = struct.unpack('>II', self.b[s + 4:s + 12])
        s, e = self.prop(tiles[0], 'ispe')
        tw, th = struct.unpack('>II', self.b[s + 4:s + 12])
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, 't.265')
            open(src, 'wb').write(self.tile_stream(tiles))
            raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'hevc', '-i', src, '-f', 'rawvideo', '-pix_fmt',
                                  'gray' if gray else 'rgb24', '-'], check=True, stdout=subprocess.PIPE).stdout
        bpp = 1 if gray else 3
        fsz = tw * th * bpp
        assert len(raw) == fsz * len(tiles), (len(raw), fsz, len(tiles))
        rowsz = ow * bpp
        out = bytearray(ow * oh * bpp)
        for i in range(len(tiles)):
            r, c = divmod(i, cols)
            fr = raw[i * fsz:(i + 1) * fsz]
            for y in range(th):
                Y = r * th + y
                if Y >= oh:
                    break
                x0 = c * tw
                if x0 >= ow:
                    break
                n = min(tw, ow - x0) * bpp
                out[Y * rowsz + x0 * bpp:Y * rowsz + x0 * bpp + n] = fr[y * tw * bpp:y * tw * bpp + n]
        return ow, oh, bytes(out)

    def rotation(self, iid):
        p = self.prop(iid, 'irot')
        return (self.b[p[0]] & 3) if p else 0


def rotate(w, h, px, bpp, quarter):
    """Rotate anticlockwise by quarter*90 degrees."""
    for _ in range(quarter):
        out = bytearray(len(px))
        # anticlockwise: new (x', y') = (y, w-1-x); new width h, height w
        for y in range(h):
            for x in range(w):
                s = (y * w + x) * bpp
                d = ((w - 1 - x) * h + y) * bpp
                out[d:d + bpp] = px[s:s + bpp]
        px, w, h = bytes(out), h, w
    return w, h, px


def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def convert(src, dst):
    h = Heif(open(src, 'rb').read())
    p = h.primary
    w, ht, rgb = h.decode(p, False)
    alpha = None
    for a, m in h.auxl.items():
        if m == p:
            aw, ah, alpha = h.decode(a, True)
            assert (aw, ah) == (w, ht)
    rgba = bytearray(w * ht * 4)
    rgba[0::4], rgba[1::4], rgba[2::4] = rgb[0::3], rgb[1::3], rgb[2::3]
    rgba[3::4] = alpha if alpha is not None else b'\xff' * (w * ht)
    q = h.rotation(p)
    if q:
        w, ht, rgba = rotate(w, ht, bytes(rgba), 4, q)
    write_png(dst, w, ht, bytes(rgba))
    print('%s: %dx%d irot %d alpha %s' % (os.path.basename(src), w, ht, q, alpha is not None))


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])
