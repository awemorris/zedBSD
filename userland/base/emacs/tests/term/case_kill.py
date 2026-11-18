# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Kill and yank: mark a word, C-w it, yank it back elsewhere.
KEYS = [
    (0.4, b"hello world"),
    (0.15, b"\x01"),          # C-a
    (0.15, b"\x00"),          # C-SPC (set mark)
    (0.15, b"\x06\x06\x06\x06\x06"),  # C-f x5
    (0.15, b"\x17"),          # C-w kill-region -> "hello"
    (0.15, b"\x05"),          # C-e
    (0.15, b"\x19"),          # C-y yank
    (0.2, b"\x18\x03"),       # C-x C-c
]
EXPECT = [" worldhello"]
