#!/usr/bin/env python3
# ws035-p035: replace the kernel's C library #include lines.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Rewrite the #include lines of the kernel, the drivers and the HAL.

usage: python3 plan/ws035/tests/kernel-include-rewrite.py [--check] [--json PATH]

Scope: the kcrt-rewrite scope (plan/ws035/tests/kcrt-rewrite.py: src/kern,
src/drivers without i915-old and the host-only i915 harness, include/kern,
include/drivers, plan/ws004/tests/pci-msi-qemu.c) plus src/hal, include/hal
and include/boot.  Rules (plan/ws035/kcrt-design.md section 8):

  <errno.h> <fcntl.h> <limits-of-the-system> ...   -> <uapi/...>  (MAP below)
  <string.h> <stdio.h>                             -> <kern/kcrt.h> (once)
  <stdlib.h> <wchar.h> <locale.h> <features.h>     -> removed
  <vulkan/X>                                       -> <libc/vulkan/X>
  <stdint.h> <stddef.h> <stdbool.h> <stdarg.h> <limits.h>
                                                   -> kept (the compiler's
                                                      freestanding headers)

A file that takes SEEK_SET/SEEK_CUR/SEEK_END from <stdio.h> gets
<uapi/unistd.h> in its place.  A file whose code names a system limit (NAME_MAX, PATH_MAX, SSIZE_MAX,
GETENTROPY_MAX, ARG_MAX, HOST_NAME_MAX) gets <uapi/limits.h> after its
<limits.h> line; the compiler's <limits.h> does not define them.  A
replacement whose target the file already includes is dropped instead of
duplicated.  A standard name included with quotes ("errno.h") is treated
the same.  <string.h> and <stdio.h> become <kern/kcrt.h> (design section 8),
not a bare removal: in a host fixture kcrt.h includes the host's <string.h>
at the same place the file used to, before a later macro such as
modeset-internal.h's ffs() could rename the host's declaration.  In the HAL
this is the approved <string.h> -> <kern/kcrt.h> (its console test build
calls memset).

--check writes nothing and exits 1 when a file would change.
"""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("kcrt_rewrite", REPO / "plan/ws035/tests/kcrt-rewrite.py")
kcrt_rewrite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kcrt_rewrite)

MAP = {
    "errno.h": "uapi/errno.h",
    "fcntl.h": "uapi/fcntl.h",
    "unistd.h": "uapi/unistd.h",
    "time.h": "uapi/time.h",
    "termios.h": "uapi/termios.h",
    "poll.h": "uapi/poll.h",
    "signal.h": "uapi/signal.h",
    "sys/types.h": "uapi/types.h",
    "sys/ioctl.h": "uapi/ioctl.h",
    "sys/stat.h": "uapi/stat.h",
    "sys/time.h": "uapi/time.h",
    "sys/mman.h": "uapi/mman.h",
    "sys/wait.h": "uapi/wait.h",
    "sys/statvfs.h": "uapi/statvfs.h",
    "sys/resource.h": "uapi/resource.h",
    "sys/mount.h": "uapi/mount.h",
    "sys/un.h": "uapi/un.h",
    "sys/socket.h": "uapi/socket.h",
    "sys/select.h": "uapi/select.h",
    "sys/sysctl.h": "uapi/sysctl.h",
}
REMOVE = {"string.h", "stdio.h", "stdlib.h", "wchar.h", "locale.h", "features.h"}
KEEP = {"stdint.h", "stddef.h", "stdbool.h", "stdarg.h", "limits.h", "stdalign.h",
        "stdnoreturn.h", "stdatomic.h", "float.h", "iso646.h"}
SEEK = re.compile(r"\bSEEK_(SET|CUR|END)\b")
LIMITS = re.compile(r"\b(NAME_MAX|PATH_MAX|SSIZE_MAX|GETENTROPY_MAX|ARG_MAX|HOST_NAME_MAX)\b")
INCLUDE = re.compile(r'^(\s*#\s*include\s*)(?:<([^>]+)>|"([^"]+)")(.*)$')
EXTRA_ROOTS = ["src/hal", "include/hal", "include/boot"]


def scope():
    files = set(kcrt_rewrite.scope())
    files.add("include/kern/kcrt.h")
    files.add("src/kern/kcrt.c")
    for root in EXTRA_ROOTS:
        base = REPO / root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in (".c", ".h", ".inc"):
                files.add(path.relative_to(REPO).as_posix())
    return sorted(files)


def is_hal(rel):
    return rel.startswith(("src/hal/", "include/hal/"))


def included(lines):
    names = set()
    for line in lines:
        m = INCLUDE.match(line)
        if m:
            names.add(m.group(2) or m.group(3))
    return names


def rewrite(rel, text):
    # kcrt.h's host face includes the host <string.h>/<stdio.h> on purpose.
    if rel == "include/kern/kcrt.h":
        return text, []
    code = "".join(c if keep or c == "\n" else " "
                   for c, keep in zip(text, kcrt_rewrite.code_mask(text)))
    needs_limits = bool(LIMITS.search(code))
    needs_seek = bool(SEEK.search(code))
    lines = text.split("\n")
    present = included(lines)
    out = []
    changes = []
    for number, line in enumerate(lines, 1):
        m = INCLUDE.match(line)
        if not m:
            out.append(line)
            continue
        prefix, angled, quoted, rest = m.groups()
        name = angled or quoted
        # A quoted include of a local file is not a C library header; a
        # quoted standard name ("errno.h") is one reached by accident.
        if quoted and not (name in MAP or name in REMOVE or name.startswith("vulkan/")):
            out.append(line)
            continue
        target = None
        if name.startswith("vulkan/"):
            target = "libc/" + name
        elif name in MAP:
            target = MAP[name]
        elif name == "stdio.h" and needs_seek:
            target = "uapi/unistd.h"
        elif name in ("string.h", "stdio.h"):
            target = "kern/kcrt.h"
        elif name in REMOVE:
            changes.append({"line": number, "from": name, "to": None})
            continue
        if target is None:
            out.append(line)
            if name == "limits.h" and needs_limits and "uapi/limits.h" not in present:
                out.append("%s<uapi/limits.h>" % prefix)
                present.add("uapi/limits.h")
                changes.append({"line": number, "from": name, "to": "limits.h + uapi/limits.h"})
            continue
        if target in present:
            changes.append({"line": number, "from": name, "to": None, "duplicate_of": target})
            continue
        out.append("%s<%s>%s" % (prefix, target, rest))
        present.add(target)
        changes.append({"line": number, "from": name, "to": target})
    return "\n".join(out), changes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", default="plan/ws035/phase035/include-rewrite.json")
    args = parser.parse_args()
    files = scope()
    summary = {}
    counts = {}
    changed = []
    for rel in files:
        path = REPO / rel
        text = path.read_text(encoding="utf-8")
        new, changes = rewrite(rel, text)
        if new == text:
            continue
        changed.append(rel)
        summary[rel] = changes
        for change in changes:
            key = "%s -> %s" % (change["from"], change["to"] or "removed")
            counts[key] = counts.get(key, 0) + 1
        if not args.check:
            path.write_text(new, encoding="utf-8")
    if args.check:
        for rel in changed:
            print("would change: " + rel)
        print("kernel-include-rewrite check: files=%d would_change=%d" % (len(files), len(changed)))
        return 1 if changed else 0
    out = REPO / args.json
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"tool": "plan/ws035/tests/kernel-include-rewrite.py",
                               "scope_files": len(files), "changed_files": len(changed),
                               "changes": dict(sorted(counts.items())),
                               "files": summary}, indent=1) + "\n", encoding="utf-8")
    print("kernel-include-rewrite: files=%d changed=%d" % (len(files), len(changed)))
    for key, value in sorted(counts.items()):
        print("  %-40s %d" % (key, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
