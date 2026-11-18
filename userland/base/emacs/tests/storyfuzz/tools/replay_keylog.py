#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Replay a storyfuzz keylog against a live remacs and hold the pty.

Usage: python3 replay_keylog.py <failures/seed-N/keylog.txt> [--no-jit]

Sends every step, then keeps the pty open and reports the process
state every 5 seconds (pid, alive, %CPU) so a debugger can attach.
"""

import ast
import fcntl
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import termios
import time

REMACS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', '..', '..', 'build-debug', 'remacs')


def main():
    import re
    keylog = sys.argv[1]
    steps = []
    for ln in open(keylog).read().splitlines():
        if not ln or ln.startswith(('seed', 'replay')):
            continue
        m = re.match(r"^(.*?)\s+(b['\"].*)$", ln)
        if m is None:
            continue
        steps.append((m.group(1), ast.literal_eval(m.group(2))))

    home = '/tmp/remacs-replay'
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home)
    dic = ';; okuri-nasi\nかんじ /漢字/感じ/\nか /可/課/\nねこ /猫/\nいぬ /犬/\n'
    open(os.path.join(home, 'SKK-JISYO.L'), 'wb').write(dic.encode('euc_jp'))
    open(os.path.join(home, 'sample.txt'), 'w').write('sample\n')

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    env = dict(os.environ)
    env['HOME'] = home
    if '--no-jit' in sys.argv:
        env['REMACS_NO_JIT'] = '1'
    proc = subprocess.Popen([os.path.abspath(REMACS)], stdin=slave,
                            stdout=slave, stderr=subprocess.DEVNULL,
                            close_fds=True, env=env, cwd=home)
    os.close(slave)
    print('spawned pid %d' % proc.pid, flush=True)

    def pump(sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.03)
            if master in r:
                try:
                    os.read(master, 8192)
                except OSError:
                    return

    pump(0.6)
    for label, data in steps:
        os.write(master, data)
        pump(0.04)
    print('sent all; pid %d' % proc.pid, flush=True)
    for i in range(120):
        pump(5.0)
        alive = proc.poll()
        cpu = subprocess.run(['ps', '-o', 'pcpu=', '-p', str(proc.pid)],
                             capture_output=True, text=True).stdout.strip()
        print('t+%3ds alive=%s cpu=%s' % ((i + 1) * 5, alive, cpu),
              flush=True)
        if alive is not None:
            break


if __name__ == '__main__':
    main()
