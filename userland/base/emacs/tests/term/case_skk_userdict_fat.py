# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# FAT16 fallback: when ~/.SKK-JISYO cannot be written the personal
# dictionary is saved as ~/SKKUSER.DIC.  Simulated by making the
# primary path an (unwritable-as-file) directory.
import pathlib, subprocess, os, shutil
home = pathlib.Path('/tmp/remacs-skk-fat-home')
home.mkdir(exist_ok=True)
subprocess.run(['sh','-c',
    'printf ";; okuri-nasi\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')],
    check=True)
blocker = home / '.SKK-JISYO'
if blocker.is_file():
    os.unlink(blocker)
blocker.mkdir(exist_ok=True)
try:
    os.unlink(home / 'SKKUSER.DIC')
except FileNotFoundError:
    pass
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),
    (0.4, b"Inu"),
    (0.5, b" "),
    (0.5, "犬".encode() + b"\r"),
    (0.4, b"\x0a"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["犬"]
FILES = [(str(home / 'SKKUSER.DIC'), "いぬ /犬/\n")]
