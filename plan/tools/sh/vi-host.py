#!/usr/bin/env python3
# ws042-p008: vi-mode (and then emacs-mode) line editing of the host build of
# sh, through a pty.
# Usage: vi-host.py SHELL   (build/ws042/host-sh from build-host-sh.sh)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os, pty, sys, time, select
SH = sys.argv[1]
cases = [
 # (keys, expected output line)
 ("echo hello\x1b0cwprintf\r", "hello$ "),
 ("echo abc def\x1bbdwiX\x1bA!\r", "abcX !"),
 ("echo one two three\x1b0wwD\r", "one"),
 ("echo aaa\x1bxxAb\r", "ab"),
 ("echo foo bar\x1b0wdwibaz\r", "bazbar"),
 ("echo hello\x1b0w~~\r", "HEllo"),
 ("echo abc\x1brZ\r", "abZ"),
 ("echo abc\x1bhhyl$p\r", "abca"),
 ("echo xyz\x1bddiecho new\r", "new"),
 ("echo 12345\x1b3X\r", "15"),
 ("echo undo\x1bxu\r", "undo"),
 ("echo first\r", "first"),
 ("\x1bk\r", "first"),
 ("echo second\r", "second"),
 ("\x1b2k\r", "first"),
 ("echo w1 w2 w3\x1b02wcwNEW\x1b\r", "w1 NEW w3"),
 ("echo abc\x1bIX\x1b\r", None),
 ("echo cc\x1bccecho replaced\r", "replaced"),
 ("echo 0caret\x1b^cwprintf '%s\\n'\x1b\r", "0caret"),
 ("echo big-word next\x1b0Wdw\r", "-word next"),
 ("echo end\x1b0ecwEND\x1b\r", None),
 ("echo xy\x1b0de\r", None),
 ("echo arrow\x1b[D\x1b[DX\r", "arow"),
 ("echo tail\x1bsL\x1b\r", "taiL"),
 ("echo p1\x1b0yw$P\r", "pecho 1"),
 ("set +o vi\r", None),
 # emacs mode again, through the same editor.
 ("echo abcdef\x02\x02\x0b\r", "abcd"),
 ("xecho hi\x01\x04\r", "hi"),
 ("xecho del\x01\x1b[3~\r", "del"),
 ("junk\x15echo cu\r", "cu"),
 ("\x10\r", "cu"),
 ("cho he\x1b[He\x1b[Fy\r", "hey"),
 ("echo left\x1b[D\x1b[DX\r", "leXft"),
]
pid, fd = pty.fork()
if pid == 0:
    os.environ['PS1'] = '$ '
    os.execv(SH, [SH, '-i'])
def readall(t=0.4):
    out = b''
    end = time.time() + t
    while time.time() < end:
        r,_,_ = select.select([fd],[],[],0.05)
        if r:
            try: out += os.read(fd, 4096)
            except OSError: break
    return out
readall()
os.write(fd, b"set -o vi\r"); readall()
passed = 0
for keys, want in cases:
    for ch in keys.encode():
        os.write(fd, bytes([ch])); time.sleep(0.005)
    out = readall().decode('latin1')
    lines = [l for l in out.replace('\r','').split('\n')]
    got = lines[1] if len(lines) > 1 else ''
    ok = want is None or got == want
    passed += ok
    print(('PASS' if ok else 'FAIL'), repr(keys), '->', repr(got), '' if ok else 'want %r' % want)
os.write(fd, b"exit\r"); readall()
print("VI-HOST %d/%d" % (passed, len(cases)))
sys.exit(0 if passed == len(cases) else 1)
