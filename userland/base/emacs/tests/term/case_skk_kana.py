# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK: romaji compose to hiragana; q switches to katakana; l to
# ASCII; C-j back to kana.
KEYS = [
    (0.4, b"\x18\x0a"),            # C-x C-j (LF = C-j)
    (0.4, b"konnnichiha"),         # こんにちは
    (0.2, b"q"),                   # katakana
    (0.3, b"kana"),                # カナ
    (0.2, b"q"),                   # back to hiragana
    (0.2, b"l"),                   # ascii
    (0.3, b"abc"),
    (0.2, b"\x0a"),                # C-j -> kana
    (0.3, b"."),                   # 。
    (0.3, b"\x18\x03"),
]
EXPECT = ["こんにちはカナabc。", "--かな"]
