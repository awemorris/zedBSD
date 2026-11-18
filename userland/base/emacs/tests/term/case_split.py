# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x 2 splits; C-x o moves to the other window; C-x b gives it a
# different buffer; both windows stay visible.
KEYS = [
    (0.4, b"top window text"),
    (0.2, b"\x182"),               # C-x 2
    (0.2, b"\x18o"),               # C-x o
    (0.3, b"\x18b"),               # C-x b
    (0.3, b"two\r"),
    (0.3, b"bottom window text"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["top window text", "bottom window text", "remacs: two", "remacs: *scratch*"]
