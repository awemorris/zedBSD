#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Measure every eligible fixed Interop 2025 testharness ID, several shards at a time.

  run-interop2025-harness-all.py --manifest M --program P --out DIR [--jobs N] [--font-dir D]

Runs run-interop2025-census.py over consecutive shards of 128 eligible IDs, at most
--jobs at once, each into DIR/shard-OFFSET, then writes DIR/summary.json: the outcome of
every eligible ID and the counts.  --program is browser, or the Chromium adapter
chrome-run.js, which answers the same `--run` command line.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
SHARD = 128


def load_module(filename, name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wpt", type=Path, default=ROOT / "build/ws074-suites/wpt")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--settle-ms", type=int, default=5000)
    parser.add_argument("--process-timeout", type=int, default=20)
    parser.add_argument("--font-dir", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("invalid job count")

    census = load_module("run-interop2025-census.py", "interop_census")
    batch = load_module("run-interop2025-batch.py", "interop_batch")
    selection = json.loads(args.manifest.read_text(encoding="utf-8"))
    identifiers, reasons = census.choose(selection, args.wpt.resolve(), batch.candidate_reason)
    args.out.mkdir(parents=True, exist_ok=False)

    def shard(offset):
        command = [sys.executable, str(Path(__file__).with_name("run-interop2025-census.py")),
                   "--wpt", str(args.wpt), "--manifest", str(args.manifest),
                   "--program", str(args.program), "--out", str(args.out / ("shard-%04d" % offset)),
                   "--offset", str(offset), "--limit", str(SHARD),
                   "--settle-ms", str(args.settle_ms), "--process-timeout", str(args.process_timeout)]
        if args.font_dir:
            command += ["--font-dir", str(args.font_dir)]
        with (args.out / ("shard-%04d.log" % offset)).open("w") as log:
            return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode

    offsets = list(range(0, len(identifiers), SHARD))
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        codes = list(pool.map(shard, offsets))

    outcomes = {}
    for offset in offsets:
        report = args.out / ("shard-%04d" % offset) / "report.json"
        if not report.is_file():
            continue
        for row in json.loads(report.read_text(encoding="utf-8"))["results"]:
            outcomes[row["test_id"]] = row["outcome"]
    summary = {"program": str(args.program), "eligible": len(identifiers), "reason_counts": reasons,
               "shard_returncodes": codes, "missing": [i for i in identifiers if i not in outcomes],
               "outcome_counts": dict(sorted(Counter(outcomes.values()).items())),
               "outcomes": dict(sorted(outcomes.items()))}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
                                           encoding="utf-8")
    print("harness-all: %d eligible, %s" % (len(identifiers), summary["outcome_counts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
