#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Prints the IDs that entered or left the gap between two gap JSON files (interop2025-chrome-gap.py compare --write).

  gap-diff.py OLD.json NEW.json
"""
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
print("ours %d -> %d of %d" % (a["ours"], b["ours"], b["target"]))
for k in a["gap"]:
    old, new = set(a["gap"][k]), set(b["gap"].get(k, []))
    print("%-9s regressions %d, fixed %d" % (k, len(new - old), len(old - new)))
    for i in sorted(new - old):
        print("  REGRESSION", i)
