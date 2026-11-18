# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x b switches buffers; each keeps its own contents.
KEYS = [
    (0.4, b"first buffer text"),
    (0.2, b"\x18b"),               # C-x b
    (0.3, b"second\r"),
    (0.3, b"other content"),
    (0.2, b"\x18b"),               # C-x b back
    (0.3, b"*scratch*\r"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["first buffer text", "remacs: *scratch*"]
EXPECT_NOT = ["other content"]
