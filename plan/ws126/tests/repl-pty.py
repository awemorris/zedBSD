#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""P6 of WS126: the interactive REPL on a terminal.

Run on the guest as `python3 repl-pty.py`.  It starts `python3 -q` on a new
pseudo-terminal, types `6*7`, expects `42`, types `exit()` and expects the
interpreter to end with status 0.  It says which REPL answered: the new one
(pyrepl, which draws with escape sequences) or the basic one.  The last line
is `repl-pty: PASS` or `repl-pty: FAIL`.
"""

import os
import select
import subprocess
import sys
import time


def read_until(descriptor, wanted, seconds):
    """Reads the terminal until a text shows, or the time runs out; returns all that was read."""
    seen = b""
    deadline = time.monotonic() + seconds
    while wanted not in seen:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        readable, _, _ = select.select([descriptor], [], [], left)
        if not readable:
            break
        try:
            chunk = os.read(descriptor, 4096)
        except OSError:
            break
        if not chunk:
            break
        seen += chunk
    return seen


def main():
    """Drives one REPL session and prints the verdict."""
    controller, terminal = os.openpty()
    environment = dict(os.environ, TERM=os.environ.get("TERM", "xterm-256color"))
    process = subprocess.Popen([sys.executable, "-q"], stdin=terminal, stdout=terminal, stderr=terminal,
                               env=environment, start_new_session=True)
    os.close(terminal)
    failed = False

    prompt = read_until(controller, b">>> ", 20)
    if b">>> " not in prompt:
        print(f"FAIL: no prompt: {prompt[-200:]!r}", flush=True)
        failed = True
    os.write(controller, b"6*7\r")
    answer = read_until(controller, b"42", 20)
    if b"42" not in answer:
        print(f"FAIL: no 42: {answer[-200:]!r}", flush=True)
        failed = True
    kind = "pyrepl" if b"\x1b[" in prompt + answer else "basic"
    print(f"repl: {kind}", flush=True)
    os.write(controller, b"exit()\r")
    try:
        status = process.wait(20)
    except subprocess.TimeoutExpired:
        process.kill()
        status = None
    read_until(controller, b"\x00", 1)
    os.close(controller)
    if status != 0:
        print(f"FAIL: the interpreter ended with {status}", flush=True)
        failed = True
    print("repl-pty: FAIL" if failed else "repl-pty: PASS", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
