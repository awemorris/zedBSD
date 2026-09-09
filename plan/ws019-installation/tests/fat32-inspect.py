#!/usr/bin/env python3
"""Independent BPB/FAT/FSInfo inspection; no production formatter imports."""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
with path.open("rb") as f:
    boot = f.read(512)
    u16 = lambda n: struct.unpack_from("<H", boot, n)[0]
    u32 = lambda n: struct.unpack_from("<I", boot, n)[0]
    bps, spc, reserved, fats = u16(11), boot[13], u16(14), boot[16]
    total, fatlen, root = u32(32), u32(36), u32(44)
    assert bps in (512, 1024, 2048, 4096) and spc & (spc - 1) == 0
    assert total * bps == path.stat().st_size
    assert reserved == 32 and fats == 2 and root == 2
    assert u16(17) == u16(19) == u16(22) == u16(40) == u16(42) == 0
    assert u32(28) == 2048 and u32(67) == 0x12345678
    assert boot[510:] == b"\x55\xaa" and boot[82:90] == b"FAT32   "
    data = reserved + fats * fatlen
    clusters = (total - data) // spc
    assert 65525 <= clusters <= 0x0fffffee
    assert fatlen * bps // 4 >= clusters + 2
    f.seek(6 * bps); assert f.read(512) == boot
    for s in (1, 7):
        f.seek(s * bps); info = f.read(bps)
        assert struct.unpack_from("<I", info, 0)[0] == 0x41615252
        assert struct.unpack_from("<IIII", info, 484)[:3] == (0x61417272, clusters - 1, 3)
        assert struct.unpack_from("<I", info, 508)[0] == 0xaa550000
    for s in (reserved, reserved + fatlen):
        f.seek(s * bps); table = f.read(fatlen * bps)
        assert struct.unpack_from("<III", table) == (0xffffff8, 0xfffffff, 0xfffffff)
        assert not any(table[12:])
    f.seek(data * bps); assert not any(f.read(spc * bps))
    # All unallocated bytes retain the independently prepared preimage.
    free_start = (data + spc) * bps
    f.seek(free_start)
    assert f.read(512) == bytes([0x6d]) * 512
    remaining = total * bps - free_start - 1024
    while remaining:
        block = f.read(min(remaining, 1048576))
        assert block and not any(block)
        remaining -= len(block)
    assert f.read(512) == bytes([0x6d]) * 512
print(f"independent FAT32 inspection {bps} PASS")
