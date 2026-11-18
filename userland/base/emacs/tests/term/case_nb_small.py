# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# The small (embedded) bundle on an unmodified noct: basic editing and
# isearch work; a command of an omitted module (skk-mode) is reported
# in the echo area without taking the editor down.
#     noct remacs-s.nap file
import subprocess, pathlib
root = pathlib.Path(__file__).resolve().parents[2]
subprocess.run(['sh', 'tools/build-nap.sh', 'build-debug/noctlang/noct',
                'build-nap', 'small'],
               cwd=root, check=True, capture_output=True)
pathlib.Path('/tmp/remacs-nap-small-test.txt').write_text('alpha\nbeta\ngamma\n')
ARGS = [str(root / 'build-nap' / 'remacs-s.nap'), '/tmp/remacs-nap-small-test.txt']
KEYS = [
    (0.8, b"\x13beta\r"),          # C-s beta RET: isearch (small keeps it)
    (0.3, b"!"),                   # extend the match line: "beta!"
    (0.3, b"\x1bxskk-mode\r"),     # M-x skk-mode: module not in this bundle
    (0.3, b"\x18\x03"),            # C-x C-c
]
EXPECT = ["beta!", "No such command: skk-mode"]
