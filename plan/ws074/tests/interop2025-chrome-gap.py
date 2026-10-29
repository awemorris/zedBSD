#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Measure the fixed Interop 2025 selection with browser or Chromium, and compare the two.

  interop2025-chrome-gap.py measure --kind ours|chrome --out DIR [--build DIR] [--jobs N] [--only KIND,...]
  interop2025-chrome-gap.py compare --chrome DIR --ours DIR [--write FILE]

measure runs the four runners of the selection into DIR: harness/ (testharness pages,
run-interop2025-harness-all.py), static/ (script-free reftests, run-wpt-reftests.py),
scripted/ (scripted reftests, run-interop2025-scripted-reftests.py) and crash.json
(crashtests, run-interop2025-crashtests.py).  --kind ours uses browser and wpt-ready-render
from --build (default build/ws074-host/plain); --kind chrome uses chrome-run.js, with one
shared Chromium (chrome-run.js --server) started for the run.

compare reads two such directories and prints, per runner, how many IDs Chromium passes
and how many of those browser passes; the goal is every ID Chromium passes.  --write keeps
the per-ID gap (Chromium PASS, browser not) as JSON.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[3]
TESTS = Path(__file__).resolve().parent
MANIFEST = ROOT / "plan/ws074/phase173/evidence/20261013-q1012/manifest.json"
KINDS = ("harness", "static", "scripted", "crash")


def start_chrome(directory):
    """Starts one shared Chromium; the runs connect to it through CHROME_RUN_WS."""
    endpoint = directory / "chrome-server.ws"
    log = (directory / "chrome-server.log").open("w")
    server = subprocess.Popen(["node", str(TESTS / "chrome-run.js"), "--server", str(endpoint)],
                              stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        if endpoint.is_file() and endpoint.read_text().strip():
            break
        time.sleep(0.1)
    else:
        server.kill()
        raise RuntimeError("the shared Chromium did not start")
    os.environ["CHROME_RUN_WS"] = endpoint.read_text().strip()
    return server


def run(command, log):
    """Runs one runner with its output in log; the runners' own exit codes are recorded."""
    with log.open("w") as output:
        started = time.monotonic()
        code = subprocess.run([str(c) for c in command], stdout=output, stderr=subprocess.STDOUT).returncode
    return {"command": [str(c) for c in command], "returncode": code,
            "seconds": round(time.monotonic() - started, 1)}


def measure(args):
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    only = set(args.only.split(",")) if args.only else set(KINDS)
    if args.kind == "chrome":
        program = TESTS / "chrome-run.js"
        ready = program
        server = start_chrome(out)
    else:
        program = args.build / "browser"
        ready = args.build / "wpt-ready-render"
        server = None
    runs = {}
    try:
        if "harness" in only:
            command = [sys.executable, TESTS / "run-interop2025-harness-all.py", "--manifest", MANIFEST,
                       "--program", program, "--out", out / "harness", "--jobs", args.jobs]
            if args.kind == "chrome":
                command += ["--process-timeout", "30"]
            else:
                command += ["--font-dir", ROOT / "userland/desktop/fonts"]
            runs["harness"] = run(command, out / "harness.log")
        if "static" in only:
            runs["static"] = run([sys.executable, TESTS / "run-wpt-reftests.py", "--program", program,
                                  "--root", ".", "--selection-manifest", MANIFEST, "--limit", "0",
                                  "--render-timeout", "30", "--save-failures", "0", "--out", out / "static"],
                                 out / "static.log")
        if "scripted" in only:
            runs["scripted"] = run([sys.executable, TESTS / "run-interop2025-scripted-reftests.py",
                                    "--manifest", MANIFEST, "--program", ready, "--out", out / "scripted",
                                    "--budget-seconds", "5400"], out / "scripted.log")
        if "crash" in only:
            runs["crash"] = run([sys.executable, TESTS / "run-interop2025-crashtests.py", "--manifest", MANIFEST,
                                 "--program", program, "--focus", "all", "--settle-ms", "5000",
                                 "--process-timeout", "30", "--out", out / "crash.json"], out / "crash.log")
    finally:
        if server is not None:
            server.kill()
    previous = {}
    if (out / "runs.json").is_file():
        previous = json.loads((out / "runs.json").read_text())
    previous.update(runs)
    (out / "runs.json").write_text(json.dumps(previous, indent=2) + "\n")
    for name, record in runs.items():
        print("%s: exit %s in %.0f s" % (name, record["returncode"], record["seconds"]))
    return 0


def outcomes(directory):
    """Reads each runner's per-ID outcome from one measured directory."""
    found = {}
    path = directory / "harness/summary.json"
    if path.is_file():
        found["harness"] = json.loads(path.read_text())["outcomes"]
    path = directory / "static/report.json"
    if path.is_file():
        found["static"] = {"/" + r["test"]: r["status"] for r in json.loads(path.read_text())["results"]}
    path = directory / "scripted/report.json"
    if path.is_file():
        found["scripted"] = {r["test_id"]: r["outcome"] for r in json.loads(path.read_text())["results"]}
    path = directory / "crash.json"
    if path.is_file():
        found["crash"] = {r["test_id"]: r["outcome"] for r in json.loads(path.read_text())["results"]}
    return found


def compare(args):
    chrome = outcomes(args.chrome)
    ours = outcomes(args.ours)
    selected = json.loads(MANIFEST.read_text())["selected"]
    gap = {}
    total_target = total_ours = 0
    print("%-9s %8s %8s %8s %8s" % ("runner", "measured", "chrome", "ours", "gap"))
    for kind in KINDS:
        if kind not in chrome or kind not in ours:
            print("%-9s (not measured on both)" % kind)
            continue
        target = sorted(i for i, o in chrome[kind].items() if o == "pass")
        passed = [i for i in target if ours[kind].get(i) == "pass"]
        gap[kind] = [i for i in target if ours[kind].get(i) != "pass"]
        total_target += len(target)
        total_ours += len(passed)
        print("%-9s %8d %8d %8d %8d" % (kind, len(chrome[kind]), len(target), len(passed), len(gap[kind])))
    measured = set()
    for kind in chrome:
        measured.update(chrome[kind])
    print("%-9s %8d %8d %8d %8d   (%.2f%% of Chromium's passes)" % (
        "total", len(measured), total_target, total_ours, total_target - total_ours,
        100.0 * total_ours / total_target if total_target else 0.0))
    print("unmeasured by any runner: %d of %d selected IDs" % (len({r["id"] for r in selected} - measured),
                                                               len(selected)))
    if args.write:
        args.write.write_text(json.dumps({"target": total_target, "ours": total_ours, "gap": gap},
                                         indent=2) + "\n")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("measure")
    one.add_argument("--kind", choices=("ours", "chrome"), required=True)
    one.add_argument("--out", type=Path, required=True)
    one.add_argument("--build", type=Path, default=ROOT / "build/ws074-host/plain")
    one.add_argument("--jobs", type=int, default=3)
    one.add_argument("--only")
    two = commands.add_parser("compare")
    two.add_argument("--chrome", type=Path, required=True)
    two.add_argument("--ours", type=Path, required=True)
    two.add_argument("--write", type=Path)
    args = parser.parse_args()
    if args.command == "measure":
        return measure(args)
    return compare(args)


if __name__ == "__main__":
    sys.exit(main())
