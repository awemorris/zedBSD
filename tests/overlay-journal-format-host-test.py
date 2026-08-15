#!/usr/bin/env python3
"""Golden and corruption tests for the overlay journal image contract."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from overlay_journal_format import (JOURNAL_BYTES, empty_active_slot,
                                    self_test, validate_empty_active_slot)


def rejected(data: bytes, name: str) -> None:
    try:
        validate_empty_active_slot(data, name)
    except ValueError:
        return
    raise AssertionError("corrupt overlay journal was accepted")


def main() -> None:
    self_test()
    for name in ("bin", "lib"):
        golden = empty_active_slot(name)
        assert len(golden) == JOURNAL_BYTES
        validate_empty_active_slot(golden, name)
        for offset in (0, 8, 0x10, 0x18, 0x20, 0x24, 508,
                       512, 512 + 8, 512 + 0x0C, 512 + 0x10,
                       512 + 0x1C, 512 + 0x28, 1024):
            corrupt = bytearray(golden)
            corrupt[offset] ^= 0x80
            rejected(bytes(corrupt), name)
        rejected(golden[:-1], name)
        rejected(bytes(JOURNAL_BYTES), name)
    print("zedBSD overlay journal wire-format tests: PASS")


if __name__ == "__main__":
    main()

