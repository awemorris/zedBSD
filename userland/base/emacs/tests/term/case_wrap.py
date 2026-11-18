# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# A long line wraps with a backslash continuation instead of being
# truncated; text past column 80 is still on screen.
KEYS = [
    (0.5, b"A" * 100 + b"TAIL"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["TAIL", "\\"]
