#!/usr/bin/env bash
# Record the exact tools used by a zedBSD validation run.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$repo/build/test-results/toolchain.json}"
mkdir -p "$(dirname "$output")"

python3 - "$repo" "$output" <<'PY'
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys

repo = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])

commands = [
    "cc", "gcc", "ld", "as", "objcopy", "readelf", "python3",
    "mcopy", "qemu-system-i386", "qemu-system-x86_64",
    "qemu-system-aarch64", "qemu-system-sparc64",
    "aarch64-linux-gnu-gcc", "aarch64-linux-gnu-ld",
    "aarch64-linux-gnu-readelf", "aarch64-linux-gnu-objdump",
]

for directory in ("~/opt/sparcv9/bin", "~/opt/sparc64/bin"):
    sparc_root = pathlib.Path(os.path.expanduser(directory))
    for name in ("sparc64-unknown-elf-gcc", "sparc64-unknown-elf-ld",
                 "sparc64-unknown-elf-readelf",
                 "sparc64-unknown-elf-objdump"):
        candidate = sparc_root / name
        if candidate.exists():
            commands.append(str(candidate))

def describe(command):
    resolved = shutil.which(command) if os.path.sep not in command else command
    if not resolved or not pathlib.Path(resolved).exists():
        return {"available": False}
    attempts = ([resolved, "--version"], [resolved, "-version"])
    text = ""
    for argv in attempts:
        try:
            result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=10,
                                    check=False)
        except (OSError, subprocess.TimeoutExpired):
            continue
        text = result.stdout.splitlines()[0] if result.stdout else ""
        if text:
            break
    return {"available": True, "path": str(pathlib.Path(resolved).resolve()),
            "version": text}

record = {
    "schema": 1,
    "revision": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
    "host": {"system": platform.system(), "release": platform.release(),
             "machine": platform.machine(), "python": platform.python_version()},
    "tools": {command: describe(command) for command in commands},
}
temporary = output.with_suffix(output.suffix + ".tmp")
temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                     encoding="utf-8")
temporary.replace(output)
print(output)
PY
