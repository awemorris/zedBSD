# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x 0 deletes the selected window; C-x 1 keeps only the selected.
KEYS = [
    (0.4, b"main text"),
    (0.2, b"\x182"),               # split
    (0.2, b"\x18o"),               # to bottom
    (0.3, b"\x18b"),               # bottom shows buffer "temp"
    (0.3, b"temp\r"),
    (0.3, b"temp text"),
    (0.2, b"\x180"),               # delete bottom window
    (0.3, b"\x18\x03"),
]
EXPECT = ["main text"]
EXPECT_NOT = ["temp text", "remacs: temp"]
