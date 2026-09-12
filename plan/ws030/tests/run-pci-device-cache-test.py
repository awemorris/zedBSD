"""Exercise unchanged PCI BAR and kernel cache-policy mapping functions."""
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
sources = {
    "src/kern/pmem.c": ["pmem_error", "kern_device_map", "kern_device_unmap"],
    "src/drivers/pci/pci-pcat.c": ["pcat_map_bar", "pcat_unmap_bar"],
}
functions = []
prototypes = []
for path, names in sources.items():
    source = (root / path).read_text()
    for name in names:
        match = re.search(r"\n([^\n]+)\n" + name + r"\(\n", source)
        if match is None:
            raise RuntimeError("missing function " + name)
        start = match.start() + 1
        opening = source.index("{", match.end())
        cursor = opening + 1
        depth = 1
        while depth:
            depth += (source[cursor] == "{") - (source[cursor] == "}")
            cursor += 1
        functions.append(source[start:cursor])
        prototypes.append(" ".join(source[start:opening].split()) + ";")
source = (root / "src/drivers/pci/pci-pcat.c").read_text()
start = source.index("struct pcat_bar_mapping {")
end = source.index("\n};", start) + 3
(out / "pci-cache-definition.h").write_text(source[start:end] + "\n")
(out / "pci-cache-functions.h").write_text("\n\n".join(functions) + "\n")
(out / "pci-cache-prototypes.h").write_text("\n".join(prototypes) + "\n")
paths = [*sources, "include/drivers/pci.h", "include/kern/pmem.h", "include/hal/hal.h",
         "plan/ws030/tests/pci-device-cache.c", "plan/ws030/tests/run-pci-device-cache-test.py"]
(out / "source.json").write_text(json.dumps({
    "files": {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
    "complete_functions": sources,
    "scope": "Real PCI and kernel cache translation; mock HAL, allocator, BAR assignment and logging only.",
}, indent=2) + "\n")
for mode in ("ordinary", "sanitize"):
    extra = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if mode == "sanitize" else []
    with (out / (mode + ".log")).open("w") as log:
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE",
                        "-Iinclude", "-Iinclude/uapi", "-I.", "-I" + str(out), *extra,
                        paths[-2], "-o", str(out / mode)], cwd=root, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=60)
        subprocess.run([str(out / mode)], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=20, env={**os.environ,
                       "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                       "UBSAN_OPTIONS": "halt_on_error=1"})
    print(mode + ": PASS")
