# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-DEL kills the word before point; M-y unused here.
KEYS = [
    (0.4, b"one two"),
    (0.2, b"\x1b\x7f"),    # M-DEL backward-kill-word
    (0.2, b"\x18\x03"),
]
EXPECT = ["one"]
EXPECT_NOT = ["two"]
