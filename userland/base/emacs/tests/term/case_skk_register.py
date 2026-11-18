# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Registration: an unknown reading asks in the minibuffer and saves
# to the personal dictionary under an isolated HOME.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-home')
home.mkdir(exist_ok=True)
jisyo = home / 'SKK-JISYO.L'
subprocess.run(['sh', '-c',
    'printf ";; okuri-nasi\\nてすと /試/\\n" | iconv -f utf-8 -t euc-jp > %s' % jisyo],
    check=True)
for name in ('.SKK-JISYO', 'SKKUSER.DIC', '.skk-jisyo'):
    p = home / name
    if p.exists():
        p.unlink()
ENV = {'HOME': str(home)}
KEYS = [
    (0.4, b"\x18\x0a"),                    # C-x C-j
    (0.4, b"Mikandesu"),                   # ▽みかんです (not in dict)
    (0.4, b" "),                           # SPC -> registration prompt
    (0.6, "蜜柑です\r".encode()),           # answer
    (0.3, b"\x18\x03"),
]
EXPECT = ["蜜柑です", "Registered:"]
FILES = [(str(home / '.SKK-JISYO'), "みかんです /蜜柑です/\n")]
