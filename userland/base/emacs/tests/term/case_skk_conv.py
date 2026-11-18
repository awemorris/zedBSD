# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK conversion: Kanji SPC -> candidate from SKK-JISYO.L, C-j commits.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-conv-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-nasi\\nかんじ /漢字/幹事/感じ/\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.4, b"\x18\x0a"),            # C-x C-j
    (0.4, b"Kanji"),               # ▽かんじ
    (0.5, b" "),                   # SPC -> ▼漢字 (dictionary load here)
    (1.5, b"\x0a"),                # C-j commit
    (0.3, b"l"),                   # ascii mode
    (0.2, b"OK"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["漢字OK"]
EXPECT_NOT = ["▼", "▽"]
