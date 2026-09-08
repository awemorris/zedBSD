#!/usr/bin/env python3
"""Production memory map/ownership tests with reproducible build logs."""
__import__('runpy').run_path(str(__import__('pathlib').Path(__file__).resolve().parents[3] / 'plan/ws025-io-memory-cache/tests/prepare-driver-fragments.py'), run_name='__main__')
import json
import os
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(repo / "plan/ws025-io-memory-cache/temp")
out.mkdir(parents=True, exist_ok=False)
groups = {
    "framebuffer-map": ["plan/ws025-io-memory-cache/tests/framebuffer-map-host.c",
                        "src/hal/amd64/framebuffer-map.c"],
    "pmem-retire": ["plan/ws025-io-memory-cache/tests/pmem-retire-host.c",
                    "src/hal/amd64/pmem-range.c"],
    "pmem-compat": ["plan/ws025-io-memory-cache/tests/pmem-compat-host.c",
                    "src/hal/pmem-constraints.c"],
    "pmem-range": ["plan/ws025-io-memory-cache/tests/pmem-range-host.c",
                   "src/hal/amd64/pmem-range.c"],
    "dma-range": ["plan/ws025-io-memory-cache/tests/dma-range-host.c",
                  "plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/dma.c", "src/hal/amd64/pmem-range.c", "src/kern/io-stats.c"],
    "dma-compat": ["plan/ws004-hardware/tests/dma-constraints-test.c",
                   "plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/dma.c", "src/hal/pmem-constraints.c", "src/kern/io-stats.c"],
    "dma-lock": ["plan/ws003-bringup/tests/dma-allocation-lock-test.c",
                 "plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/dma.c", "src/hal/pmem-constraints.c", "src/kern/io-stats.c"],
    "ram-map": ["plan/ws025-io-memory-cache/tests/ram-map-host.c",
                "src/hal/amd64/ram-map.c"],
    "legacy-parameters": ["plan/ws003-bringup/tests/x86-parameter-handoff-test.c",
                          "src/hal/x86/boot-parameters.c",
                          "src/hal/amd64/bsp-pcat/handoff-validation.c"],
    "legacy-uefi-map": ["plan/ws003-bringup/tests/uefi-memory-map-test.c",
                        "bootloader/uefi/memory-map.c"],
    "normalize": ["plan/ws025-io-memory-cache/tests/memory-normalize-host.c",
                  "bootloader/common/memory-map.c"],
    "handoff": ["plan/ws025-io-memory-cache/tests/memory-handoff-host.c",
                "bootloader/common/memory-map.c", "bootloader/bios/memory-map.c",
                "bootloader/uefi/memory-map-v6.c",
                "src/hal/amd64/bsp-pcat/handoff-validation.c"],
}
commands = []
for variant in ("ordinary", "sanitize"):
    for name, sources in groups.items():
        if name.startswith("dma-"):
            sources = [*sources, "plan/ws025-io-memory-cache/tests/host-fatal.c"]
        binary = out / (name + "-" + variant)
        flags = [] if variant == "ordinary" else [
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
        for suffix, argv in [("build", ["cc", "-std=c11", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-pthread", "-I.", "-Iinclude", "-Iinclude/uapi", *flags, *sources, "-o", str(binary)]),
                ("run", [str(binary)])]:
            commands.append(argv)
            (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            with (out / (binary.name + "-" + suffix + ".log")).open("w") as log:
                result = subprocess.run(argv, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1",
                         "UBSAN_OPTIONS": "halt_on_error=1"})
            if result.returncode:
                raise SystemExit(f"FAIL: {binary.name} {suffix}: {result.returncode}")
        print("PASS:", binary.name, flush=True)
