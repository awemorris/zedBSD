# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x 3: side-by-side windows with a divider column.
KEYS = [
    (0.4, b"leftside"),
    (0.2, b"\x183"),               # C-x 3
    (0.2, b"\x18o"),               # C-x o
    (0.3, b"\x18b"),               # C-x b
    (0.3, b"right\r"),
    (0.3, b"rightside"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["leftside", "rightside", "remacs: right"]
