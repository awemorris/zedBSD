#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Runs the static reftests of the fixed selection whose ID starts with a prefix, and shows what changed.

  static-subset.py [--base DIR] [--out DIR] PREFIX...      (PREFIX like /css/css-grid/abspos/)

--base is a measured directory (default: the newest build/tmp/p8/cur, which links the last full
measurement) whose static/report.json is the reference: IDs that passed there and fail now are
printed as REGRESSION; IDs that failed there and pass now as FIXED.  Only the subset is rendered.
"""
import argparse, json, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "plan/ws074/phase173/evidence/20261013-q1012/manifest.json"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=str(ROOT / "build/tmp/p8/cur"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--program", default=str(ROOT / "build/ws074-host/plain/browser"))
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("prefixes", nargs="+")
    a = ap.parse_args()
    m = json.loads(MANIFEST.read_text())
    sel = [r for r in m["selected"] if any(r["id"].startswith(p) for p in a.prefixes)]
    out = Path(a.out or ROOT / ("build/tmp/p8/sub-%d" % time.time()))
    out.mkdir(parents=True, exist_ok=True)
    # A snapshot of the program, so that a rebuild during the run does not change it.
    import shutil
    snap = out / "bin"
    snap.mkdir(exist_ok=True)
    src = Path(a.program).parent
    shutil.copy2(a.program, snap / "browser")
    shutil.copy2(src / "libbrowser.so", snap / "libbrowser.so")
    a.program = str(snap / "browser")
    base = {}
    rp = Path(a.base) / "static/report.json"
    if rp.is_file():
        base = {"/" + r["test"]: r["status"] for r in json.loads(rp.read_text())["results"]}
    # split into shards run in parallel
    shards = [sel[i::a.jobs] for i in range(a.jobs)]
    procs = []
    for i, s in enumerate(shards):
        if not s:
            continue
        mf = dict(m); mf["selected"] = s
        (out / f"m{i}.json").write_text(json.dumps(mf))
        procs.append(subprocess.Popen([sys.executable, str(ROOT / "plan/ws074/tests/run-wpt-reftests.py"),
            "--program", a.program, "--root", ".", "--selection-manifest", str(out / f"m{i}.json"),
            "--limit", "0", "--render-timeout", "30", "--save-failures", "0", "--out", str(out / f"s{i}")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for p in procs:
        p.wait()
    now = {}
    for i in range(a.jobs):
        r = out / f"s{i}/report.json"
        if r.is_file():
            for x in json.loads(r.read_text())["results"]:
                now["/" + x["test"]] = x["status"]
    npass = sum(1 for v in now.values() if v == "pass")
    print("subset", len(now), "pass", npass, "(was", sum(1 for k in now if base.get(k) == "pass"), ")")
    for k, v in sorted(now.items()):
        if base.get(k) == "pass" and v != "pass":
            print("REGRESSION", k, v)
    for k, v in sorted(now.items()):
        if base.get(k) != "pass" and v == "pass":
            print("FIXED", k)
main()
