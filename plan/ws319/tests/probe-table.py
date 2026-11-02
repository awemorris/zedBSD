#!/usr/bin/env python3
# Makes the per-stream table of a run-5330-video.sh probe part: one Markdown row per HEVC stream of HEVC/list.txt with
# its class, the reference's frames, vkvideo-probe's exit, frames decoded, output and matched, and the verdict (MATCH:
# every reference frame output and equal; DIFFER, REFUSED, FAIL), then the counts.
#   python3 -I plan/ws319/tests/probe-table.py RUN/probe.txt HEVC/list.txt RUN/m
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import sys


def main():
    probe_path, list_path, m_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    lines = {}
    for line in open(probe_path):
        parts = line.split()
        if len(parts) > 2 and parts[0] == "probe":
            lines[parts[1]] = line.strip()
    counts = {}
    print("| stream | class | reference | exit | decoded | output | matched | verdict |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for row in open(list_path):
        fields = row.rstrip("\n").split("\t")
        if not fields or not fields[0]:
            continue
        name, klass = fields[0], fields[1] if len(fields) > 1 else ""
        line = lines.get(name, "")
        exit_match = re.search(r"exit=(\d+)", line)
        reference = re.search(r"reference=(\d+)", line)
        decoded = re.search(r"(\d+) frames decoded", line)
        output = re.search(r"(\d+) output", line)
        matched = re.search(r"(\d+) match the reference", line)
        refused = ""
        try:
            text = open("%s/%s.txt" % (m_dir, name), errors="replace").read()
            found = re.search(r"refus\w*[^\n]*", text)
            if found:
                refused = found.group(0)[:60]
        except OSError:
            text = ""
        ref = int(reference.group(1)) if reference else 0
        dec = int(decoded.group(1)) if decoded else 0
        mat = int(matched.group(1)) if matched else 0
        out = int(output.group(1)) if output else 0
        if not line:
            verdict = "NOT RUN"
        elif refused and mat == 0:
            verdict = "REFUSED (%s)" % refused
        elif ref > 0 and mat == ref and out == ref:
            verdict = "MATCH"
        elif dec > 0:
            verdict = "DIFFER"
        else:
            verdict = "FAIL"
        key = verdict.split(" ")[0]
        counts[key] = counts.get(key, 0) + 1
        print("| %s | %s | %d | %s | %d | %d | %d | %s |" % (name, klass, ref, exit_match.group(1) if exit_match else "-", dec, out, mat, verdict))
    print()
    print("counts: " + ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts)))


main()
