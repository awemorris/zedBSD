# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK bundled dictionary: with no ~/SKK-JISYO.L present, remacs must fall
# back to the clean-room UTF-8 dictionary shipped in dict/SKK-JISYO.remacs.
# Verifies both an okuri-nasi lookup (かくにん -> 確認) and okuri-ari
# conversion (KaKeru -> 書ける) resolve from the bundled dictionary.
import pathlib, os
home = pathlib.Path('/tmp/remacs-skk-bundled-home')
home.mkdir(exist_ok=True)
# Ensure no user SKK-JISYO.L shadows the bundled dictionary.
try:
    (home / 'SKK-JISYO.L').unlink()
except FileNotFoundError:
    pass
# The test harness runs with cwd = tests/, so the bundled dictionary is
# at ../dict/SKK-JISYO.remacs; pass it as an absolute path.
_dict = os.path.abspath(os.path.join('..', 'dict', 'SKK-JISYO.remacs'))
ENV = {'HOME': str(home), 'REMACS_SKK_DICT': _dict}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j  (enter SKK)
    (0.4, b"Kakuninn"),          # ▽かくにん  (nn -> ん)
    (0.5, b" "),                 # SPC -> ▼確認
    (0.4, b"\x0a"),              # C-j commit -> 確認
    (0.3, b"\n"),                # newline
    (0.4, b"Jissou"),            # ▽じっそう  (diary top word)
    (0.5, b" "),                 # SPC -> ▼実装
    (0.4, b"\x0a"),              # commit -> 実装
    (0.3, b"\n"),
    (0.4, b"Ka"),                # ▽か
    (0.4, b"K"),                 # start okurigana (k)
    (0.5, b"e"),                 # convert key かk -> ▼書け
    (0.4, b"ru"),                # 書ける
    (0.4, b"\x0a"),              # commit
    (0.3, b"\n"),
    (0.4, b"Muzuka"),            # ▽むずか  (adjective, tests しい okurigana)
    (0.4, b"S"),                 # start okurigana s -> lookup むずかs -> ▼難
    (0.5, b"hii"),               # しい -> 難しい
    (0.4, b"\x0a"),              # commit
    (0.3, b"\x18\x03"),          # C-x C-c
]
EXPECT = ["確認", "実装", "書ける", "難しい"]
EXPECT_NOT = ["▽", "▼", "かk", "かくにん", "難い"]
