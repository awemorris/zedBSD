# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
# The whole editor as one 8.3 bytecode file on an unmodified noct:
#     noct remacs.nap file
import subprocess, pathlib
root = pathlib.Path('/home/awe/NoctLang/apps/remacs')
subprocess.run(['sh', 'tools/build-nap.sh', 'build-debug/noctlang/noct',
                'build-debug/generated', 'build-nap'],
               cwd=root, check=True, capture_output=True)
pathlib.Path('/tmp/remacs-nap-test.txt').write_text('from bytecode\n')
ARGS = [str(root / 'build-nap' / 'remacs.nap'), '/tmp/remacs-nap-test.txt']
KEYS = [
    (0.8, b"edited "),
    (0.3, b"\x18\x03"),
]
EXPECT = ["edited from bytecode", "remacs: remacs-nap-test.txt"]
