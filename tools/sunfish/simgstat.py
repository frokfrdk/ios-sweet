#!/usr/bin/env python3
# Summarize an Android sparse image: totals per chunk type, and the tail (what fastboot's last split sends).
import struct, sys
f = open(sys.argv[1], 'rb')
magic, maj, mnr, fhs, chs, blk, total_blks, total_chunks, _ = struct.unpack('<IHHHHIIII', f.read(28))
f.seek(fhs)
kinds = {0xCAC1: 'RAW', 0xCAC2: 'FILL', 0xCAC3: 'DONT_CARE', 0xCAC4: 'CRC'}
tot = {}; chunks = []
pos = 0
for i in range(total_chunks):
    t, _, nblk, sz = struct.unpack('<HHII', f.read(chs))
    k = kinds.get(t, hex(t)); tot[k] = tot.get(k, 0) + nblk * blk
    chunks.append((k, pos, nblk * blk)); pos += nblk * blk
    f.seek(sz - chs, 1)
print(f"block {blk}, {total_blks*blk/2**30:.2f} GiB, {total_chunks} chunks")
for k, v in tot.items():
    print(f"  {k}: {v/2**30:.2f} GiB")
# Tail: chunks in the last ~1 GiB of output space, biggest first
big = sorted(chunks[-200:], key=lambda c: -c[2])[:6]
for k, p, s in big:
    print(f"  tail {k} at {p/2**30:.2f} GiB size {s/2**20:.1f} MiB")
