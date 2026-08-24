#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# POSIX-UTILITY-TEST: write positive negative

import ctypes
import os
import pathlib
import pty
import pwd
import select
import subprocess
import sys
import tempfile


class Utmpx(ctypes.Structure):
    _fields_ = [
        ("ut_type", ctypes.c_int16),
        ("ut_reserved0", ctypes.c_int16),
        ("ut_pid", ctypes.c_int32),
        ("ut_session", ctypes.c_int32),
        ("ut_id", ctypes.c_char * 8),
        ("ut_line", ctypes.c_char * 32),
        ("ut_user", ctypes.c_char * 32),
        ("ut_host", ctypes.c_char * 64),
        ("ut_tv_sec", ctypes.c_int64),
        ("ut_tv_usec", ctypes.c_int32),
        ("ut_reserved", ctypes.c_uint32 * 8),
    ]


def record(user: str, line: str) -> bytes:
    entry = Utmpx()
    entry.ut_type = 7
    entry.ut_pid = os.getpid()
    entry.ut_line = line.encode()
    entry.ut_user = user.encode()
    return bytes(entry)


def receive(master: int) -> bytes:
    result = bytearray()
    while select.select([master], [], [], 0.2)[0]:
        try:
            result.extend(os.read(master, 4096))
        except OSError:
            break
    return bytes(result)


def main() -> int:
    repo = pathlib.Path(__file__).resolve().parent.parent
    user = pwd.getpwuid(os.getuid()).pw_name
    with tempfile.TemporaryDirectory(prefix="zedbsd-write-") as temporary:
        work = pathlib.Path(temporary)
        include = work / "include"
        include.mkdir()
        (include / "utmpx.h").symlink_to(repo / "libc/include/utmpx.h")
        binary = work / "write"
        utmp = work / "utmp"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-D_DEFAULT_SOURCE",
                f'-DWRITE_UTMP_PATH="{utmp}"',
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{include}",
                f"-I{repo}",
                str(repo / "userland/base/write/main.c"),
                str(repo / "userland/base/common/command.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )

        terminals = [pty.openpty(), pty.openpty()]
        try:
            lines = [os.ttyname(slave)[5:] for _, slave in terminals]
            for _, slave in terminals:
                os.chmod(os.ttyname(slave), 0o620)
            utmp.write_bytes(record(user, lines[1]) + record(user, lines[0]))

            selected = min(range(2), key=lambda index: lines[index])
            completed = subprocess.run(
                [str(binary), user], input=b"hello from write\n", check=False
            )
            if completed.returncode != 0:
                raise AssertionError("write failed for a writable session")
            received = receive(terminals[selected][0])
            if b"Message from " not in received or b"hello from write" not in received:
                raise AssertionError("write output did not reach selected terminal")
            other = 1 - selected
            if b"hello from write" in receive(terminals[other][0]):
                raise AssertionError("write did not select the terminal deterministically")

            os.chmod(os.ttyname(terminals[selected][1]), 0o600)
            failed = subprocess.run(
                [str(binary), user, lines[selected]],
                input=b"blocked\n",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if failed.returncode == 0:
                raise AssertionError("write ignored mesg-disabled terminal mode")

            missing = subprocess.run(
                [str(binary), "zedbsd-no-such-user"],
                input=b"blocked\n",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if missing.returncode == 0:
                raise AssertionError("write accepted an unknown recipient")
        finally:
            for master, slave in terminals:
                os.close(master)
                os.close(slave)
    print("zedBSD POSIX write host tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
