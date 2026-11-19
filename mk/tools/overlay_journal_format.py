#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""The overlay journal's empty slot, as the data image carries it.

A slot is 128 KiB: a 512-byte header record (ZOVLSLT), a 512-byte
commit record (ZOVLCMT) that names the header by its CRC-32, and zeros.
Each record ends in the CRC-32 of its first 508 bytes.
"""

from __future__ import annotations

import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zedbuild import put16le, put32le, put64le  # noqa: E402

RECORD_BYTES = 512
JOURNAL_BYTES = 128 * 1024
OVERLAY_ID = b"ZOVL"


def crc32(data: bytes | bytearray) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def empty_journal() -> bytes:
    """The slot of an overlay that has nothing written yet: epoch 1."""
    data = bytearray(JOURNAL_BYTES)
    data[0:7] = b"ZOVLSLT"
    put16le(data, 8, 1)
    put16le(data, 10, 48)
    put32le(data, 12, RECORD_BYTES)
    data[16:20] = OVERLAY_ID
    put64le(data, 24, 1)
    put32le(data, 36, 1)
    put32le(data, 508, crc32(data[0:508]))
    c = RECORD_BYTES
    data[c:c + 7] = b"ZOVLCMT"
    put16le(data, c + 8, 1)
    data[c + 12:c + 16] = OVERLAY_ID
    put64le(data, c + 16, 1)
    put32le(data, c + 28, 1)
    put32le(data, c + 40, crc32(data[0:RECORD_BYTES]))
    put32le(data, c + 508, crc32(data[c:c + 508]))
    return bytes(data)
