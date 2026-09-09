# q142: bootstrap retirement and lifecycle baseline

Date: 2026-09-09
Status: current regression corrected; historical provenance uncleared.

The fixture sampled a transient bootstrap owner. `src/kern/main.c` starts PID 1
from a detached ordinary kernel thread and then returns. The original fixture
could start child comparisons before this unrelated lifetime ended.

The maintained fixture now observes all original owners plus PID 0 thread count
before creating any child. It requires 25 equal samples spaced by 20 ms, bounded
to 250 attempts. Every transition is logged. No owner tolerance or baseline
adjustment is permitted once the 100 child cycles begin.

Both PC98 and three PCAT repeat boots show the decrease **before any fork**:
PID 0 threads 8 to 7, total thread/task owners 9 to 8, stack bytes 131072 to
114688. Process, filedesc, file and VM owners do not change. This establishes
that the decrease is independent of getty teardown and matches the one-shot
boot-worker lifetime. No retiring thread instruction pointer was recorded.

| Evidence below `plan/ws002-services/temp/` | Result |
| --- | --- |
| `q142-lifecycle-pcat2/result.json` | 100/100 exact comparisons; bootstrap already retired at first sample |
| `q142-lifecycle-pcat3/result.json`, `run*/debugcon.log` | Three boots, 300/300; all show the pre-child kernel-owner transition |
| `q142-lifecycle-pc98/result.json`, `run0/screen.log` | 100/100; pre-child kernel-owner transition captured |

Fixture builds used explicit PCAT/PC98 CI configurations and `make -j16`.
Ordinary disk hashes are unchanged before/after every accepted run.
`q142-lifecycle-pcat` was an infrastructure failure: sandbox denied the local
monitor socket before boot. Approved socket-enabled reruns replace it.
No production code changed in q142; q141 three-platform builds and heap/USB,
halt and normal-session regressions remain applicable.

p024 is now complete. The original p021 historical PC98 invalid allocation free
still has no exact allocation provenance; present-day success does not prove
its cause. Preserve that item as uncleared.
