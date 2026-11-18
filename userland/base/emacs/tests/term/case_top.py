# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-< moves point (and the view) to the beginning of the buffer.
_lines = "".join(f"l{i:02d}\r" for i in range(1, 31)).encode()
KEYS = [
    (0.5, _lines),
    (0.3, b"\x1b<"),        # M-<
    (0.2, b"\x18\x03"),
]
EXPECT = ["l01"]
EXPECT_NOT = ["l30"]
