# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# TAB completion in the minibuffer: file names complete to the common
# prefix, then a unique continuation completes fully.
import pathlib
d = pathlib.Path('/tmp/remacs-comp-test')
d.mkdir(exist_ok=True)
(d / 'notes.txt').write_text('completed file\n')
(d / 'notable.md').write_text('x\n')
KEYS = [
    (0.4, b"\x18\x06"),                       # C-x C-f
    (0.3, b"/tmp/remacs-comp-test/n\t"),      # TAB -> "not"
    (0.3, b"e\t"),                            # "note" + TAB -> notes.txt
    (0.3, b"\r"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["completed file", "remacs: notes.txt"]
