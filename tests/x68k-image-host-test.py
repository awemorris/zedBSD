#!/usr/bin/env python3
"""Negative-path tests for the independent X68k image checker."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def rejected(checker: Path, image: bytes, arguments: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="zedbsd-x68k-image-test-") as work:
        damaged = Path(work) / "damaged.hd"
        damaged.write_bytes(image)
        result = subprocess.run(
            ["python3", str(checker), *arguments, str(damaged)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode == 0:
            raise SystemExit("checker accepted a deliberately damaged image")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", required=True, type=Path)
    parser.add_argument("--stage1", required=True, type=Path)
    parser.add_argument("--stage2", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--shell", required=True, type=Path)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    common = ["--stage1", str(args.stage1), "--stage2", str(args.stage2),
              "--kernel", str(args.kernel), "--shell", str(args.shell)]
    original = args.image.read_bytes()

    damaged = bytearray(original)
    damaged[0] ^= 1
    rejected(args.checker, damaged, common)

    damaged = bytearray(original)
    damaged[2048] ^= 1
    rejected(args.checker, damaged, common)

    damaged = bytearray(original)
    damaged[8 * 512 + 24] ^= 1
    rejected(args.checker, damaged, common)

    stage2_lba = int.from_bytes(original[8 * 512 + 8:8 * 512 + 12], "big")
    damaged = bytearray(original)
    damaged[stage2_lba * 512] ^= 1
    rejected(args.checker, damaged, common)

    damaged = bytearray(original)
    damaged[4096 * 512 + 510] = 0
    rejected(args.checker, damaged, common)

    shell = args.shell.read_bytes()
    shell_offset = original.find(shell)
    if shell_offset < 0:
        raise SystemExit("shell payload is absent from the source image")
    damaged = bytearray(original)
    damaged[shell_offset] ^= 1
    rejected(args.checker, damaged, common)

    print("zedBSD X68k image negative-path tests: PASS")


if __name__ == "__main__":
    main()
