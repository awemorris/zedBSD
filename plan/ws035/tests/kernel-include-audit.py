#!/usr/bin/env python3
# ws035-p001: which C library headers the kernel and HAL objects really read.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Replay the kernel/HAL compile commands of a build log with full -M output.

usage: python3 plan/ws035/tests/kernel-include-audit.py [--out DIR] [--require-none] <name> <build.log> [...]
       python3 plan/ws035/tests/kernel-include-audit.py --compare BASE.json NEW.json

-MMD (what the build uses) leaves out headers reached through -isystem, which
is how amd64/pcat/pc98 kernels see the C library today (sysroot/usr/include).
This tool re-runs each kernel, HAL and kernel-libc compile command from the
log as a preprocessor-only -M pass (no object is written) and records every
header each object reads, classified by origin:

  libc       include/libc/<R> or <sysroot>/usr/include/<R> (R not uapi/)
  libc-vulkan  include/libc/vulkan/<R>: the Vulkan declarations, the one part
             of the C library the kernel may read (ws035-p035)
  uapi       include/uapi/<R> or <sysroot>/usr/include/uapi/<R>
  libc-internal  libc/<file> (src/libc/<file> after p003), private libc headers
  kernel     include/{kern,hal,drivers,boot}/..., src/...
  compiler   the compiler's own include directory
  other      anything else (reported by path)

The result (plan/ws035/phase001/include-audit/<name>.json) is the p023
allowlist: after include/libc moves to include/, the same audit must report
the same libc header set per object class (with include/libc/R read as
include/R) and no header of the C library from a new place.  --compare does
that check and exits non-zero on a difference.

--require-none (ws035-p035) additionally fails unless, for the kernel and hal
classes, the libc and libc-internal sets are empty, other holds only
bootloader/include/ headers, and compiler holds only the freestanding headers
(COMPILER_ALLOWED).
"""

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OUT = REPO / "plan/ws035/phase001/include-audit"
SYSROOT_INC = re.compile(r".*/sysroot/[^/]+/usr/include/(.+)$")
COMPILER_INC = re.compile(r".*/lib/(clang|gcc)/.*/include(-fixed)?/(.+)$")
COMPILER_ALLOWED = re.compile(r"^(stdint|stddef|stdbool|stdarg|limits|__stddef_[a-z_]+|__stdarg_[a-z_]+)\.h$")


def commands(log):
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    text = text.replace("\\\n", " ")
    for line in text.splitlines():
        if " -c " not in line or " -o " not in line:
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv or not re.search(r"(clang|gcc|cc)$", os.path.basename(argv[0])):
            continue
        try:
            src = argv[argv.index("-c") + 1]
        except (ValueError, IndexError):
            continue
        yield src, argv


def object_class(src):
    if src.startswith("src/hal/"):
        return "hal"
    if src.startswith(("src/kern/", "src/drivers/")):
        return "kernel"
    if src.startswith("libc/") or src.startswith("src/libc/"):
        return "kernel-libc"
    return None


def classify(path):
    p = os.path.normpath(path)
    if p.startswith(str(REPO) + "/"):
        p = p[len(str(REPO)) + 1:]
    m = SYSROOT_INC.match(p)
    if m:
        rel = m.group(1)
        return ("uapi", rel) if rel.startswith("uapi/") else ("libc", rel)
    m = COMPILER_INC.match(p)
    if m:
        return "compiler", m.group(3)
    if p.startswith("include/libc/vulkan/"):
        return "libc-vulkan", p[len("include/libc/"):]
    if p.startswith("include/libc/"):
        return "libc", p[len("include/libc/"):]
    if p.startswith("include/uapi/"):
        return "uapi", p[len("include/"):]
    # after p003 the private libc headers are src/libc/*; before, libc/*
    if p.startswith("src/libc/"):
        return "libc-internal", p[len("src/"):]
    if p.startswith("libc/"):
        return "libc-internal", p
    if p.startswith(("include/kern/", "include/hal/", "include/drivers/",
                     "include/boot/", "src/")):
        return "kernel", p
    # after p023 the C library headers are include/<R>
    if p.startswith("include/"):
        return "libc", p[len("include/"):]
    return "other", p


def audit_one(item):
    src, argv = item
    out = []
    skip = 0
    for i, a in enumerate(argv):
        if skip:
            skip -= 1
            continue
        if a in ("-MMD", "-MP", "-MD"):
            continue
        if a in ("-MF", "-MT", "-MQ", "-o"):
            skip = 1
            continue
        if a == "-c":
            continue
        out.append(a)
    with tempfile.NamedTemporaryFile(suffix=".d", delete=False) as tmp:
        dfile = tmp.name
    try:
        r = subprocess.run(out + ["-M", "-MF", dfile, "-o", "/dev/null"], cwd=REPO,
                           capture_output=True, text=True, timeout=60)
        if r.returncode:
            return src, None, r.stderr.strip().splitlines()[:3]
        deps = Path(dfile).read_text().replace("\\\n", " ").split(":", 1)[1].split()
        return src, deps, None
    finally:
        os.unlink(dfile)


def audit(name, logs, out_dir=None):
    items = {}
    for log in logs:
        for src, argv in commands(log):
            if object_class(src):
                items.setdefault(src, argv)
    result = {"name": name, "logs": [str(x) for x in logs], "objects": len(items),
              "failed": {}, "classes": {}}
    per_class = defaultdict(lambda: defaultdict(Counter))
    via = defaultdict(Counter)
    with ThreadPoolExecutor(max_workers=16) as pool:
        for src, deps, err in pool.map(audit_one, sorted(items.items())):
            cls = object_class(src)
            if deps is None:
                result["failed"][src] = err
                continue
            for d in deps[1:]:
                origin, rel = classify(d)
                per_class[cls][origin][rel] += 1
                if origin == "libc":
                    via[cls]["sysroot" if SYSROOT_INC.match(d) else "tree"] += 1
    for cls, origins in sorted(per_class.items()):
        result["classes"][cls] = {o: dict(sorted(c.items())) for o, c in sorted(origins.items())}
    # where the C library headers were read from (-isystem sysroot or the tree);
    # informative only, p023 may change it on purpose
    result["libc_via"] = {cls: dict(c) for cls, c in sorted(via.items())}
    out_dir = Path(out_dir) if out_dir else OUT
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"{name}.json").write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
    libc = {cls: len(v.get("libc", {})) for cls, v in result["classes"].items()}
    other = {cls: len(v.get("other", {})) for cls, v in result["classes"].items()}
    print(f"{name}: objects={len(items)} failed={len(result['failed'])} "
          f"libc-headers={libc} other={other} via={result['libc_via']}")


def require_none(path):
    result = json.loads(Path(path).read_text())
    status = 0
    if result["failed"]:
        status = 1
        print("objects the audit could not preprocess: %s" % sorted(result["failed"]))
    for cls in ("kernel", "hal"):
        origins = result["classes"].get(cls, {})
        for origin in ("libc", "libc-internal"):
            if origins.get(origin):
                status = 1
                print("%s/%s: %s" % (cls, origin, sorted(origins[origin])))
        other = [h for h in origins.get("other", {}) if not h.startswith("bootloader/include/")]
        if other:
            status = 1
            print("%s/other outside bootloader/include: %s" % (cls, sorted(other)))
        compiler = [h for h in origins.get("compiler", {}) if not COMPILER_ALLOWED.match(h)]
        if compiler:
            status = 1
            print("%s/compiler outside the freestanding set: %s" % (cls, sorted(compiler)))
        print("%s: libc=%d libc-internal=%d libc-vulkan=%d uapi=%d compiler=%s other=%s" % (
            cls, len(origins.get("libc", {})), len(origins.get("libc-internal", {})),
            len(origins.get("libc-vulkan", {})), len(origins.get("uapi", {})),
            sorted(origins.get("compiler", {})), sorted(origins.get("other", {}))))
    print("include audit require-none:", "PASS" if status == 0 else "FAIL")
    return status


def compare(base, new):
    a = json.loads(Path(base).read_text())
    b = json.loads(Path(new).read_text())
    status = 0
    for cls in sorted(set(a["classes"]) | set(b["classes"])):
        for origin in ("libc", "libc-internal", "uapi", "other"):
            sa = set(a["classes"].get(cls, {}).get(origin, {}))
            sb = set(b["classes"].get(cls, {}).get(origin, {}))
            if sa != sb:
                status = 1
                print(f"{cls}/{origin}: added {sorted(sb - sa)} removed {sorted(sa - sb)}")
    print("include audit compare:", "PASS" if status == 0 else "FAIL")
    return status


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--compare":
        return compare(sys.argv[2], sys.argv[3])
    args = sys.argv[1:]
    out_dir = None
    strict = False
    while args and args[0] in ("--out", "--require-none"):
        if args[0] == "--out":
            out_dir = REPO / args[1]
            args = args[2:]
        else:
            strict = True
            args = args[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    os.chdir(REPO)
    audit(args[0], [Path(x) for x in args[1:]], out_dir)
    if strict:
        return require_none((Path(out_dir) if out_dir else OUT) / f"{args[0]}.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
