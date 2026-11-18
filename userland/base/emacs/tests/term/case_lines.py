# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Multi-line editing with RET and arrow keys.
KEYS = [
    (0.5, b'first line\rsecond'),
    (0.2, b'\x1b[A'),        # Up arrow
    (0.2, b'\x01'),          # C-a
    (0.2, b'>'),
    (0.2, b'\x18\x03'),
]
EXPECT = [
    '>first line',
    'second',
]
