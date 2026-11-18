# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# skkSetRomKanaRule via ~/.remacs: "!" inputs a fullwidth ！.
import pathlib, subprocess
home = pathlib.Path('/tmp/remacs-skk-rule-home')
home.mkdir(exist_ok=True)
(home / '.remacs.noct').write_text('''
func remacsInit(ed) {
    Editor.skkSetRomKanaRule({ "!": "！", "@": "＠" });
}
''')
subprocess.run(['sh','-c','printf ";; okuri-nasi\\n" | iconv -f utf-8 -t euc-jp > %s' % (home/'SKK-JISYO.L')], check=True)
ENV = {'HOME': str(home)}
KEYS = [
    (0.5, b"\x18\x0a"),          # C-x C-j
    (0.4, b"a"),                 # あ
    (0.3, b"!"),                 # -> ！
    (0.3, b"@"),                 # -> ＠
    (0.4, b"\x18\x03"),
]
EXPECT = ["あ！＠"]
