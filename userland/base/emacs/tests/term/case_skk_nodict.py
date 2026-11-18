# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK with no dictionary (BUG-302): the conversion says so in the echo
# area ("SKK: no dictionary (...)") and keeps the reading, instead of
# going straight to the (登録) prompt as for a word the dictionary
# lacks.
import pathlib
home = pathlib.Path('/tmp/remacs-skk-nodict-home')
home.mkdir(exist_ok=True)
# Nothing writes a dictionary into this HOME.
ENV = {'HOME': str(home),
       'REMACS_SKK_DICT': '/nonexistent/remacs-skk.dic',
       'REMACS_SKK_DICT_X': '/nonexistent/remacs-skk-x.dic'}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j  (enter SKK)
    (0.4, b"Kanji"),             # ▽かんじ
    (0.5, b" "),                 # SPC -> the message, still ▽かんじ
    (0.8, b"\x18\x03"),          # C-x C-c
]
EXPECT = ["SKK: no dictionary (~/SKK-JISYO.L, /nonexistent/remacs-skk.dic)",
          "▽かんじ"]
EXPECT_NOT = ["(登録)"]
