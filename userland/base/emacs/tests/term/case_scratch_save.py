# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x C-s on the file-less *scratch* buffer asks for a file name and
# writes there (previously: the "No file name" error).
import pathlib, os
home = pathlib.Path('/tmp/remacs-scratch-save-home')
home.mkdir(exist_ok=True)
out = home / 'saved.txt'
try:
    os.unlink(out)
except FileNotFoundError:
    pass
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"hello scratch"),
    (0.4, b"\x18\x13"),                          # C-x C-s -> prompt
    (0.5, str(out).encode() + b"\r"),            # type the file name
    (0.5, b"\x18\x03"),
]
EXPECT = ["Wrote"]
EXPECT_NOT = ["No file name"]
FILES = [(str(out), "hello scratch")]
