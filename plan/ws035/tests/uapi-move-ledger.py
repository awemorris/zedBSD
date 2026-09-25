#!/usr/bin/env python3
# ws035-p035: which C library definitions the kernel really uses.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""List the names the kernel takes from the C library headers.

usage: python3 plan/ws035/tests/uapi-move-ledger.py [--json PATH]

Collects every name a C library public header (include/libc, without the
vulkan/, wayland and X11 trees) defines -- object-like and function-like
macros, typedef names, struct/union/enum tags and enumerators -- and every
identifier the kernel, driver and HAL sources use (the kcrt-rewrite scope plus
src/hal, include/hal and include/uapi).  A name that is used by the kernel,
defined by the C library and defined neither by include/uapi nor by the kernel
itself is one the kernel still takes from the C library.  The output groups
those names by the C library header that defines them; it is the work list of
plan/ws035/kcrt-design.md section 8 (what moves to include/uapi).

Comments, strings and character literals are not read.  A name the kernel only
mentions inside a disabled conditional is still counted (the ledger errs on
the side of listing too much; the build decides).
"""

import argparse
import importlib.util
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("kcrt_rewrite", REPO / "plan/ws035/tests/kcrt-rewrite.py")
kcrt_rewrite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kcrt_rewrite)

LIBC_EXCLUDE = ("vulkan/", "wayland", "X11/", "xdg-shell")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", re.M)
TYPEDEF_SIMPLE = re.compile(r"\btypedef\b[^;{}]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;")
TYPEDEF_FUNC = re.compile(r"\btypedef\b[^;{}]*\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
TYPEDEF_BODY = re.compile(r"\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")
TAG = re.compile(r"\b(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")
ENUM_BODY = re.compile(r"\benum\b[^{;]*\{([^}]*)\}")


def strip(text):
    mask = kcrt_rewrite.code_mask(text)
    return "".join(c if keep or c == "\n" else " " for c, keep in zip(text, mask))


def definitions(text):
    code = strip(text)
    names = set(DEFINE.findall(code))
    for m in TYPEDEF_SIMPLE.finditer(code):
        names.add(m.group(1))
    names.update(TYPEDEF_FUNC.findall(code))
    # typedef struct {...} name;  (only when a typedef opens the body)
    for m in TYPEDEF_BODY.finditer(code):
        head = code[:m.start()]
        open_at = head.rfind("{")
        if open_at >= 0 and re.search(r"\btypedef\s+(struct|union|enum)\b[^;{}]*$", head[:open_at]):
            names.add(m.group(1))
    for kind, tag in TAG.findall(code):
        names.add(kind + " " + tag)
    for body in ENUM_BODY.findall(code):
        for item in body.split(","):
            m = IDENT.match(item.strip())
            if m:
                names.add(m.group(0))
    return names


def used(text):
    code = strip(text)
    names = set(IDENT.findall(code))
    for kind, tag in re.findall(r"\b(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)", code):
        names.add(kind + " " + tag)
    return names


def files_under(root, suffixes=(".h",)):
    return sorted(p for p in (REPO / root).rglob("*") if p.is_file() and p.suffix in suffixes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", default="plan/ws035/phase035/uapi-move-ledger.json")
    args = parser.parse_args()

    libc_defs = defaultdict(set)
    for path in files_under("include/libc"):
        rel = path.relative_to(REPO / "include/libc").as_posix()
        if rel.startswith(LIBC_EXCLUDE):
            continue
        for name in definitions(path.read_text(encoding="utf-8", errors="replace")):
            libc_defs[name].add(rel)

    uapi_defs = set()
    for path in files_under("include/uapi"):
        uapi_defs |= definitions(path.read_text(encoding="utf-8", errors="replace"))

    kernel_files = list(kcrt_rewrite.scope())
    kernel_files += [p.relative_to(REPO).as_posix() for p in files_under("src/hal", (".c", ".h", ".inc"))]
    kernel_files += [p.relative_to(REPO).as_posix() for p in files_under("include/hal")]
    kernel_files += ["include/kern/kcrt.h", "src/kern/kcrt.c"]
    kernel_defs = set()
    users = defaultdict(set)
    for rel in sorted(set(kernel_files)):
        text = (REPO / rel).read_text(encoding="utf-8", errors="replace")
        kernel_defs |= definitions(text)
        for name in used(text):
            if name in libc_defs:
                users[name].add(rel)

    by_header = defaultdict(dict)
    in_uapi = defaultdict(dict)
    for name, files in sorted(users.items()):
        entry = {"users": len(files), "example": sorted(files)[:3],
                 "kernel_defines_too": name in kernel_defs}
        for header in sorted(libc_defs[name]):
            if name in uapi_defs:
                in_uapi[header][name] = entry
            else:
                by_header[header][name] = entry

    result = {
        "tool": "plan/ws035/tests/uapi-move-ledger.py",
        "kernel_files": len(set(kernel_files)),
        "take_from_libc": {h: by_header[h] for h in sorted(by_header)},
        "already_in_uapi": {h: sorted(in_uapi[h]) for h in sorted(in_uapi)},
    }
    out = REPO / args.json
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    for header in sorted(by_header):
        print("%-22s %s" % (header, " ".join(sorted(by_header[header]))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
