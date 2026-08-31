#!/usr/bin/env python3
"""Audit canonical copyright and explanation headers in userland source."""

import argparse
import csv
import hashlib
import pathlib
import re


CANONICAL = b"""/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

"""
MODELINE = b"/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */\n\n"
SUFFIXES = {".c", ".h", ".S"}


def source_paths(root):
    """Return every in-scope source in deterministic pathname order."""
    return sorted(path for path in root.rglob("*")
                  if path.is_file() and path.suffix in SUFFIXES)


def audit(path):
    """Return a diagnostic when one file lacks the exact required prefix."""
    data = path.read_bytes()
    if data.startswith(b"\xef\xbb\xbf"):
        return "UTF-8 BOM precedes canonical header"
    if not data.startswith(MODELINE):
        return "canonical Emacs modeline is not at byte zero"
    if not data[len(MODELINE):].startswith(CANONICAL):
        return "canonical copyright/SPDX block does not follow the modeline"

    remainder = data[len(MODELINE) + len(CANONICAL):]
    match = re.match(rb"/\*\n((?: \*[^\n]*\n)+) \*/\n\n", remainder)
    if match is None:
        return "separate multi-line file explanation is missing"
    prose = []
    for line in match.group(1).splitlines():
        text = line[2:].strip()
        if text:
            prose.append(text)
    if not prose:
        return "file explanation is empty"
    joined = b" ".join(prose).lower()
    forbidden = (b"copyright", b"spdx-license-identifier", b"-*-")
    if any(value in joined for value in forbidden):
        return "file explanation contains header metadata"

    return None


def implementation_body(data):
    """Return bytes after all migrated leading comment blocks."""
    position = 0
    while True:
        match = re.match(rb"[ \t\r\n]*/\*(.*?)\*/", data[position:], re.S)
        if match is None:
            break
        position += match.end()
    return data[position:].lstrip(b"\r\n")


def audit_body_inventory(inventory):
    """Verify bodies against the migration-time SHA-256 inventory."""
    failed = False
    count = 0
    with inventory.open(encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        for row in rows:
            path = pathlib.Path(row["path"])
            body = implementation_body(path.read_bytes())
            actual = hashlib.sha256(body).hexdigest()
            if actual != row["body-sha256"]:
                print(f"{path}: implementation body changed during migration")
                failed = True
            count += 1
    if failed:
        return -1
    print(f"USERLAND-HEADER-T003 implementation bodies: PASS ({count} files)")
    return count


def main():
    """Audit one complete source tree."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="userland")
    parser.add_argument("--expected-count", type=int, default=269)
    parser.add_argument("--body-inventory")
    arguments = parser.parse_args()

    root = pathlib.Path(arguments.root)
    paths = source_paths(root)
    failed = False
    if len(paths) != arguments.expected_count:
        print(f"USERLAND-HEADER-T001: source count {len(paths)}, "
              f"expected {arguments.expected_count}")
        failed = True

    for path in paths:
        diagnostic = audit(path)
        if diagnostic is not None:
            print(f"{path}: {diagnostic}")
            failed = True

    if failed:
        return 1

    if arguments.body_inventory:
        inventory_count = audit_body_inventory(
            pathlib.Path(arguments.body_inventory))
        if inventory_count != len(paths):
            return 1

    print(f"USERLAND-HEADER-T001 canonical headers: PASS ({len(paths)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
