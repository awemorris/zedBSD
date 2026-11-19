#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Helpers shared by the host-side build scripts in mk/tools."""

from __future__ import annotations

import os
import sys


def fail(message: str) -> None:
    """Reports MESSAGE on the standard error and stops with a failure."""
    print(message, file=sys.stderr)
    raise SystemExit(1)


def parse_number(text: str) -> int:
    """Reads a decimal number, or a hexadecimal one written 0x..., with an
    optional leading minus.  Anything else stops the script, so that a
    mistyped size is not taken as some other one."""
    negative = text.startswith("-")
    pos = 1 if negative else 0
    base = 10
    if text[pos:pos + 2] == "0x":
        base = 16
        pos += 2
    if pos >= len(text):
        fail("invalid number: " + text)
    value = 0
    for c in text[pos:]:
        digit = -1
        if "0" <= c <= "9":
            digit = ord(c) - ord("0")
        elif "A" <= c <= "F":
            digit = ord(c) - ord("A") + 10
        elif "a" <= c <= "f":
            digit = ord(c) - ord("a") + 10
        if digit < 0 or digit >= base:
            fail("invalid number: " + text)
        value = value * base + digit
    return -value if negative else value


def put16le(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 2] = (value & 0xFFFF).to_bytes(2, "little")


def put32le(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 4] = (value & 0xFFFFFFFF).to_bytes(4, "little")


def put64le(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 8] = (value & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")


def ensure_parent(path: str) -> None:
    """Makes the directory PATH is to be written in."""
    parent = os.path.dirname(path)
    if parent == "":
        return
    try:
        os.makedirs(parent, exist_ok=True)
    except OSError:
        fail("cannot create output directory for " + path)
