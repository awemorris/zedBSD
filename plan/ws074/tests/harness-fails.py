#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Lists the failing subtests (and messages) of the harness IDs in a gap that match a prefix.

  harness-fails.py --gap gap.json --ours DIR [--max N] PREFIX...
"""
import argparse, json, glob
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument("--gap", required=True)
ap.add_argument("--ours", required=True)
ap.add_argument("--max", type=int, default=3)
ap.add_argument("prefixes", nargs="+")
a = ap.parse_args()
gap = json.loads(Path(a.gap).read_text())["gap"]["harness"]
want = [i for i in gap if any(i.startswith(p) for p in a.prefixes)]
records = {}
for f in glob.glob(a.ours + "/harness/shard-*/test-*.json"):
    try:
        d = json.loads(Path(f).read_text())
    except Exception:
        continue
    tid = d.get("test_id") or d.get("id")
    if tid:
        records[tid] = d
for tid in want:
    d = records.get(tid)
    if d is None:
        print(tid, "(no record)")
        continue
    fails = [r for r in d.get("result_callbacks", []) if r.get("status") != 0]
    print("%s  outcome=%s  fails=%d/%d" % (tid, d.get("outcome"), len(fails), len(d.get("result_callbacks", []))))
    for r in fails[:a.max]:
        print("    %s: %s" % (r.get("name"), (r.get("message") or "")[:160].replace("\n", " ")))
    if d.get("stderr"):
        print("    stderr:", d["stderr"][:160].replace("\n", " "))
