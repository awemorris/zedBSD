#!/usr/bin/env python3
"""Validate the machine-readable zedBSD POSIX R2 implementation manifest."""

import csv
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MATRIX = ROOT / "tests" / "posix-r2-api.csv"
FIELDS = [
    "standard_section", "header", "symbol", "option_group", "mandatory",
    "kernel_entry", "libc_source", "runtime_test", "status", "notes",
]
STATUSES = {"implemented", "optional-disabled", "future"}


def fail(message: str) -> None:
    print(f"POSIX R2 API matrix: {message}", file=sys.stderr)
    raise SystemExit(1)


with MATRIX.open(newline="", encoding="utf-8") as stream:
    reader = csv.DictReader(stream)
    if reader.fieldnames != FIELDS:
        fail(f"unexpected columns: {reader.fieldnames!r}")
    rows = list(reader)

if not rows:
    fail("manifest is empty")

seen = set()
for line, row in enumerate(rows, 2):
    key = (row["header"], row["symbol"])
    if key in seen:
        fail(f"line {line}: duplicate {key[0]}:{key[1]}")
    seen.add(key)
    if row["status"] not in STATUSES:
        fail(f"line {line}: invalid status {row['status']!r}")
    if row["mandatory"] not in {"yes", "no"}:
        fail(f"line {line}: mandatory must be yes or no")
    for field in ("header", "runtime_test"):
        value = row[field]
        if not value or not (ROOT / value).is_file():
            fail(f"line {line}: missing {field} file {value!r}")
    if row["status"] == "implemented":
        for field in ("kernel_entry", "libc_source"):
            value = row[field]
            if not value or not (ROOT / value).is_file():
                fail(f"line {line}: implemented symbol lacks {field} file")
    elif row["status"] == "optional-disabled" and row["mandatory"] != "no":
        fail(f"line {line}: a mandatory interface cannot be optional-disabled")

implemented = sum(row["status"] == "implemented" for row in rows)
disabled = sum(row["status"] == "optional-disabled" for row in rows)
print(f"zedBSD POSIX R2 API matrix: PASS ({implemented} implemented, "
      f"{disabled} optional-disabled)")
