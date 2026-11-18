# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-x runs a command by its Emacs name.
KEYS = [
    (0.4, b"abc"),
    (0.2, b"\x1bx"),                       # M-x
    (0.3, b"beginning-of-line\r"),
    (0.2, b"XX"),
    (0.2, b"\x18\x03"),
]
EXPECT = ["XXabc"]
