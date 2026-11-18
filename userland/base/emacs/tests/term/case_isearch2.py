# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Failing search message, then wrap with a second C-s.
KEYS = [
    (0.4, b"one two\rthree two one"),
    (0.2, b"\x13"),           # C-s at eob: forward has nothing
    (0.2, b"two"),            # fails
    (0.2, b"\x13"),           # wrap
    (0.2, b"\r"),
    (0.2, b"W"),
    (0.2, b"\x18\x03"),
]
EXPECT = ["one twoW"]
