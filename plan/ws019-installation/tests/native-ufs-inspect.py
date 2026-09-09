#!/usr/bin/env python3
"""Independent native namespace/bitmap/accounting and persistence inspection."""
import struct
import sys

with open(sys.argv[1], "rb") as f:
    def read(offset, count):
        f.seek(offset)
        data = f.read(count)
        assert len(data) == count
        return data
    def u32(data, offset): return struct.unpack_from("<I", data, offset)[0]
    def u64(data, offset): return struct.unpack_from("<Q", data, offset)[0]
    sb = read(65536, 8192)
    size, ncg, fpg = u64(sb, 1080), u32(sb, 44), u32(sb, 188)
    assert u32(sb, 1372) == 0x19540119
    totals = [0, 0, 0, 0]
    for group in range(ncg):
        base = group * fpg
        count = min(fpg, size - base)
        used = 152 if group == 0 else 144
        cg = read((base + 72) * 1024, 8192)
        assert u32(cg, 20) == count
        values = [1 if group == 0 else 0, (count - used) // 8,
                  253 if group == 0 else 256, (count - used) % 8]
        assert [u32(cg, n) for n in (24, 28, 32, 36)] == values
        totals = [a + b for a, b in zip(totals, values)]
        assert cg[168:200] == (b"\x07" + bytes(31) if group == 0 else bytes(32))
        for frag in range(fpg):
            assert bool(cg[200 + frag // 8] & (1 << (frag % 8))) == (used <= frag < count)
        table = read((base + 80) * 1024, 65536)
        if group == 0:
            inode = table[512:768]
            assert struct.unpack_from("<HH", inode) == (0o40755, 2)
            assert u64(inode, 16) == 512 and u64(inode, 24) == 16
            assert u64(inode, 112) == 144
            assert not any(table[:512]) and not any(table[768:])
        else:
            assert not any(table)
            assert read((base + 64) * 1024, 8192) == sb
    assert [u64(sb, n) for n in (1008, 1016, 1024, 1032)] == totals
    root = read(144 * 1024, 8192)
    assert struct.unpack_from("<IHBB", root) == (2, 12, 4, 1)
    assert root[8:10] == b".\0"
    assert struct.unpack_from("<IHBB", root, 12) == (2, 500, 4, 2)
    assert root[20:23] == b"..\0" and not any(root[23:])
    journal = read(size * 1024, 512)
    assert journal[:4] == b"ZUJ2" and u32(journal, 4) == 2
    assert u64(journal, 12) == size * 2
    snapshot = read((size * 2 + 257) * 512, 512)
    assert snapshot[:4] == b"ZSL1" and u64(snapshot, 24) == size * 2
print(f"independent native UFS {size} fragments/{ncg} groups PASS")
