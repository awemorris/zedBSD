#!/usr/bin/env python3
# ws035-p035: host fixtures that read the zedBSD C library headers ask for the zedBSD ABI.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Add -DKERN_UAPI_NATIVE after every -I<...>include/libc of a host fixture.

usage: python3 plan/ws035/tests/add-uapi-native.py [--check] [--list PATH]

A fixture that puts the zedBSD C library headers (include/libc) on its
include path with -I compiles against the zedBSD ABI, not the host's.  Since
ws035-p035 those headers take the ABI from include/uapi, and include/uapi
gives the zedBSD definitions only to a zedBSD target (__ZEDBSD__) or to a
build that defines KERN_UAPI_NATIVE (include/uapi/hosted.h); without it a
host build meets a clear #error.  This script adds the definition, once per
-I token, to the host fixtures (plan/**/tests, plan/**/phase*/tests,
src/drivers/gpu/i915/tests, the host tests of src/libc/softfloat.mk).

Left alone: -idirafter include/libc (the host headers come first there, which
is the host ABI the fixture wants), -I.../include/libc/<subdirectory>, the
target builds (Makefile, platform/, toolchain/, the soft-float objects), files
that already say KERN_UAPI_NATIVE, and this Phase's own tools.

Token forms rewritten:
  shell/make   -I<path>include/libc  -I"<path>include/libc"  (also inside a
               "..." string: the definition goes in the same string)
  python       '-Iinclude/libc' / "-Iinclude/libc" list items

The list of changed files goes to plan/ws035/phase035/native-fixtures.txt.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DEFINE = "-DKERN_UAPI_NATIVE"
SKIP = {"plan/ws035/tests/uapi-value-ledger.py", "plan/ws035/tests/refactor-refs.py",
        "plan/ws035/tests/add-uapi-native.py"}
SHELL_TOKEN = re.compile(r'(-I(?:"[^"\s]*include/libc"|[^\s"\']*include/libc))(?=["\s\\;)]|$)')
PY_TOKEN = re.compile(r"""(?P<q>['"])-Iinclude/libc(?P=q)""")
# the soft-float target objects are compiled for zedBSD; only the host tests
SOFTFLOAT_HOST = re.compile(r"\$\(HOSTCC\)")


def candidates():
    out = subprocess.run(["git", "grep", "-lE", "-e", r"-I[^ ]*include/libc", "--",
                          "plan", "src/drivers/gpu/i915/tests", "src/libc/softfloat.mk"],
                         cwd=REPO, capture_output=True, text=True, check=False).stdout.split()
    files = []
    for rel in out:
        if rel in SKIP or rel.endswith((".md", ".json", ".log", ".txt", ".tsv", ".patch", ".c", ".h")):
            continue
        if "/temp/" in rel or "/handover/" in rel:
            continue
        files.append(rel)
    return sorted(files)


def rewrite_shell(text, host_only_makefile):
    lines = text.split("\n")
    changed = 0
    in_host_rule = False
    for index, line in enumerate(lines):
        if host_only_makefile:
            # A recipe line of a make rule; the host test rules start with $(HOSTCC).
            if SOFTFLOAT_HOST.search(line):
                in_host_rule = True
            elif not line.startswith("\t"):
                in_host_rule = False
            elif not lines[index - 1].rstrip().endswith("\\"):
                in_host_rule = bool(SOFTFLOAT_HOST.search(line))
            if not in_host_rule:
                continue
        if DEFINE in line:
            continue
        new, count = SHELL_TOKEN.subn(r"\1 " + DEFINE, line)
        if count:
            lines[index] = new
            changed += count
    return "\n".join(lines), changed


def rewrite_python(text):
    lines = text.split("\n")
    changed = 0
    for index, line in enumerate(lines):
        if DEFINE in line:
            continue
        new, count = PY_TOKEN.subn(lambda m: "%s-Iinclude/libc%s, %s%s%s" % (
            m.group("q"), m.group("q"), m.group("q"), DEFINE, m.group("q")), line)
        new2, count2 = SHELL_TOKEN.subn(r"\1 " + DEFINE, new) if not count else (new, 0)
        if count or count2:
            lines[index] = new2
            changed += count + count2
    return "\n".join(lines), changed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list", default="plan/ws035/phase035/native-fixtures.txt")
    args = parser.parse_args()
    changed_files = []
    total = 0
    for rel in candidates():
        path = REPO / rel
        text = path.read_text(encoding="utf-8")
        if rel.endswith(".py"):
            new, count = rewrite_python(text)
        else:
            new, count = rewrite_shell(text, rel == "src/libc/softfloat.mk")
        if count == 0 or new == text:
            continue
        changed_files.append((rel, count))
        total += count
        if not args.check:
            path.write_text(new, encoding="utf-8")
    if args.check:
        for rel, count in changed_files:
            print("would change: %s (%d)" % (rel, count))
        print("add-uapi-native check: would_change=%d" % len(changed_files))
        return 1 if changed_files else 0
    listing = REPO / args.list
    listing.parent.mkdir(parents=True, exist_ok=True)
    listing.write_text("".join("%s\t%d\n" % item for item in changed_files), encoding="utf-8")
    print("add-uapi-native: files=%d tokens=%d" % (len(changed_files), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
