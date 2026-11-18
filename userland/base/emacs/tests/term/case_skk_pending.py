# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# DDSKK-style pending romaji: the in-progress alphabet shows inline
# and is replaced by kana. Type "ky" (shows), then "a" -> きゃ.
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j
    (0.4, b"ky"),                # pending "ky" visible
    (0.4, b"a"),                 # -> きゃ
    (0.3, b"n"),                 # pending "n" visible
    (0.4, b"\x18\x03"),
]
EXPECT = ["きゃn"]
