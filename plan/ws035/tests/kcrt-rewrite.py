#!/usr/bin/env python3
# ws035-p034: rename the kernel's C library calls to the kcrt names.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Rewrite the kernel's standard string/format calls to kern_* (kcrt).

usage: python3 plan/ws035/tests/kcrt-rewrite.py [--check] [--json PATH]

Scope (plan/ws035/kcrt-design.md section 3.2): src/kern/**, src/drivers/**,
include/kern/**, include/drivers/** and plan/ws004/tests/pci-msi-qemu.c,
files ending in .c, .h or .inc.  Left alone: src/drivers/gpu/i915-old/**,
the host-only harness of i915 (tests/contracts/**, tests/display/host-*.c,
tests/display/host-test.h), and kcrt itself (include/kern/kcrt.h,
src/kern/kcrt.c), which defines the standard names on purpose.

Rules:
  1. A call token NAME( for NAME in the kcrt set becomes kern_NAME(.  Only
     code is rewritten: the file is lexed, and a name inside a comment, a
     string or a character literal is left alone (and listed as skipped).
     A name preceded by an identifier character, '.' or '->' is not a call
     of the C function and is not matched.
  2. hal_memset( becomes kern_memset( (the four kernel-side calls).
  3. A rewritten file that does not include <kern/kcrt.h> gets
     "#include <kern/kcrt.h>" after the last line of its first block of
     #include lines at the file's base conditional level (inside the include
     guard of a header), or at the shallowest level when the whole file is
     conditional.  A file with no #include at all (an .inc fragment) is not
     changed there; the files that #include it get the line instead.
  The <string.h> and <stdio.h> lines stay (removed in ws035-p035).

The rewrite is deterministic.  --check rewrites nothing and exits 1 when a
file would change (the fixed-point test).  Without --check the files are
written and the summary goes to plan/ws035/phase034/rewrite.json.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ROOTS = ["src/kern", "src/drivers", "include/kern", "include/drivers"]
EXTRA_FILES = ["plan/ws004/tests/pci-msi-qemu.c"]
EXTENSIONS = (".c", ".h", ".inc")
EXCLUDED_PREFIXES = [
    "src/drivers/gpu/i915-old/",
    "src/drivers/gpu/i915/tests/contracts/",
]
EXCLUDED_FILES = {
    "include/kern/kcrt.h",
    "src/kern/kcrt.c",
    "src/drivers/gpu/i915/tests/display/host-test.h",
}
EXCLUDED_PATTERNS = [re.compile(r"^src/drivers/gpu/i915/tests/display/host-[^/]*\.c$")]
NAMES = [
    "memcpy", "memmove", "memset", "memset_explicit", "memcmp", "memchr",
    "strlen", "strnlen", "strcmp", "strncmp", "strcpy", "strncpy", "strcat",
    "strchr", "strrchr", "strstr", "snprintf", "vsnprintf",
]
CALL = re.compile(r"(?<![A-Za-z0-9_.])(?<!->)(" + "|".join(NAMES) + r")(?=\s*\()")
HAL_MEMSET = re.compile(r"(?<![A-Za-z0-9_.])(?<!->)hal_memset(?=\s*\()")
KCRT_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]kern/kcrt\.h[>"]', re.M)
INCLUDE_LINE = re.compile(r'^\s*#\s*include\b')
INCLUDE_TARGET = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
DIRECTIVE = re.compile(r'^\s*#\s*(\w+)\s*(\w*)')
KCRT_LINE = "#include <kern/kcrt.h>"


def excluded(rel):
    if rel in EXCLUDED_FILES:
        return True
    for prefix in EXCLUDED_PREFIXES:
        if rel.startswith(prefix):
            return True
    for pattern in EXCLUDED_PATTERNS:
        if pattern.match(rel):
            return True
    return False


def scope():
    files = []
    for root in ROOTS:
        for directory, subdirs, names in os.walk(REPO / root):
            subdirs.sort()
            for name in sorted(names):
                if not name.endswith(EXTENSIONS):
                    continue
                rel = (Path(directory) / name).relative_to(REPO).as_posix()
                if not excluded(rel):
                    files.append(rel)
    for rel in EXTRA_FILES:
        files.append(rel)
    return sorted(set(files))


def code_mask(text):
    """Per character: True where the character is C code (not comment/literal)."""
    mask = [True] * len(text)
    i = 0
    n = len(text)
    state = "code"
    while i < n:
        c = text[i]
        if state == "code":
            if c == "/" and i + 1 < n and text[i + 1] == "*":
                state = "block"
                mask[i] = mask[i + 1] = False
                i += 2
                continue
            if c == "/" and i + 1 < n and text[i + 1] == "/":
                state = "line"
                mask[i] = False
                i += 1
                continue
            if c == '"':
                state = "string"
                mask[i] = False
                i += 1
                continue
            if c == "'":
                state = "char"
                mask[i] = False
                i += 1
                continue
            i += 1
            continue
        mask[i] = False
        if state == "block":
            if c == "*" and i + 1 < n and text[i + 1] == "/":
                mask[i + 1] = False
                state = "code"
                i += 2
                continue
        elif state == "line":
            if c == "\n":
                state = "code"
                mask[i] = True
        elif state in ("string", "char"):
            if c == "\\":
                if i + 1 < n:
                    mask[i + 1] = False
                i += 2
                continue
            quote = '"' if state == "string" else "'"
            if c == quote or c == "\n":
                state = "code"
        i += 1
    return mask


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def rewrite_calls(rel, text):
    mask = code_mask(text)
    counts = {}
    skipped = []
    pieces = []
    last = 0
    matches = [(m.start(), m.end(), m.group(0), "kern_" + m.group(1), m.group(1))
               for m in CALL.finditer(text)]
    matches += [(m.start(), m.end(), m.group(0), "kern_memset", "hal_memset")
                for m in HAL_MEMSET.finditer(text)]
    matches.sort()
    for start, end, old, new, key in matches:
        if not mask[start]:
            skipped.append({"file": rel, "line": line_of(text, start), "name": old})
            continue
        pieces.append(text[last:start])
        pieces.append(new)
        last = end
        counts[key] = counts.get(key, 0) + 1
    pieces.append(text[last:])
    return "".join(pieces), counts, skipped


def insertion_line(lines, header):
    """Index after the last line of the first base-level #include block, or None."""
    depth = 0
    base = 0
    guard = None
    if header:
        # An include guard (#ifndef X / #define X) at the top makes level 1 the base.
        for index, line in enumerate(lines):
            if not line.strip() or line.lstrip().startswith(("/*", "*", "//")):
                continue
            m = DIRECTIVE.match(line)
            if m and m.group(1) == "ifndef":
                guard = index
            break
        if guard is not None:
            base = 1
    # The first #include at the base level; failing that, the first one at the
    # shallowest level (a file whose whole body is inside one #ifdef).
    candidates = []
    for index, line in enumerate(lines):
        m = DIRECTIVE.match(line)
        if m:
            word = m.group(1)
            if word in ("if", "ifdef", "ifndef"):
                depth += 1
                continue
            if word == "endif":
                depth -= 1
                continue
        if INCLUDE_LINE.match(line) and depth >= base:
            candidates.append((depth, index))
    if not candidates:
        return None
    first = min(candidates)[1]
    end = first
    while end + 1 < len(lines) and INCLUDE_LINE.match(lines[end + 1]):
        end += 1
    return end + 1


def add_include(text, rel):
    if KCRT_INCLUDE.search(text):
        return text, False
    lines = text.split("\n")
    at = insertion_line(lines, rel.endswith(".h"))
    if at is None:
        return text, None
    lines.insert(at, KCRT_LINE)
    return "\n".join(lines), True


def includers(files, contents, fragment):
    name = os.path.basename(fragment)
    found = []
    for rel in files:
        for line in contents[rel].split("\n"):
            m = INCLUDE_TARGET.match(line)
            if m and os.path.basename(m.group(1)) == name:
                found.append(rel)
                break
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", default="plan/ws035/phase034/rewrite.json")
    args = parser.parse_args()

    files = scope()
    contents = {rel: (REPO / rel).read_text(encoding="utf-8") for rel in files}
    new = dict(contents)
    per_file = {}
    skipped = []
    totals = {}
    needs_include = set()
    fragments = []

    for rel in files:
        text, counts, skip = rewrite_calls(rel, contents[rel])
        skipped += skip
        if counts:
            new[rel] = text
            per_file[rel] = {"calls": counts}
            for key, value in counts.items():
                totals[key] = totals.get(key, 0) + value
        uses_kcrt = counts or re.search(r"(?<![A-Za-z0-9_])kern_(" + "|".join(NAMES) + r")\s*\(", text)
        if uses_kcrt:
            needs_include.add(rel)

    inserted = []
    for rel in sorted(needs_include):
        text, added = add_include(new[rel], rel)
        if added is None:
            fragments.append(rel)
            continue
        if added:
            new[rel] = text
            inserted.append(rel)
            per_file.setdefault(rel, {"calls": {}})["include_inserted"] = True

    fragment_includers = {}
    for fragment in fragments:
        users = includers(files, new, fragment)
        fragment_includers[fragment] = users
        for rel in users:
            text, added = add_include(new[rel], rel)
            if added:
                new[rel] = text
                inserted.append(rel)
                per_file.setdefault(rel, {"calls": {}})["include_inserted"] = True
            elif added is None:
                print("no include block to extend in %s (includer of %s)" % (rel, fragment), file=sys.stderr)
                return 2

    changed = sorted(rel for rel in files if new[rel] != contents[rel])
    if args.check:
        for rel in changed:
            print("would change: " + rel)
        print("kcrt-rewrite check: files=%d would_change=%d" % (len(files), len(changed)))
        return 1 if changed else 0

    for rel in changed:
        (REPO / rel).write_text(new[rel], encoding="utf-8")

    summary = {
        "tool": "plan/ws035/tests/kcrt-rewrite.py",
        "scope_files": len(files),
        "changed_files": len(changed),
        "call_replacements": dict(sorted(totals.items())),
        "call_replacements_total": sum(totals.values()),
        "include_insertions": len(sorted(set(inserted))),
        "include_removals": 0,
        "fragments_without_include_block": {k: v for k, v in sorted(fragment_includers.items())},
        "skipped_in_comment_or_literal": skipped,
        "files": {rel: per_file[rel] for rel in sorted(per_file)},
    }
    out = REPO / args.json
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=1, sort_keys=False) + "\n", encoding="utf-8")
    print("kcrt-rewrite: files=%d changed=%d calls=%d includes=%d skipped=%d"
          % (len(files), len(changed), sum(totals.values()), len(set(inserted)), len(skipped)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
