#!/usr/bin/env python3
"""Check the retained workload samples and emit named, reproducible summaries."""
import json
import math
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[3]
path = Path(sys.argv[1]).resolve()
path.relative_to(REPO / "plan/ws025-io-memory-cache/temp")
text = (path / "guest.log").read_text().replace("\r", "")
header = (REPO / "include/uapi/zedbsd/io-stats.h").read_text()
names = re.findall(r"^\s*(IO_[A-Z0-9_]+),", header, re.M)
# Event IDs only append. Retained version-one samples still use the same prefix.
counts = {len(re.findall(r" (\d+):(\d+):(\d+)", tail))
          for tail in re.findall(r"^SAMPLE mode=\d+ n=\d+ ns=\d+(.*)$", text, re.M)}
assert len(counts) == 1
observed_count = counts.pop()
assert 0 < observed_count <= len(names)
names = names[:observed_count]
ids = {name: i for i, name in enumerate(names)}
result = {}
for mode in range(2):
    rows = []
    for number, ns, tail in re.findall(
            rf"^SAMPLE mode={mode} n=(\d+) ns=(\d+)(.*)$", text, re.M):
        counters = [(int(e), int(c), int(b)) for e, c, b in
                    re.findall(r" (\d+):(\d+):(\d+)", tail)]
        assert [e for e, _, _ in counters] == list(range(len(names)))
        events = {names[e]: (c, b) for e, c, b in counters}
        assert events["IO_SYSCALL_WRITE"] == (4, 262144)
        ufs_write = "IO_UFS_CONTENT_WRITE" if "IO_UFS_CONTENT_WRITE" in events else "IO_UFS1_CONTENT_WRITE"
        assert events[ufs_write][1] == 262144
        assert events["IO_COMPLETE_ERROR"] == (0, 0)
        if "IO_POOL_BACKING_ALLOC" in events:
            assert events["IO_POOL_BACKING_ALLOC"] == (0, 0)
            assert events["IO_POOL_LARGE_BORROW"] == (4, 262144)
            assert events["IO_POOL_RETURN"] == (4, 262144)
            assert events["IO_POOL_SMALL_BORROW"] == (0, 0)
            assert events["IO_POOL_FALLBACK"] == (0, 0)
        if "IO_USB_TRANSFER_RESERVE_ALLOC" in events:
            for name in ("IO_USB_TRANSFER_RESERVE_ALLOC", "IO_USB_TRANSFER_RESERVE_FREE",
                         "IO_XHCI_REQUEST_ALLOC", "IO_DMA_ALLOC", "IO_USB_BUFFER_ALLOC"):
                assert events[name] == (0, 0), (name, events[name])
        # Both the loop leaf and physical USB leaf are in driver bytes.
        assert events["IO_DRIVER_WRITE"][1] - events["IO_LOOP_WRITE"][1] == events["IO_USB_WRITE10"][1]
        rows.append((int(number), int(ns), events))
    assert sorted(n for n, _, _ in rows) == list(range(101))
    times = sorted(ns for _, ns, _ in rows)
    quantiles = {f"p{p}_ns": times[math.ceil(len(times) * p / 100) - 1]
                 for p in (50, 95, 99)}
    result[mode] = {"samples": len(rows), **quantiles, "events": {
        name: {"calls_min": min(row[2][name][0] for row in rows),
               "calls_max": max(row[2][name][0] for row in rows),
               "bytes_min": min(row[2][name][1] for row in rows),
               "bytes_max": max(row[2][name][1] for row in rows)}
        for name in names}}
(path / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({mode: {k: v for k, v in summary.items() if k != "events"}
                  for mode, summary in result.items()}, indent=2))
print("WS025 sample oracle PASS")
