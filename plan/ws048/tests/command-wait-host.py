#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Compile and exercise the unchanged production command waiter with host models."""

from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[3]
    output = root / "build/ws048-command-wait-host"
    output.mkdir(parents=True, exist_ok=True)
    source = (root / "src/drivers/pci/pci-xhci.c").read_text()
    start = source.index("static int\ncommand_ex(")
    end = source.index("/* Runs one controller command. */", start)
    (output / "command-wait-under-test.inc").write_text(source[start:end])
    binary = output / "command-wait"
    subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DHAL_BOARD_RPI4", "-I" + str(output),
            str(root / "plan/ws048/tests/command-wait-host.c"),
            "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
