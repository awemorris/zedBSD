#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Writes the Raspberry Pi 4's config.txt for the build's configuration (ws215-p003).

The source (platform/arm64/config.txt) routes the PL011 to the serial pins and
switches the onboard Bluetooth off (enable_uart, uart_2ndstage and the
disable-bt overlay).  A build with the onboard Bluetooth driver and without the
serial console leaves the PL011 to the Bluetooth controller: those three lines,
and the comment just above uart_2ndstage, are left out.  Every other build gets
the source byte for byte.  A source without exactly one of each line is refused,
so a change of the source cannot leave the serial console's lines behind
unnoticed.

usage: make-rpi4-config.py --source FILE --bluetooth y|n --serial y|n --output FILE
"""

import argparse
import sys
from pathlib import Path

# The lines that hand the PL011 to the serial console, as the source spells them.
SERIAL_LINES = ("enable_uart=1", "uart_2ndstage=1", "dtoverlay=disable-bt")

# The line whose comment block above it goes with it.
COMMENTED_LINE = "uart_2ndstage=1"


def bluetooth_config(text: str) -> str:
    """Returns the source without the serial console's lines and their comment."""
    lines = text.splitlines(keepends=True)

    # Each serial line exactly once.
    for wanted in SERIAL_LINES:
        count = sum(1 for line in lines if line.rstrip("\r\n") == wanted)
        if count != 1:
            raise SystemExit(f"make-rpi4-config: '{wanted}' appears {count} times in the source, not once")

    # The comment lines right above uart_2ndstage, which describe it alone.
    commented = next(index for index, line in enumerate(lines)
                     if line.rstrip("\r\n") == COMMENTED_LINE)
    first = commented
    while first > 0 and lines[first - 1].startswith("#"):
        first -= 1
    dropped = set(range(first, commented))

    # Everything else, in order.
    kept = [line for index, line in enumerate(lines)
            if index not in dropped and line.rstrip("\r\n") not in SERIAL_LINES]
    return "".join(kept)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--bluetooth", choices=("y", "n"), required=True)
    parser.add_argument("--serial", choices=("y", "n"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")
    if args.bluetooth == "y" and args.serial == "n":
        text = bluetooth_config(text)
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
