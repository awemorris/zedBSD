# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Registering a word saves the personal dictionary to ~/.SKK-JISYO.
import pathlib, subprocess, os
home = pathlib.Path('/tmp/remacs-skk-userdict-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-nasi\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
for legacy in ('.SKK-JISYO', 'SKKUSER.DIC', '.skk-jisyo'):
    try:
        os.unlink(home / legacy)
    except FileNotFoundError:
        pass
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j
    (0.4, b"Neko"),              # ▽ねこ
    (0.5, b" "),                 # no candidate -> register prompt
    (0.5, "猫".encode() + b"\r"),
    (0.4, b"\x0a"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["猫"]
FILES = [(str(home / ".SKK-JISYO"), "ねこ /猫/\n")]
