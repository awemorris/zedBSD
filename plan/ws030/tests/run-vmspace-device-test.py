"""Test complete production VM device functions with a stateful HAL peer.

The source slice includes publication, actual device-fault completion, splitting,
protection rollback, fork and pinning. Only the large generic fault dispatcher,
address-space construction, scheduling and non-device backing collaborators are
replaced. This is not a concurrent scheduler or real page-table test.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(root / "build")
out.mkdir(parents=True, exist_ok=False)
source_path = root / "src/kern/vmspace.c"
source = source_path.read_text()
names = [
    "vmspace_map_device", "vmspace_protect", "vmspace_unmap", "vmspace_user_range_valid", "vm_page_effective_prot",
    "vmspace_unpin_user_pages", "vmspace_pin_user_pages", "range_valid",
    "overlaps", "insert_region", "vmspace_fork_locked", "map_region",
    "find_region_locked", "find_page", "vmspace_wait_faults_locked",
    "vmspace_fault_wake_locked", "vmspace_check_locked",
    "vmspace_pin_mapping_ready", "vmspace_find_free_range_bounded_locked",
    "vmspace_find_free_range_locked", "detach_vm_page_for_unmap",
    "detach_region_pages_for_unmap", "release_detached_region_pages",
    "split_region_prepared", "split_region", "release_retired_regions",
    "vmspace_unmap_locked", "vmspace_protect_locked",
    "vmspace_generation_advance_locked", "vmspace_device_fault",
]
functions = []
prototypes = []
for name in names:
    match = re.search(r"\n([^\n]+)\n" + name + r"\(\n", source)
    if match is None:
        raise RuntimeError("missing complete function: " + name)
    start = match.start() + 1
    opening = source.index("{", match.end())
    cursor = opening + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    functions.append(source[start:cursor])
    prototypes.append(" ".join(source[start:opening].split()) + ";")
(out / "vmspace-device-functions.h").write_text("\n\n".join(functions) + "\n")
(out / "vmspace-device-prototypes.h").write_text("\n".join(prototypes) + "\n")
paths = ["src/kern/vmspace.c", "src/kern/vm-device.c",
         "include/kern/vmspace.h", "include/kern/vm-device.h", "include/hal/hal.h",
         "plan/ws030/tests/vmspace-device.c", "plan/ws030/tests/run-vmspace-device-test.py"]
(out / "source.json").write_text(json.dumps({
    "files": {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
    "complete_functions": names,
    "scope": __doc__,
}, indent=2) + "\n")
for mode in ("ordinary", "sanitize"):
    extra = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if mode == "sanitize" else []
    with (out / (mode + ".log")).open("w") as log:
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Iinclude", "-Iinclude/uapi", "-I.", "-I" + str(out), *extra,
                        paths[-2], "src/kern/vm-device.c", "-o", str(out / mode)],
                       cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        subprocess.run([str(out / mode)], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=20, env={**os.environ,
                       "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                       "UBSAN_OPTIONS": "halt_on_error=1"})
    print(mode + ": PASS")
