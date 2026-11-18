# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# ~/.remacs (Noct) loads at startup: it can define commands and rebind
# keys through the same infrastructure the editor uses.
import pathlib
home = pathlib.Path('/tmp/remacs-init-home')
home.mkdir(exist_ok=True)
(home / '.remacs.noct').write_text('''
func remacsInit(ed) {
    defineCommand("init-stamp", (_) => {
        Editor.insert("INIT-OK");
    });
    globalSetKey("C-t", "init-stamp");
    ed.echoMessage = "init loaded";
}
''')
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x14"),          # C-t -> rebound by the init file
    (0.3, b"\x18\x03"),
]
EXPECT = ["INIT-OK", "init loaded"]
