#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-vkdemo-cli.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc -std=c89 -pedantic -Wall -Wextra -Werror \
    -D_POSIX_C_SOURCE=200809L -I "$repo/include" \
    "$repo/userland/base/vkdemo/main.c" \
    "$repo/plan/ws014/tests/vkdemo-cli.c" \
    -o "$work/vkdemo-cli"

python3 - "$work/vkdemo-cli" <<'PY'
import subprocess
import sys

binary = sys.argv[1]
accepted = [
    [],
    ["--duration=0"],
    ["--duration=3600"],
    ["--time-ms=0", "--hold=0"],
    ["--time-ms=3600000", "--hold=120"],
    ["--verify-session", "--token=q307-001_A.test"],
    ["--verify-session", "--token=" + "a" * 64],
    ["--device-index=37", "--duration=1"],
    ["--device-index=4294967295", "--offscreen"],
    ["--offscreen", "--output=/tmp/frame.ppm", "--verify-session"],
]
rejected = [
    ["--duration="],
    ["--duration=-1"],
    ["--duration=+1"],
    ["--duration=3601"],
    ["--duration=999999999999999999999"],
    ["--duration=1x"],
    ["--time-ms=3600001"],
    ["--time-ms=0", "--hold=121"],
    ["--time-ms=0", "--hold=-1"],
    ["--hold=0"],
    ["--time-ms=1", "--duration=1"],
    ["--verify-session", "--duration=0"],
    ["--verify-session", "--time-ms=0"],
    ["--verify-session", "--hold=0"],
    ["--device=/dev/gpu0"],
    ["--device-index="],
    ["--device-index=-1"],
    ["--device-index=4294967296"],
    ["--offscreen=true"],
    ["--output="],
    ["--token="],
    ["--token=a b"],
    ["--token=a/b"],
    ["--token=" + "a" * 65],
    ["--unknown"],
]
for valid, cases in ((True, accepted), (False, rejected)):
    for arguments in cases:
        result = subprocess.run(
            [binary, *arguments], capture_output=True, text=True, timeout=5
        )
        expected = 1 if valid else 2
        entered = "VKDEMO-CLI-STUB initialize" in result.stdout
        if result.returncode != expected or entered != valid:
            raise SystemExit(
                f"CLI case failed: {arguments!r}, rc={result.returncode}, "
                f"stdout={result.stdout!r}, stderr={result.stderr!r}"
            )
print(f"vkdemo CLI: PASS ({len(accepted)} accepted, {len(rejected)} rejected)")
PY
