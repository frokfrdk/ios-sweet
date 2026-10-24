#!/usr/bin/env python3
"""Replay a pending HFS+ journal into an image (offline), then mark the journal for re-init.

iOS on sunfish has no clean shutdown, so a pulled userdata image usually carries committed but
unapplied journal transactions. Linux's hfsplus driver ignores the journal; editing such an image and
flashing it back would let iOS replay stale metadata over the edits. This applies the transactions the
way XNU's replay_journal() does, then sets kJIJournalNeedInitMask and clears the journal header (the
same state a freshly built root image has), so XNU starts a fresh journal on the next mount.

usage: hfsreplay.py IMAGE [--dry-run]
"""
import struct
import sys

JHDR_CKSUM_SIZE = 44     # offsetof(journal_header, sequence_num)
BLHDR_CKSUM_SIZE = 32    # block_list_header + binfo[0]
BLHDR_CHECK_CHECKSUMS = 1
JI_NEED_INIT = 4


def calc_checksum(data):
    cksum = 0
    for c in data:
        cksum = ((cksum << 8) ^ (cksum + c)) & 0xffffffff
    return ~cksum & 0xffffffff


def main():
    path = sys.argv[1]
    dry = '--dry-run' in sys.argv
    f = open(path, 'rb' if dry else 'r+b')
    f.seek(0x400)
    vh = f.read(512)
    sig, _ver, attrs, _lmv, jib = struct.unpack_from('>2sHI4sI', vh, 0)
    bsize, = struct.unpack_from('>I', vh, 40)
    assert sig in (b'H+', b'HX') and attrs & (1 << 13), 'not a journaled HFS+ volume'
    f.seek(jib * bsize)
    ji_flags, = struct.unpack('>I', f.read(4))
    f.seek(jib * bsize + 36)
    joff, jsize = struct.unpack('>QQ', f.read(16))
    f.seek(joff)
    jh = bytearray(f.read(512))
    magic, endian = struct.unpack_from('<II', jh, 0)
    if magic != 0x4a4e4c78:
        print(f'no journal header (magic 0x{magic:x}, info flags 0x{ji_flags:x}); nothing to replay')
        return
    e = '<' if endian == 0x12345678 else '>'
    _m, _e, start, end, size, blhdr_size, cksum, jhdr_size, seq = struct.unpack_from(e + 'IIQQQIIII', jh, 0)
    hdr = bytearray(jh[:JHDR_CKSUM_SIZE])
    struct.pack_into(e + 'I', hdr, 36, 0)
    assert calc_checksum(hdr) == cksum, 'journal header checksum mismatch'
    assert size == jsize
    print(f'journal at 0x{joff:x}: start 0x{start:x} end 0x{end:x} size 0x{size:x} blhdr {blhdr_size} '
          f'jhdr {jhdr_size} seq 0x{seq:x}')

    def jread(off, n):
        out = b''
        while n:
            if off >= size:
                off = jhdr_size
            k = min(n, size - off)
            f.seek(joff + off)
            out += f.read(k)
            off += k
            n -= k
        return out, (jhdr_size if off >= size else off)

    writes = []  # (byte offset, data), in commit order
    off = start
    ntx = 0
    while off != end:
        b, data_off = jread(off, blhdr_size)
        mx, nb, used, ck, flags = struct.unpack_from(e + 'HHiIi', b, 0)
        h = bytearray(b[:BLHDR_CKSUM_SIZE])
        struct.pack_into(e + 'I', h, 8, 0)
        assert calc_checksum(h) == ck, f'block list header checksum mismatch at 0x{off:x}'
        assert 0 < nb <= mx + 1 and blhdr_size <= used, (nb, mx, used)
        total = blhdr_size
        for i in range(1, nb):
            bnum, bsz, bck = struct.unpack_from(e + 'QiI', b, 16 + 16 * i)
            data, data_off = jread(data_off, bsz)
            total += bsz
            if bnum == 0xffffffffffffffff:
                continue
            if flags & BLHDR_CHECK_CHECKSUMS:
                assert calc_checksum(data[:32]) == bck or calc_checksum(data) == bck, \
                    f'block checksum mismatch: bnum 0x{bnum:x}'
            writes.append((bnum * jhdr_size, data))
        assert total == used, (total, used)
        ntx += 1
        off = (off + used) if off + used < size else jhdr_size + (off + used - size)
    print(f'{ntx} block lists, {len(writes)} block writes, '
          f'{len({w[0] for w in writes})} distinct blocks, {sum(len(w[1]) for w in writes)} bytes')
    if dry:
        return
    for pos, data in writes:
        f.seek(pos)
        f.write(data)
    # fresh-journal state: header cleared, info block asks XNU to initialise a new journal
    f.seek(joff)
    f.write(bytes(512))
    f.seek(jib * bsize)
    f.write(struct.pack('>I', ji_flags | JI_NEED_INIT))
    f.flush()
    print(f'replayed; journal info flags 0x{ji_flags:x} -> 0x{ji_flags | JI_NEED_INIT:x}, header cleared')


if __name__ == '__main__':
    main()
