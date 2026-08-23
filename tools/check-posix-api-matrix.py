#!/usr/bin/env python3
"""Validate the machine-readable zedBSD POSIX R2 implementation manifest."""

import csv
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MATRIX = ROOT / "tests" / "posix-r2-api.csv"
FIELDS = [
    "standard", "option", "header", "symbol", "libc_source", "syscall",
    "kernel_source", "positive_test", "errno_test", "boundary_test",
    "thread_test", "arch_status", "status", "notes",
]
STATUSES = {"implemented", "optional-disabled", "future"}
ARCHITECTURES = {"i386", "amd64", "aarch64", "sparcv9", "m68k"}


def fail(message):
    print(f"POSIX R2 API matrix: {message}", file=sys.stderr)
    raise SystemExit(1)


with MATRIX.open(newline="", encoding="utf-8") as stream:
    reader = csv.DictReader(stream)
    if reader.fieldnames != FIELDS:
        fail(f"unexpected columns: {reader.fieldnames!r}")
    rows = list(reader)

if not rows:
    fail("manifest is empty")

public_headers = list((ROOT / "libc/include").rglob("*.h")) + list(
    (ROOT / "include/uapi").rglob("*.h"))
syscall_header = (ROOT / "include/uapi/zedbsd/syscall.h").read_text(
    encoding="utf-8")
seen = set()
for line, row in enumerate(rows, 2):
    key = (row["header"], row["symbol"])
    if key in seen:
        fail(f"line {line}: duplicate {key[0]}:{key[1]}")
    seen.add(key)
    if row["status"] not in STATUSES:
        fail(f"line {line}: invalid status {row['status']!r}")
    header = ROOT / row["header"]
    if not row["header"] or not header.is_file():
        fail(f"line {line}: missing header file {row['header']!r}")
    if row["status"] == "implemented":
        for field in ("libc_source", "positive_test"):
            value = row[field]
            if not value or not (ROOT / value).is_file():
                fail(f"line {line}: implemented symbol lacks {field} file")
        declaration = re.compile(r"\b" + re.escape(row["symbol"]) + r"\s*\(")
        if not any(declaration.search(path.read_text(encoding="utf-8"))
                   for path in public_headers):
            fail(f"line {line}: {row['symbol']} is not publicly declared")
    for field in ("kernel_source", "errno_test", "boundary_test",
                  "thread_test"):
        value = row[field]
        if value and not (ROOT / value).is_file():
            fail(f"line {line}: missing {field} file {value!r}")
    arches = set(filter(None, row["arch_status"].split("|")))
    if row["status"] == "implemented" and not arches:
        fail(f"line {line}: implemented symbol has no architecture evidence")
    if not arches.issubset(ARCHITECTURES):
        fail(f"line {line}: invalid architecture set {row['arch_status']!r}")
    if row["syscall"] and row["syscall"] not in syscall_header:
        fail(f"line {line}: unknown syscall {row['syscall']!r}")

implemented = sum(row["status"] == "implemented" for row in rows)
disabled = sum(row["status"] == "optional-disabled" for row in rows)
print(f"zedBSD POSIX R2 API matrix: PASS ({implemented} implemented, "
      f"{disabled} optional-disabled)")
