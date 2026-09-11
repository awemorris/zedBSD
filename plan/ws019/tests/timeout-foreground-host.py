#!/usr/bin/env python3
"""Exercise the actual timeout utility with a controlling PTY and terminal input."""
import errno
import os
import select
import signal
import sys
import time


def run(options, expected):
    pid, terminal = os.forkpty()
    if pid == 0:
        os.execv(sys.argv[1], [sys.argv[1], *options, "-k", "1", "2", "/bin/sh", "-c",
            'printf READY; read answer; test "$answer" = ACCEPT'])
    output = b""
    sent = False
    finished = False
    try:
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if select.select([terminal], [], [], 0.05)[0]:
                try:
                    output += os.read(terminal, 4096)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
            if b"READY" in output and not sent:
                os.write(terminal, b"ACCEPT\n")
                sent = True
            waited, status = os.waitpid(pid, os.WNOHANG)
            if waited:
                finished = True
                assert sent and os.waitstatus_to_exitcode(status) == expected, (options, status, output)
                return
        raise AssertionError(("unbounded timeout child", options, output))
    finally:
        os.close(terminal)
        if not finished:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)


run([], 124)
run(["-f"], 0)
run(["--foreground"], 0)
print("timeout foreground PASS: default child cannot read PTY; both foreground forms can")
