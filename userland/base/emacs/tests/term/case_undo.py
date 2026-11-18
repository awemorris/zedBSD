# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Typing amalgamates into one undo group; C-_ removes it entirely.
KEYS = [
    (0.4, b"abc def"),
    (0.2, b"\x1f"),        # C-_ undo
    (0.2, b"xyz"),
    (0.2, b"\x18\x03"),    # C-x C-c
]
EXPECT = ["xyz"]
EXPECT_NOT = ["abc"]
