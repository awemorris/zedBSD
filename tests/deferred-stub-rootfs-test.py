#!/usr/bin/env python3
"""Verify that every temporary provider stub is installed in the rootfs."""

from pathlib import Path
import os
import sys


COMMANDS = ("at", "batch", "crontab", "logger", "mailx", "talk", "lp")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: deferred-stub-rootfs-test.py ROOTFS")
    rootfs = Path(sys.argv[1])
    for command in COMMANDS:
        path = rootfs / "bin" / command
        if not path.is_file():
            raise SystemExit(f"rootfs manifest lacks /bin/{command}")
        if not os.access(path, os.X_OK):
            raise SystemExit(f"rootfs command is not executable: /bin/{command}")
    print("zedBSD deferred command rootfs manifest test: PASS")


if __name__ == "__main__":
    main()
