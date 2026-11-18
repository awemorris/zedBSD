# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
import pathlib, os
home = pathlib.Path('/tmp/remacs-skk-real-home')
home.mkdir(exist_ok=True)
_dst = home / 'SKK-JISYO.L'
if not _dst.exists():
    os.symlink('/home/awe/SKK-JISYO.L', _dst)
ENV = {'HOME': str(home)}
# okuri-ari against the real ~/SKK-JISYO.L (via HOME).
KEYS = [
    (0.5, b"\x18\x0a"),
    (0.4, b"Ka"),
    (0.4, b"K"),
    (0.6, b"e"),                 # dict load here
    (1.5, b"ru"),
    (0.4, b"\x0a"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["書ける"]
EXPECT_NOT = ["▽", "▼"]
