# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# skk-isearch: SKK-on isearch composes romaji into kana to find
# Japanese text; point lands after the match.
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j (SKK on, hiragana)
    (0.4, b"nihongo"),           # にほんご
    (0.3, b"l"),                 # ascii mode
    (0.3, b"ABC"),               # marker text
    (0.3, b"\x0a"),              # C-j: back to hiragana
    (0.3, b"\x1b<"),             # M-<
    (0.3, b"\x13"),              # C-s
    (0.4, b"nihongo"),           # compose にほんご as the search
    (0.4, b"\r"),                # RET exit at the match end
    (0.3, b"\x18\x0a"),          # C-x C-j: SKK off
    (0.3, b"Z"),                 # marker at point (after にほんご)
    (0.4, b"\x18\x03"),
]
EXPECT = ["にほんごZABC"]
