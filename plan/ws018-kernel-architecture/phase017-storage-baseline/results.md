# ws018-p017 results

Date: 2026-09-06. Result: completed for the defined measurement/image scope.
Baseline commit 02eeed5; experimental kernels include q086's other corrections,
held constant across the four cells. These are not timings of an untouched
02eeed5 kernel. Host comparison also runs the retained original FAT path and
mapped path against the same production buffer cache and memory medium.

| Kernel syscall regular chunk | xHCI IMOD | QEMU 256KiB overwrite + final fsync |
| --- | --- | --- |
| 512 | 4000 | 440 ms |
| 4096 | 4000 | 70 ms |
| 512 | 0 | 450 ms |
| 4096 | 0 | 70 ms |

Actual four kernel builds and xHCI USB-root guests:
[AB results](../temp/q086-native-ab2/results.json).
Runner: `python3 plan/ws018-kernel-architecture/tests/run-storage-ab.py OUTPUT`.
Each cell preallocates/writes/syncs a 64KiB file, times four positional overwrites
and final fsync using guest CLOCK_MONOTONIC, then verifies content.
One sample per cell; guest timer granularity and QEMU scheduling limit precision.
No physical IRQ histogram/CPU measurement was performed. No evidence here supports
changing production IMOD from 4000; it remains 4000. Both experimental objects
are force-rebuilt with ordinary flags on runner exit.

Production syscall function fixtures: a 64KiB read/write makes 128 backend calls
at 512 versus 16 at 4096. IMOD labels cannot influence that host fixture.
Production FAT + loop + buf fixture: fragmented 8192-byte overwrite makes
16 memory-medium write calls through the original FAT path versus 4 with the map.
These are observed operation counts, not estimated hardware latency.

Current amd64 root and data images contain zero directories larger than one
UFS block. An actual zedimage-host fixture produces a 13312-byte root directory,
which the new audit detects. Read-only images report this limit; the optional
`--writable-directory-limit` rejects images intended for mutation.
F1 mutation support itself remains with WS024.

Evidence is also captured in the [50-story result](../phase019-storage-acceptance/results.md).

