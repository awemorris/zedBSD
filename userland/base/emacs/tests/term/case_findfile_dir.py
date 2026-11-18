# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# find-file pre-fills the default directory, abbreviated with ~.
import pathlib
home = pathlib.Path('/tmp/remacs-ffdir-home')
home.mkdir(exist_ok=True)
(home / 'hello.txt').write_text('dir default works\n')
ENV = {'HOME': str(home)}
ARGS = [str(home / 'hello.txt')]
KEYS = [
    (0.5, b"\x18\x06"),      # C-x C-f: prompt shows "Find file: ~/"
    (0.5, b"hello.txt\r"),   # relative to the buffer's directory
    (0.4, b"\x18\x03"),
]
EXPECT = ["dir default works", "~/hello.txt"]
