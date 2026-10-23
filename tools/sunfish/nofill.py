#!/usr/bin/env python3
# Rewrite an Android sparse image so every FILL chunk becomes a RAW chunk (same content).
# Pixel 4a ABL appears to write FILL chunks extremely slowly; RAW chunks go at ~300 MB/s.
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
f = open(src, 'rb'); o = open(dst, 'wb')
hdr = f.read(28)
magic, maj, mnr, fhs, chs, blk, total_blks, total_chunks, crc = struct.unpack('<IHHHHIIII', hdr)
assert magic == 0xED26FF3A and fhs == 28 and chs == 12
o.write(hdr)
for i in range(total_chunks):
    t, r, nblk, sz = struct.unpack('<HHII', f.read(12))
    if t == 0xCAC2:                                  # FILL: 4-byte pattern repeated
        pat = f.read(4)
        data_len = nblk * blk
        o.write(struct.pack('<HHII', 0xCAC1, 0, nblk, 12 + data_len))
        one = pat * (blk // 4)
        for _ in range(nblk):
            o.write(one)
    else:
        o.write(struct.pack('<HHII', t, r, nblk, sz))
        left = sz - 12
        while left:
            buf = f.read(min(left, 1 << 24)); o.write(buf); left -= len(buf)
o.close()
print("done", dst)
