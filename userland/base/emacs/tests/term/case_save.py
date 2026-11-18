# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Visit a file, edit it, save with C-x C-s, quit with C-x C-c.
import tempfile, os
_tmp = os.path.join(tempfile.gettempdir(), 'remacs-save-test.txt')
with open(_tmp, 'w') as f:
    f.write('old line\n')

ARGS = [_tmp]
KEYS = [
    (0.5, b'\x05'),          # C-e (end of first line)
    (0.2, b' edited'),
    (0.2, b'\x18\x13'),      # C-x C-s save
    (0.3, b'\x18\x03'),      # C-x C-c quit
]
EXPECT = [
    'old line edited',
    'Wrote',
]
FILES = [
    (_tmp, 'old line edited\n'),
]
