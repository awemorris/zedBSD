# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# Meta bindings from ~/.remacs.el: the "\M-2" string escape (the Lisp
# reader must preserve \M- for the key parser) and the [(meta 3)]
# vector form (a bare small int means the digit key, not codepoint 3).
import pathlib
home = pathlib.Path('/tmp/remacs-meta-key')
home.mkdir(exist_ok=True)
(home / '.remacs.el').write_text(r'''
(global-set-key "\M-2" 'split-window-vertically)
(global-set-key [(meta 3)] 'kill-word)
''')
ENV = {'HOME': str(home)}
KEYS = [
    (0.6, b"KILLME keepme"),
    (0.2, b"\x1b<"),        # M-< beginning-of-buffer
    (0.3, b"\x1b3"),        # M-3 -> kill-word: eats KILLME
    (0.3, b"\x1b2"),        # M-2 -> split-window-vertically
    (0.2, b"\x18o"),        # C-x o to the new window
    (0.3, b"\x18b"),        # C-x b two
    (0.3, b"two\r"),
    (0.3, b"SECOND"),
    (0.3, b"\x18\x03"),
]
EXPECT = ["keepme", "SECOND", "remacs: two"]
EXPECT_NOT = ["KILLME"]
