# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# SKK candidates past the first (BUG-302): each is cut out of the
# dictionary line by itself (no "漢字/幹事"), and the fuller
# SKK-JISYO.X adds its candidates after the compact dictionary's,
# without repeating them: かんじ is 感じ/漢字/幹事 (compact) then 完治
# (X only), へんかん is 変換 then 返還 (X only).
import os
# The harness runs with cwd = tests/.
_dict = os.path.abspath(os.path.join('..', 'dict', 'SKK-JISYO.remacs'))
_dictx = os.path.abspath(os.path.join('..', 'dict', 'SKK-JISYO.X'))
ENV = {'HOME': '/tmp/remacs-skk-lone-n-home', 'REMACS_SKK_DICT': _dict,
       'REMACS_SKK_DICT_X': _dictx}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j  (enter SKK)
    (0.4, b"Kanji"),             # ▽かんじ
    (0.8, b" "),                 # ▼感じ (dictionaries load here)
    (0.4, b" "),                 # ▼漢字
    (0.4, b"\x0a"),              # commit
    (0.3, b"\n"),
    (0.4, b"Kanji"),
    (0.5, b" "),                 # ▼感じ
    (0.4, b" "),                 # ▼漢字
    (0.4, b" "),                 # ▼幹事
    (0.4, b" "),                 # ▼完治 (X)
    (0.4, b"\x0a"),
    (0.3, b"\n"),
    (0.4, b"Henkan"),
    (0.5, b" "),                 # ▼変換
    (0.4, b" "),                 # ▼返還 (X)
    (0.4, b"\x0a"),
    (0.3, b"\x18\x03"),          # C-x C-c
]
EXPECT = ["漢字\n完治\n返還"]
EXPECT_NOT = ["/", "▼", "▽"]
