# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-x command-name completion.
KEYS = [
    (0.4, b"abc"),
    (0.2, b"\x1bx"),                          # M-x
    (0.3, b"beginning-of-l\t"),               # completes to beginning-of-line
    (0.3, b"\r"),
    (0.2, b"X"),
    (0.2, b"\x18\x03"),
]
EXPECT = ["Xabc"]
