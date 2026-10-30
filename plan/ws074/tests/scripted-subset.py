#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Runs the scripted reftests of the fixed selection whose ID starts with a prefix, and shows what changed.

  scripted-subset.py [--base DIR] [--out DIR] PREFIX...      (PREFIX like /css/css-view-transitions/)

--base's scripted/report.json is the reference (default build/tmp/p8/cur): REGRESSION is a pass there that fails
now, FIXED the other way.  The program is copied first, so a rebuild during the run does not change it.
"""
import argparse, json, shutil, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "plan/ws074/phase173/evidence/20261013-q1012/manifest.json"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=str(ROOT / "build/tmp/p8/cur"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--build", default=str(ROOT / "build/ws074-host/plain"))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("prefixes", nargs="+")
    a = ap.parse_args()
    m = json.loads(MANIFEST.read_text())
    sel = [r for r in m["selected"] if any(r["id"].startswith(p) for p in a.prefixes)]
    out = Path(a.out or ROOT / ("build/tmp/p8/ssub-%d" % time.time()))
    out.mkdir(parents=True, exist_ok=True)
    snap = out / "bin"; snap.mkdir(exist_ok=True)
    for n in ("browser", "wpt-ready-render", "libbrowser.so"):
        shutil.copy2(Path(a.build) / n, snap / n)
    base = {}
    rp = Path(a.base) / "scripted/report.json"
    if rp.is_file():
        base = {r["test_id"]: r["outcome"] for r in json.loads(rp.read_text())["results"]}
    procs = []
    for i in range(a.jobs):
        s = sel[i::a.jobs]
        if not s:
            continue
        procs.append(subprocess.Popen([sys.executable, str(ROOT / "plan/ws074/tests/run-interop2025-scripted-reftests.py"),
            "--manifest", str(MANIFEST), "--only", ",".join(r["id"] for r in s), "--program", str(snap / "wpt-ready-render"), "--out", str(out / f"s{i}"),
            "--budget-seconds", "5400"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for p in procs:
        p.wait()
    now = {}
    for i in range(a.jobs):
        r = out / f"s{i}/report.json"
        if r.is_file():
            for x in json.loads(r.read_text())["results"]:
                now[x["test_id"]] = x["outcome"]
    print("subset", len(now), "pass", sum(1 for v in now.values() if v == "pass"), "(was", sum(1 for k in now if base.get(k) == "pass"), ")")
    for k, v in sorted(now.items()):
        if base.get(k) == "pass" and v != "pass":
            print("REGRESSION", k, v)
    for k, v in sorted(now.items()):
        if base.get(k) != "pass" and v == "pass":
            print("FIXED", k)
main()
