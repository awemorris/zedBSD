#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Measure warm REMacs cursor-command latency through a pseudo-terminal.

The timer starts immediately after one C-b/C-f command is written and stops
when REMacs emits the first terminal update. Startup, source registration and
the initial full redisplay are therefore excluded.
"""

import argparse
import os
import pty
import select
import statistics
import struct
import subprocess
import tempfile
import termios
import time


def drain_until_quiet(fd, quiet=0.10, timeout=10.0):
    deadline = time.monotonic() + timeout
    quiet_deadline = time.monotonic() + quiet
    while time.monotonic() < deadline:
        wait = max(0.0, min(quiet_deadline, deadline) - time.monotonic())
        readable, _, _ = select.select([fd], [], [], wait)
        if not readable:
            if time.monotonic() >= quiet_deadline:
                return
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        quiet_deadline = time.monotonic() + quiet
    raise TimeoutError("REMacs terminal output did not become quiet")


def command_latency(fd, key, timeout=2.0):
    started = time.perf_counter_ns()
    os.write(fd, key)
    readable, _, _ = select.select([fd], [], [], timeout)
    if not readable:
        raise TimeoutError("no redisplay output after cursor command")
    data = os.read(fd, 65536)
    first = time.perf_counter_ns()
    completed = first
    byte_count = len(data)
    while True:
        readable, _, _ = select.select([fd], [], [], 0.002)
        if not readable:
            break
        data = os.read(fd, 65536)
        if not data:
            break
        byte_count += len(data)
        completed = time.perf_counter_ns()
    return ((first - started) / 1_000_000.0,
            (completed - started) / 1_000_000.0,
            byte_count)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("remacs")
    parser.add_argument("--lines", type=int, default=50000)
    parser.add_argument("--samples", type=int, default=50)
    parser.add_argument("--object-model", choices=("0", "1"), default="0")
    parser.add_argument("--optimize-level", choices=("0", "2"), default="2")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="remacs-cursor-") as temp:
        path = os.path.join(temp, "large.txt")
        with open(path, "w", encoding="utf-8") as stream:
            for index in range(args.lines):
                ending = "\n" if index + 1 < args.lines else ""
                stream.write(f"line {index:06d} " + "x" * 56 + ending)

        master, slave = pty.openpty()
        import fcntl

        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 80, 0, 0))
        env = dict(os.environ)
        env["HOME"] = temp
        env["REMACS_OBJECT_MODEL"] = args.object_model
        env["REMACS_OPT_LEVEL"] = args.optimize_level
        process = subprocess.Popen([args.remacs, path], stdin=slave,
                                   stdout=slave, stderr=slave,
                                   close_fds=True, env=env)
        os.close(slave)
        try:
            drain_until_quiet(master)
            os.write(master, b"\x1b>")       # M->, end-of-buffer
            drain_until_quiet(master)

            # Prime both directions and all JIT paths before measuring.
            command_latency(master, b"\x02")  # C-b
            command_latency(master, b"\x06")  # C-f

            first_samples = []
            complete_samples = []
            byte_samples = []
            for _ in range(args.samples):
                first, complete, byte_count = command_latency(master, b"\x02")
                first_samples.append(first)
                complete_samples.append(complete)
                byte_samples.append(byte_count)
                command_latency(master, b"\x06")

            ordered = sorted(first_samples)
            p95 = ordered[min(len(ordered) - 1,
                              int(len(ordered) * 0.95))]
            print("lines,model,samples,first_min_ms,first_median_ms,"
                  "first_p95_ms,complete_median_ms,median_bytes")
            print(f"{args.lines},m{args.object_model},{args.samples},"
                  f"{min(first_samples):.3f},"
                  f"{statistics.median(first_samples):.3f},{p95:.3f},"
                  f"{statistics.median(complete_samples):.3f},"
                  f"{statistics.median(byte_samples):.0f}")
        finally:
            if process.poll() is None:
                os.write(master, b"\x18\x03")  # C-x C-c
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            os.close(master)


if __name__ == "__main__":
    main()
