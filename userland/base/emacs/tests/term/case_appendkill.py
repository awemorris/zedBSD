# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-M-w makes the next kill append instead of pushing a new entry.
KEYS = [
    (0.4, b"one two"),
    (0.2, b"\x1b<"),            # M-<
    (0.2, b"\x1b\x64"),         # M-d kill "one" -> ring: "one"
    (0.2, b"\x1b\x17"),         # C-M-w (ESC C-w) append-next-kill
    (0.2, b"\x1b\x64"),         # M-d kill " two" -> appends -> "one two"
    (0.2, b"\x19"),             # C-y yank the joined kill
    (0.2, b"\x18\x03"),
]
EXPECT = ["one two"]
