#!/usr/bin/env python3
"""Production-linked WS025 counter tests, with retained commands and logs."""
import argparse
import json
import os
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
out = args.output.resolve()
out.relative_to(REPO / "plan/ws025-io-memory-cache/temp")
out.mkdir(parents=True, exist_ok=False)
commands = []

def run(name, argv):
    print(name, flush=True)
    commands.append({"name": name, "argv": argv})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    with (out / (name + ".log")).open("w") as log:
        result = subprocess.run(argv, cwd=REPO, stdout=log, stderr=subprocess.STDOUT,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1", "UBSAN_OPTIONS": "halt_on_error=1"})
    if result.returncode:
        raise SystemExit((out / (name + ".log")).read_text())
    print((out / (name + ".log")).read_text()[-500:], end="", flush=True)

for variant in ("ordinary", "sanitize"):
    extra = [] if variant == "ordinary" else [
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    flags = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Iinclude", "-Iinclude/uapi", "-Isrc", "-I.", *extra]
    binary = out / ("counter-" + variant)
    run("counter-build-" + variant, [*flags, "-pthread",
        "plan/ws025-io-memory-cache/tests/io-stats-host.c", "src/kern/io-stats.c",
        "-o", str(binary)])
    run("counter-" + variant, ["timeout", "60s", str(binary)])
    thread = out / ("thread-" + variant + ".o")
    run("thread-build-" + variant, ["cc", "-std=c11", "-O1", "-g", "-pthread", *extra,
        "-c", "plan/ws018-kernel-architecture/tests/mount-thread-host.c", "-o", str(thread)])
    for version in (2,):
        binary = out / f"ufs{version}-{variant}"
        run(f"ufs{version}-build-{variant}", [*flags,
            "-ffunction-sections", "-fdata-sections", "-DZEDBSD_USER_ABI_LP64",
            f"-DUFS_AUDIT_VERSION={version}", "-Ilibc/include", "-pthread",
            *([] if variant == "ordinary" else ["--param", "asan-globals=0"]),
            "plan/ws025-io-memory-cache/tests/ufs-run-host.c",
            "src/drivers/fs/ufs/ufs-endian.c",
            "src/kern/quota.c", "src/kern/io-stats.c", str(thread),
            "-Wl,--gc-sections", "-o", str(binary)])
        run(f"ufs{version}-{variant}", ["timeout", "60s", str(binary)])
print("WS025 UFS run host gates PASS")
