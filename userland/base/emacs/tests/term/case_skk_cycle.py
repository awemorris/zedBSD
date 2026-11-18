# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SPC cycles candidates; x steps back; commit by typing on.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-conv-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-nasi\\nかんじ /漢字/幹事/感じ/\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.4, b"\x18\x0a"),
    (0.4, b"Kanji"),
    (0.5, b" "),                   # ▼漢字
    (1.5, b" "),                   # ▼幹事
    (0.3, b"x"),                   # back to ▼漢字
    (0.3, b"\x0a"),                # commit
    (0.3, b"\x18\x03"),
]
EXPECT = ["漢字"]
EXPECT_NOT = ["幹事"]
