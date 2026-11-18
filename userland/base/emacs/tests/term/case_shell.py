# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# M-x shell: run a command, see its output, interactively.
KEYS = [
    (0.5, b"\x1bx"),                 # M-x
    (0.4, b"shell\r"),
    (1.2, b"echo remacs-$((6*7))\r"),
    (1.5, b"exit\r"),
    (1.0, b"\x18\x03"),
]
EXPECT = ["remacs-42", "(Shell)"]
