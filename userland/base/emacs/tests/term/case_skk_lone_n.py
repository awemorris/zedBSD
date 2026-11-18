# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK lone "n" (BUG-302): a reading that ends in a single n is ん when
# SPC, C-j, q or a text key finishes it, as in DDSKK. Henkan SPC looks
# up へんかん (not へんか), in katakana mode the n is ン, and after a
# cancelled (登録) no romaji is left over. Ten SPC finds 天, which only
# the fuller SKK-JISYO.X has, so the X dictionary is read after the
# compact one.
import pathlib, os
home = pathlib.Path('/tmp/remacs-skk-lone-n-home')
home.mkdir(exist_ok=True)
# Nothing writes a dictionary into this HOME (the (登録) is cancelled).
# The harness runs with cwd = tests/.
_dict = os.path.abspath(os.path.join('..', 'dict', 'SKK-JISYO.remacs'))
_dictx = os.path.abspath(os.path.join('..', 'dict', 'SKK-JISYO.X'))
ENV = {'HOME': str(home), 'REMACS_SKK_DICT': _dict,
       'REMACS_SKK_DICT_X': _dictx}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j  (enter SKK)
    (0.4, b"Nihon"),             # ▽にほn
    (0.8, b" "),                 # SPC -> ▼日本 (dictionaries load here)
    (0.4, b"\x0a"),              # C-j commit
    (0.3, b"\n"),
    (0.4, b"Henkan"),            # ▽へんかn
    (0.5, b" "),                 # SPC -> ▼変換
    (0.4, b"\x0a"),
    (0.3, b"\n"),
    (0.4, b"Kantan"),            # ▽かんたn
    (0.5, b" "),                 # SPC -> ▼簡単
    (0.4, b"\x0a"),
    (0.3, b"\n"),
    (0.4, b"Ten"),               # ▽てn
    (0.5, b" "),                 # SPC -> ▼天 (SKK-JISYO.X only)
    (0.4, b"\x0a"),
    (0.3, b"\n"),
    (0.4, b"q"),                 # katakana
    (0.4, b"hon"),               # ホn
    (0.4, b"q"),                 # back to hiragana: ホン
    (0.3, b"\n"),
    (0.4, b"q"),                 # katakana
    (0.4, b"Pan"),               # ▽パn
    (0.4, b"\x0a"),              # C-j commit -> パン
    (0.4, b"q"),                 # back to hiragana
    (0.3, b"\n"),
    (0.4, b"hon"),               # ほn
    (0.4, b" "),                 # SPC -> ほん + space
    (0.4, b"desu"),              # です
    (0.3, b"\n"),
    (0.4, b"Zun"),               # ▽ずn (in neither dictionary)
    (0.5, b" "),                 # SPC -> ずん (登録):
    (0.5, b"\x07"),              # C-g -> back to ▽ずん
    (0.4, b"a"),                 # ▽ずんあ (no leftover n to make な)
    (0.4, b"\x0a"),              # C-j commit
    (0.3, b"\x18\x03"),          # C-x C-c
]
EXPECT = ["日本", "変換", "簡単", "天", "ホン", "パン", "ほん です", "ずんあ"]
EXPECT_NOT = ["▽", "▼", "にほ\n", "へんか\n", "ずな"]
