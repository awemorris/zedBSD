# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# C-x C-f prompts for a path in the echo area and visits the file.
import pathlib
pathlib.Path('/tmp/remacs-findfile.txt').write_text('found me\n')
KEYS = [
    (0.4, b"scratch text"),
    (0.2, b"\x18\x06"),                    # C-x C-f
    (0.3, b"/tmp/remacs-findfile.txt\r"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["found me", "remacs: remacs-findfile.txt"]
EXPECT_NOT = ["scratch text"]
