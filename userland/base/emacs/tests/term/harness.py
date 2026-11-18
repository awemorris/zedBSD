#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""pty test harness for remacs.

Runs remacs in a pseudo-terminal, feeds it a key script, and renders
the terminal output into a plain-text screen with a tiny in-process
VT100 interpreter. Tests assert against the final screen and the exit
status.

Usage: harness.py REMACS_BINARY TESTCASE...
  A testcase is a Python file defining:
    KEYS    = bytes to send (may be a list of (delay_sec, bytes))
    EXPECT  = list of strings that must appear on the final screen
    ROWS, COLS (optional, default 24x80)
    ARGS    (optional, extra argv for remacs)
"""

import os
import pty
import sys
import time
import select
import signal
import subprocess


class Screen:
    """A minimal VT100 screen: enough for remacs's own output."""

    def __init__(self, rows=24, cols=80):
        self.rows = rows
        self.cols = cols
        self.pre_clear = ''
        self.grid = [[' '] * cols for _ in range(rows)]
        self.row = 0
        self.col = 0

    def feed(self, data: str):
        i = 0
        while i < len(data):
            c = data[i]
            if c == '\x1b':
                j = self._escape(data, i)
                if j < 0:
                    break  # Incomplete escape at end of stream.
                i = j
                continue
            if c == '\r':
                self.col = 0
            elif c == '\n':
                self.row = min(self.row + 1, self.rows - 1)
            elif c == '\b':
                self.col = max(self.col - 1, 0)
            elif c >= ' ':
                if self.row < self.rows and self.col < self.cols:
                    self.grid[self.row][self.col] = c
                self.col += 1
                if self.col >= self.cols:
                    self.col = 0
                    self.row = min(self.row + 1, self.rows - 1)
            i += 1

    def _escape(self, data, i):
        # i points at ESC. Returns the index after the sequence, or -1.
        if i + 1 >= len(data):
            return -1
        if data[i + 1] != '[':
            return i + 2  # ESC x: ignore.
        j = i + 2
        params = []
        acc = ''
        while j < len(data):
            c = data[j]
            if c.isdigit():
                acc += c
                j += 1
                continue
            if c in ';?>=':
                params.append(int(acc) if acc else 0)
                acc = ''
                j += 1
                continue
            break
        if j >= len(data):
            return -1
        params.append(int(acc) if acc else 0)
        final = data[j]
        if final == 'H':
            row = params[0] if params and params[0] > 0 else 1
            col = params[1] if len(params) > 1 and params[1] > 0 else 1
            self.row = min(row - 1, self.rows - 1)
            self.col = min(col - 1, self.cols - 1)
        elif final == 'J':
            # Preserve the screen just before a full clear (the editor
            # clears on exit); tests assert against the last content.
            if any(ch != ' ' for row in self.grid for ch in row):
                self.pre_clear = self.text()
            self.grid = [[' '] * self.cols for _ in range(self.rows)]
        elif final == 'K':
            for x in range(self.col, self.cols):
                self.grid[self.row][x] = ' '
        # m (SGR), h/l (modes) and others: ignored.
        return j + 1

    def text(self):
        return '\n'.join(''.join(r).rstrip() for r in self.grid)


def run_case(remacs, case_path):
    ns = {}
    with open(case_path) as f:
        exec(compile(f.read(), case_path, 'exec'), ns)
    keys = ns['KEYS']
    expect = ns.get('EXPECT', [])
    expect_not = ns.get('EXPECT_NOT', [])
    files = ns.get('FILES', [])
    rows = ns.get('ROWS', 24)
    cols = ns.get('COLS', 80)
    args = ns.get('ARGS', [])
    # Hermetic HOME by default so a developer's ~/.emacs / ~/.remacs
    # cannot perturb the tests; a case may still set ENV explicitly.
    import os as _os
    full = dict(_os.environ)
    _hermetic = '/tmp/remacs-test-home'
    _os.makedirs(_hermetic, exist_ok=True)
    full['HOME'] = _hermetic
    env = ns.get('ENV', None)
    if env is not None:
        full.update(env)
    env = full

    if isinstance(keys, bytes):
        keys = [(0.3, keys)]

    master, slave = pty.openpty()
    import fcntl
    import struct
    import termios as tmod
    fcntl.ioctl(slave, tmod.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))

    proc = subprocess.Popen([remacs] + args, stdin=slave, stdout=slave,
                            stderr=slave, close_fds=True, env=env)
    os.close(slave)

    screen = Screen(rows, cols)
    raw = b''
    deadline = time.time() + 15

    def pump(duration):
        nonlocal raw
        end = time.time() + duration
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.05)
            if master in r:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    return False
                if not chunk:
                    return False
                raw += chunk
        return True

    # Optional mid-session cursor check: CURSOR_AFTER_KEY selects the
    # 0-based key index after which the cursor cell is recorded (fed
    # through the Screen), EXPECT_CURSOR = (row, col) 0-based.
    cursor_after = ns.get('CURSOR_AFTER_KEY', None)
    expect_cursor = ns.get('EXPECT_CURSOR', None)
    captured_cursor = None

    ok = True
    for ki, (delay, data) in enumerate(keys):
        if not pump(delay):
            break
        os.write(master, data)
        if cursor_after is not None and ki == cursor_after:
            pump(0.6)
            snap = Screen(rows, cols)
            snap.feed(raw.decode('utf-8', errors='replace'))
            captured_cursor = (snap.row, snap.col)
    pump(0.5)

    # Wait for exit.
    exited = False
    while time.time() < deadline:
        if proc.poll() is not None:
            exited = True
            break
        pump(0.1)
    if not exited:
        proc.kill()
        proc.wait()

    os.close(master)
    screen.feed(raw.decode('utf-8', errors='replace'))
    final = screen.text()
    if not final.strip() and screen.pre_clear:
        final = screen.pre_clear

    failures = []
    if not exited:
        failures.append('process did not exit')
    elif proc.returncode != 0:
        failures.append(f'exit code {proc.returncode}')
    for e in expect:
        if e not in final:
            failures.append(f'missing on screen: {e!r}')
    for e in expect_not:
        if e in final:
            failures.append(f'unexpectedly on screen: {e!r}')
    if expect_cursor is not None:
        if captured_cursor != tuple(expect_cursor):
            failures.append(
                f'cursor at {captured_cursor}, expected {tuple(expect_cursor)}')
    for path, want in files:
        try:
            with open(path) as fh:
                got = fh.read()
        except OSError as ex:
            failures.append(f'file {path}: {ex}')
            continue
        if got != want:
            failures.append(f'file {path}: got {got!r}, want {want!r}')

    if failures:
        print(f'FAIL {case_path}')
        for f_ in failures:
            print(f'  - {f_}')
        print('  --- final screen ---')
        for line in final.split('\n'):
            if line:
                print(f'  |{line}')
        return False
    print(f'PASS {case_path}')
    return True


def main():
    remacs = sys.argv[1]
    ok = True
    for case in sys.argv[2:]:
        ok = run_case(remacs, case) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
