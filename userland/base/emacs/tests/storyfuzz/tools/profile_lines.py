#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Poor-man's Noct line profiler for a stalled remacs.

Spawns remacs (REMACS_NO_JIT=1 so the interpreter keeps env->line
fresh), feeds it a key script, then repeatedly samples env->line /
env->file_name with gdb and prints a histogram: where the VM spends
its time during a stall found by storyfuzz.

Usage: python3 profile_lines.py [seconds]
Edit drive() below to reproduce the scenario under investigation.
"""

import collections
import fcntl
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import termios
import time

REMACS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', '..', '..', 'build-debug', 'remacs')


def drive(master, pump):
    """Reproduce the stall: C-x 3, then paste 30 wrapped lines."""
    import random
    rng = random.Random(1)
    words = ['alpha', 'beta', 'gamma', 'delta', 'code', 'test']
    lines = []
    total = 0
    while total < 9000:
        l = ' '.join(rng.choice(words) for _ in range(rng.randint(1, 10)))
        lines.append(l)
        total += len(l) + 1
    os.write(master, ('\r'.join(lines)).encode())
    pump(0.5)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
    home = '/tmp/remacs-profile'
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    env = dict(os.environ)
    env['HOME'] = home
    env['REMACS_NO_JIT'] = '1'
    proc = subprocess.Popen([os.path.abspath(REMACS)], stdin=slave,
                            stdout=slave, stderr=subprocess.DEVNULL,
                            close_fds=True, env=env, cwd=home)
    os.close(slave)

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
    drive(master, pump)

    # The innermost frame is often libc, where "env" is not in scope;
    # grab the stable env pointer from a backtrace once, then sample
    # through it directly.
    g = subprocess.run(['gdb', '-p', str(proc.pid), '-batch', '-ex', 'bt'],
                       capture_output=True, text=True)
    m = re.search(r'env=(0x[0-9a-f]+)', g.stdout)
    if m is None:
        print('could not find env pointer in backtrace')
        proc.kill()
        return
    env_ptr = m.group(1)

    hist = collections.Counter()
    t0 = time.time()
    while time.time() - t0 < seconds and proc.poll() is None:
        g = subprocess.run(
            ['gdb', '-p', str(proc.pid), '-batch',
             '-ex', 'print ((struct rt_env *)%s)->line' % env_ptr,
             '-ex', 'print ((struct rt_env *)%s)->file_name' % env_ptr],
            capture_output=True, text=True)
        m2 = re.findall(r'\$\d+ = (.*)', g.stdout)
        if len(m2) >= 2:
            line = m2[0].strip()
            fn = re.search(r'"([^"\\]*)', m2[1])
            fname = os.path.basename(fn.group(1)) if fn else '?'
            hist['%s:%s' % (fname, line)] += 1
        time.sleep(0.05)

    for k, v in hist.most_common(20):
        print('%4d  %s' % (v, k), flush=True)
    proc.kill()
    proc.wait()


if __name__ == '__main__':
    main()
