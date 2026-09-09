#!/usr/bin/env python3
"""Inspect actual codec output independently using Python struct/zlib/UUID."""
import pathlib
import struct
import sys
import uuid
import zlib

for sector in (512, 4096):
    data = (pathlib.Path(sys.argv[1]) / f"init-{sector}.bin").read_bytes()
    size, sectors = struct.unpack_from("<IQ", data)
    assert size == sector and len(data) == 12 + 2 * 65536
    head, tail = data[12:12 + 65536], data[12 + 65536:]

    def region(lba, length):
        offset = lba * sector
        if offset < len(head):
            return head[offset:offset + length]
        offset -= sectors * sector - len(tail)
        return tail[offset:offset + length]

    assert head[510:512] == b"\x55\xaa"
    assert head[:446] == bytes(446)
    assert head[450] == 0xEE
    assert struct.unpack_from("<II", head, 454) == (1, 0xFFFFFFFF)
    assert head[462:510] == bytes(48)
    tables = []
    for lba in (1, sectors - 1):
        header = region(lba, sector)
        assert header[:8] == b"EFI PART"
        revision, length, crc, reserved = struct.unpack_from("<IIII", header, 8)
        assert (revision, length, reserved) == (0x10000, 92, 0)
        check = bytearray(header[:length])
        check[16:20] = bytes(4)
        assert zlib.crc32(check) == crc
        current, alternate, first, last = struct.unpack_from("<QQQQ", header, 24)
        assert current == lba and alternate == (sectors - 1 if lba == 1 else 1)
        assert first == 2 + 16384 // sector
        assert last == sectors - 2 - 16384 // sector
        assert str(uuid.UUID(bytes_le=header[56:72])) == "12345678-1234-4567-890a-bcdef0123456"
        table_lba, slots, stride, table_crc = struct.unpack_from("<QIII", header, 72)
        assert (slots, stride) == (128, 128)
        assert table_lba == (2 if lba == 1 else sectors - 1 - 16384 // sector)
        assert header[92:] == bytes(sector - 92)
        entries = region(table_lba, slots * stride)
        assert zlib.crc32(entries) == table_crc
        assert str(uuid.UUID(bytes_le=entries[:16])) == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
        assert str(uuid.UUID(bytes_le=entries[16:32])) == "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"
        assert struct.unpack_from("<QQQ", entries, 32) == (2048, 6143, 0)
        assert entries[56:128].decode("utf-16-le").rstrip("\0") == "ESP"
        assert entries[128:] == bytes(16384 - 128)
        tables.append(entries)
    assert tables[0] == tables[1]
print("GPT initialization: independent 512/4096 field and CRC inspection PASS")
