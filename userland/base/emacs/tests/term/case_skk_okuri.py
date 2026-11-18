# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK okuri-ari (送りあり): KaKeru -> ▽か, K starts okurigana, e triggers
# conversion of key "かk", ru completes -> 書ける. C-j commits.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-okuri-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-ari\\nかk /書/掛/\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j
    (0.4, b"Ka"),                # ▽か
    (0.4, b"K"),                 # start okurigana (consonant k)
    (0.5, b"e"),                 # け -> convert key かk -> ▼書け
    (0.4, b"ru"),                # ける -> 書ける
    (0.4, b"\x0a"),              # C-j commit
    (0.3, b"\x18\x03"),
]
EXPECT = ["書ける"]
EXPECT_NOT = ["▽", "▼", "かk"]
