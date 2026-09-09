#!/usr/bin/env python3
"""Exercise real Noct terminal confirmation and terminal-mode restoration."""
import os
from pathlib import Path
import pty
import select
import subprocess
import termios
import time

ROOT = Path(__file__).resolve().parents[3]
NOCT = ROOT / "build/NoctLang/build-static/noct"
SCRIPT = Path(__file__).with_suffix(".noct").with_name("installer-confirmation.noct")
PHRASE = b"INSTALL nvme0n1 nvme0n1p2"


def exercise(mode, keys, expected, script=SCRIPT, steps=None):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    command = [str(NOCT), mode, "--path=userland/base/zedinst", str(script)]
    child = subprocess.Popen(command, cwd=ROOT, stdin=slave, stdout=slave, stderr=slave)
    output = bytearray()
    sent = False
    pending = list(steps or [])
    deadline = time.monotonic() + 15
    try:
        while child.poll() is None and time.monotonic() < deadline:
            if select.select([master], [], [], 0.1)[0]:
                output.extend(os.read(master, 65536))
            if not sent and b"\r\n> " in output:
                os.write(master, keys)
                sent = True
            if pending and pending[0][0] in output:
                _, fragment = pending.pop(0)
                os.write(master, fragment)
        assert child.poll() is not None, (mode, keys, "timeout", output)
        while select.select([master], [], [], 0)[0]:
            output.extend(os.read(master, 65536))
        assert child.returncode == 0, output
        assert expected in output, output
        assert termios.tcgetattr(slave) == before, "terminal attributes not restored"
        assert b"\x1b[?1049l" in output, "alternate screen not restored"
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        os.close(master)
        os.close(slave)


def main():
    for mode in ("-j", "-j0"):
        for keys, expected in (
            (PHRASE + b"\r", b"CONFIRM ACCEPTED"),
            (PHRASE + b"\n", b"CONFIRM ACCEPTED"),
            (PHRASE[:-1] + b"X\x7f2\r", b"CONFIRM ACCEPTED"),
            (b"\r", b"CONFIRM CANCELLED"),
            (b"yes\r", b"CONFIRM CANCELLED"),
            (PHRASE + b"X\r", b"CONFIRM CANCELLED"),
            (b"\x1b", b"CONFIRM CANCELLED"),
            (b"\x03", b"CONFIRM CANCELLED"),
            (b"\x04", b"CONFIRM CANCELLED"),
        ):
            exercise(mode, keys, expected)
        rejected = subprocess.run(
            [str(NOCT), mode, "--path=userland/base/zedinst", str(SCRIPT)],
            cwd=ROOT, input=b"yes\n", capture_output=True, timeout=15)
        assert rejected.returncode != 0
        assert b"interactive terminal" in rejected.stdout + rejected.stderr
        exercise(mode, b"", b"FRAGMENTS PASS",
                 SCRIPT.with_name("noct-terminal-fragments.noct"), [
                     (b"READY CSI", b"\x1b["),
                     (b"CONTINUE CSI", b"A"),
                     (b"READY UTF8", b"\xe3"),
                     (b"CONTINUE UTF8", b"\x81\x82"),
                     (b"READY SS3", b"\x1bO"),
                     (b"CONTINUE SS3", b"A"),
                 ])
        for fragment in (b"", b"\x1b[", b"\xe3", b"\x1bO"):
            ended = subprocess.run(
                [str(NOCT), mode, str(SCRIPT.with_name("noct-terminal-eof.noct"))],
                cwd=ROOT, input=fragment, capture_output=True, timeout=5)
            assert ended.returncode == 0, ended.stdout + ended.stderr
            assert b"EOF PASS" in ended.stdout
    print("PASS 18 confirmations, 2 fragmented-input PTYs, restoration, 2 pipe refusals, 8 EOF cases")


if __name__ == "__main__":
    main()
