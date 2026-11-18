#!/usr/bin/env python3
"""zedBSD overlay journal wire-format helpers shared by image tools."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import struct
import zlib

RECORD_BYTES = 512
JOURNAL_BYTES = 128 * 1024
PROFILE_IDS = {"overlay": b"ZOVL"}


def _record_crc(record: bytes | bytearray) -> int:
    if len(record) != RECORD_BYTES:
        raise ValueError("overlay record must be exactly 512 bytes")
    return zlib.crc32(record[:508]) & 0xFFFFFFFF


def seal(record: bytearray) -> bytes:
    struct.pack_into("<I", record, 508, _record_crc(record))
    return bytes(record)


def empty_active_slot(name: str, epoch: int = 1) -> bytes:
    overlay_id = PROFILE_IDS[name]
    header = bytearray(RECORD_BYTES)
    header[:8] = b"ZOVLSLT\0"
    struct.pack_into("<HHI", header, 8, 1, 48, RECORD_BYTES)
    header[0x10:0x14] = overlay_id
    struct.pack_into("<QIIQ", header, 0x18, epoch, 0, 1, 0)
    header = bytearray(seal(header))
    commit = bytearray(RECORD_BYTES)
    commit[:8] = b"ZOVLCMT\0"
    struct.pack_into("<H", commit, 8, 1)
    commit[0x0C:0x10] = overlay_id
    struct.pack_into("<QIIQ", commit, 0x10, epoch, 0, 1, 0)
    struct.pack_into("<I", commit, 0x28, zlib.crc32(header) & 0xFFFFFFFF)
    commit = seal(commit)
    return bytes(header) + commit + bytes(JOURNAL_BYTES - 2 * RECORD_BYTES)


def validate_empty_active_slot(data: bytes, name: str) -> None:
    if len(data) != JOURNAL_BYTES:
        raise ValueError("overlay journal has the wrong size")
    header = data[:RECORD_BYTES]
    commit = data[RECORD_BYTES:2 * RECORD_BYTES]
    if header[:8] != b"ZOVLSLT\0" or _record_crc(header) != struct.unpack_from("<I", header, 508)[0]:
        raise ValueError("invalid overlay slot header")
    if struct.unpack_from("<HHI", header, 8) != (1, 48, RECORD_BYTES):
        raise ValueError("unsupported overlay slot header")
    if header[0x10:0x14] != PROFILE_IDS[name] or any(header[0x14:0x18]):
        raise ValueError("wrong overlay identifier")
    if struct.unpack_from("<QIIQ", header, 0x18) != (1, 0, 1, 0):
        raise ValueError("overlay slot is not the initial empty snapshot")
    if any(header[0x30:508]):
        raise ValueError("nonzero reserved header bytes")
    if commit[:8] != b"ZOVLCMT\0" or _record_crc(commit) != struct.unpack_from("<I", commit, 508)[0]:
        raise ValueError("invalid overlay commit")
    if struct.unpack_from("<H", commit, 8)[0] != 1 or any(commit[0x0A:0x0C]):
        raise ValueError("unsupported overlay commit")
    if commit[0x0C:0x10] != PROFILE_IDS[name]:
        raise ValueError("wrong overlay commit identifier")
    if struct.unpack_from("<QIIQ", commit, 0x10) != (1, 0, 1, 0):
        raise ValueError("wrong overlay commit bounds")
    if struct.unpack_from("<I", commit, 0x28)[0] != zlib.crc32(header) & 0xFFFFFFFF:
        raise ValueError("wrong overlay snapshot digest")
    if any(commit[0x2C:508]) or any(data[2 * RECORD_BYTES:]):
        raise ValueError("nonzero reserved journal tail")


def self_test() -> None:
    if zlib.crc32(b"123456789") & 0xFFFFFFFF != 0xCBF43926:
        raise RuntimeError("host CRC-32 implementation is incompatible")
    for name in PROFILE_IDS:
        validate_empty_active_slot(empty_active_slot(name), name)
