#!/usr/bin/env python3
# ws035-p001: summary of the pre-refactor baseline build logs.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""usage: python3 plan/ws035/tests/baseline-summary.py [log-dir]

Reads plan/ws035/phase001/baseline/*.log (baseline-build.sh) and writes
summary.json beside them: exit status, seconds, compiler/linker diagnostics
(errors and warnings, de-duplicated by location and message), and the make
targets that failed.  Paths inside BUILD are shown relative to BUILD so a
post-refactor log can be compared with --compare-to after the same run.
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIAG = re.compile(r"^(?P<loc>[^\s:][^:]*:\d+(:\d+)?|ld\.lld|clang|[\w.-]*gcc|[\w.-]*ld): "
                  r"(?:fatal )?(?P<kind>error|warning): (?P<msg>.*)$")
FAILED = re.compile(r"^make: \*\*\* \[(?P<rule>[^\]]+)\] Error \d+")
NORULE = re.compile(r"^make: \*\*\* No rule to make target '(?P<target>[^']+)'")
TAIL = re.compile(r"^(?P<name>\S+) status=(?P<status>\d+) seconds=(?P<sec>\d+)")


def summarize(log):
    name = log.stem
    text = log.read_text(encoding="utf-8", errors="replace")
    strip = re.compile(r"build/ws035-p001/[^/\s]+/")
    errors, warnings, failed, norule = set(), set(), [], []
    status = sec = None
    for line in text.splitlines():
        m = DIAG.match(line)
        if m:
            item = strip.sub("$(BUILD)/", f"{m.group('loc')}: {m.group('msg')}")
            (errors if m.group("kind") == "error" else warnings).add(item)
            continue
        m = FAILED.match(line)
        if m:
            failed.append(strip.sub("$(BUILD)/", m.group("rule")))
            continue
        m = NORULE.match(line)
        if m:
            norule.append(m.group("target"))
            continue
        m = TAIL.match(line)
        if m and m.group("name") == name:
            status, sec = int(m.group("status")), int(m.group("sec"))
    head = text.splitlines()[:3]
    return {"name": name, "status": status, "seconds": sec,
            "command": head[2][2:] if len(head) > 2 else "",
            "errors": len(errors), "warnings": len(warnings),
            "failed_rules": sorted(set(failed)), "no_rule": sorted(set(norule)),
            "error_samples": sorted(errors)[:12], "warning_list": sorted(warnings)}


def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "plan/ws035/phase001/baseline"
    rows = [summarize(p) for p in sorted(d.glob("*.log"))]
    (d / "summary.json").write_text(json.dumps(rows, indent=1, ensure_ascii=False) + "\n")
    for r in rows:
        print(f"{r['name']:24} status={r['status']} errors={r['errors']} "
              f"warnings={r['warnings']} failed_rules={len(r['failed_rules'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
