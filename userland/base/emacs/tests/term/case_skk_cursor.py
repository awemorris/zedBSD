# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# During SKK conversion the composition marks are double width on CJK
# terminals: the cursor must sit just AFTER the last kanji, not on its
# right half.  ▼日本語 = 2+2+2+2 = 8 columns -> cursor col 8 (0-based).
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-cursor-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-nasi\\nにほんご /日本語/\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),      # C-x C-j
    (0.4, b"Nihongo"),       # ▽にほんご
    (0.6, b" "),             # ▼日本語  <- cursor checked here
    (0.4, b"\x0a"),          # commit
    (0.3, b"\x18\x03"),
]
CURSOR_AFTER_KEY = 2
EXPECT_CURSOR = (0, 8)
EXPECT = ["日本語"]
EXPECT_NOT = ["▼"]
