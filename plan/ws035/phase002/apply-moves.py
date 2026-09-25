#!/usr/bin/env python3
# ws035-p002: apply the include/drivers/ moves from the pre-move inventory.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Apply the p002 moves recorded by refactor-refs.py (run before the move).

usage: python3 plan/ws035/phase002/apply-moves.py [--refs DIR] [--dry-run]

1. Rewrite every reference line of refs.tsv (phase p002) that is not left
   alone: #include spellings become the new spelling, path tokens in scripts
   become the new path.
2. git mv every p002 file of moves.tsv (history is kept).
3. Rename the two PCI registration functions everywhere they are built.

Left alone (decisions of ws035-p002): plan records (past results are not
rewritten), src/drivers/gpu/i915-old/ (not built), AGENTS.md (a handoff
record), and the directory-level mentions in the WS035 tools themselves.
"""

import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

LEAVE_FILES = {
    "AGENTS.md",
    "plan/ws035/tests/refactor-refs.py",
    "plan/ws035/tests/kernel-include-audit.py",
}

RENAMES = [
    ("drv_i915_pci_driver_register", "drv_pci_i915_driver_register"),
    ("drv_venus_pci_driver_register", "drv_pci_venus_driver_register"),
]

RENAME_FILES = [
    "include/drivers/pci/pci-i915.h",
    "include/drivers/pci/pci-venus.h",
    "src/drivers/gpu/i915/i915.c",
    "src/drivers/gpu/venus/venus.c",
    "src/kern/platform/pcat.c",
]

PATH_END = r"(?![A-Za-z0-9_./-])"


def left_alone(kind, f):
    return (kind == "plan-records" or "/i915-old/" in f or f in LEAVE_FILES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refs", default="plan/ws035/phase002/pre-refs")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    refs_dir = REPO / args.refs

    with open(refs_dir / "moves.tsv", encoding="utf-8") as fp:
        moves = [r for r in csv.DictReader(fp, delimiter="\t") if r["phase"] == "p002"]
    with open(refs_dir / "refs.tsv", encoding="utf-8") as fp:
        refs = [r for r in csv.DictReader(fp, delimiter="\t", quoting=csv.QUOTE_NONE)
                if r["phase"] == "p002"]
    newpath = {m["old"]: m["new"] for m in moves}

    edits = {}  # file -> {line: [(old, new)]}
    skipped = []
    for r in refs:
        f, kind, change, target = r["file"], r["kind"], r["change"], r["target"]
        if left_alone(kind, f):
            skipped.append((f, r["line"], target))
            continue
        if change.startswith("keep"):
            continue
        if change == "path":
            if target.endswith("/"):
                skipped.append((f, r["line"], target))
                continue
            pair = (target, newpath[target])
        else:
            old, new = change.split(" -> ")
            pair = (old, new)
        edits.setdefault(f, {}).setdefault(int(r["line"]), []).append(pair)

    changed_lines = 0
    for f, lines in sorted(edits.items()):
        path = REPO / f
        text = path.read_text(encoding="utf-8")
        out = text.splitlines(keepends=True)
        for lineno, pairs in lines.items():
            line = out[lineno - 1]
            for old, new in pairs:
                if old.startswith("include/"):
                    pat = r"(?<![A-Za-z0-9_./-])" + re.escape(old) + PATH_END
                else:
                    pat = r"(?<=[<\"])" + re.escape(old) + r"(?=[>\"])"
                line, n = re.subn(pat, new, line)
                if n == 0:
                    raise SystemExit(f"{f}:{lineno}: {old} not found in {out[lineno - 1]!r}")
            out[lineno - 1] = line
            changed_lines += 1
        if not args.dry_run:
            path.write_text("".join(out), encoding="utf-8")

    for m in moves:
        new = REPO / m["new"]
        if args.dry_run:
            continue
        new.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "mv", m["old"], m["new"]], cwd=REPO, check=True)

    renamed = 0
    for f in RENAME_FILES:
        path = REPO / f
        if args.dry_run:
            continue
        text = path.read_text(encoding="utf-8")
        for old, new in RENAMES:
            text, n = re.subn(r"\b" + old + r"\b", new, text)
            renamed += n
        path.write_text(text, encoding="utf-8")

    print(f"files edited: {len(edits)}, lines edited: {changed_lines}, "
          f"moves: {len(moves)}, renamed identifiers: {renamed}")
    print(f"left alone: {len(skipped)} rows")
    for f, line, target in skipped:
        if not f.startswith("plan/") or "/tests/" in f:
            print(f"  {f}:{line} {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
