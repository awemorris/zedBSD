# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# DDSKK-compatible: (setq skk-rom-kana-rule-list '((in nil out) ...))
# in ~/.remacs.el merges string rules into the SKK romaji table.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-rule-el')
home.mkdir(exist_ok=True)
(home / '.remacs.el').write_text('''
(setq skk-rom-kana-rule-list
      '(("1" nil "#1") ("!" nil "!*")
        ("2" nil "#2") ("@" nil "!w")))
''')
subprocess.run(['sh', '-c',
    'printf ";; okuri-nasi\\n" | iconv -f utf-8 -t euc-jp > %s'
    % (home / 'SKK-JISYO.L')], check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j: kana mode
    (0.4, b"a"),                 # あ
    (0.3, b"1"),                 # -> #1
    (0.3, b"!"),                 # -> !*
    (0.3, b"@"),                 # -> !w
    (0.4, b"\x18\x03"),
]
EXPECT = ["あ#1!*!w"]
