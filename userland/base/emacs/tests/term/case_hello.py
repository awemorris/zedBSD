# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Type text, move around, delete, and quit.
KEYS = [
    (0.5, b'hello world'),
    (0.2, b'\x01'),          # C-a
    (0.2, b'\x04'),          # C-d  (delete "h")
    (0.2, b'X'),             # insert X -> "Xello world"
    (0.2, b'\x05'),          # C-e
    (0.2, b'!'),             # append
    (0.2, b'\x18\x03'),      # C-x C-c quit
]
EXPECT = [
    'Xello world!',
    'remacs 0.0.1',
]
