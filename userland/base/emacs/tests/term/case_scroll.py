# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# 30 lines on a 24-row screen: M-< shows the top, two C-v go back down.
_lines = "".join(f"l{i:02d}\r" for i in range(1, 31)).encode()
KEYS = [
    (0.5, _lines),
    (0.3, b"\x1b<"),        # M-<
    (0.2, b"\x16\x16"),     # C-v C-v
    (0.2, b"\x18\x03"),
]
EXPECT = ["l30"]
EXPECT_NOT = ["l01"]
