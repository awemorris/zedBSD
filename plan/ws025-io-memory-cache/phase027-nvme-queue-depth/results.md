# q139 progress: bounded NVMe BIO pipeline

Date: 2026-09-09
Status: completed (q139).

## Implementation

`src/drivers/pci/pci-nvme.c` reuses existing command slots, CID/epoch checking,
per-slot bounce DMA and the BIO admission frontier. One data BIO holds at most
four chunks by default; a private compile-time override exercises depths 1/2/4/8
through the same code. No second queue or buffer ownership protocol was added.

Filling the window never blocks for another slot while this BIO has commands
it can reap. Completion interrupts still resolve arbitrary order by CID. The
caller reaps logical order, reports/copies only the successful prefix, stops
issuing after an observed failure and drains all its slots before entering
controller recovery. Writes beyond a failed chunk may already have reached the
device; the returned byte count conservatively reports the contiguous successful
prefix, not a rollback guarantee. External and internal FLUSH retain the same
admission and recovery machinery. Controller-local lifetime post/high-water
counters can be read under command_lock or with all QEMU CPUs stopped; no public
diagnostic command or ABI was added.

## Completed verification

- Whole-driver host fixture injects only MMIO writes, waiting and hardware
  recovery. Slot allocation/release, lifecycle, CID lookup, CQ processing,
  pipeline and BIO admission/completion are the production functions.
- `temp/q139-pipeline-host-6`: depth 1/2/4/8 ordinary and ASan/UBSan PASS,
  respectively 11042/11533/12217/13105 checks per mode. Covers ordered/reverse CQ,
  one/two/four/eight available slots, repeated ring reuse, short final transfer,
  reads/writes/FLUSH, an error at each of the first nine chunks, post and pre-post
  failure, timeout, foreign CID, recovery/retry, waiting for an external owner,
  prior-data/FLUSH and active-FLUSH/later-data exclusion.
- The host recovery boundary verifies zero software-owned slots before simulating
  hardware quiescence. This does not replace actual controller reset validation.
- Explicit `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk`
  passed for all three platforms. Logs: `/tmp/zedbsd-q139-{amd64,pcat,pc98}.log`.
  User `config.mk` was not changed.

Initial host attempts exposed fixture setup omissions (maximum transfer size,
stop-before-quiesce) and ASan retaining unrelated registration tables. Corrected
fixtures preserve these failed attempts under `temp/q139-pipeline-host-1` through
`-5`; no production bug fix is attributed to those fixture failures. The final
sanitizer run disables ASan global registration so unrelated hardware entry
points can be garbage-collected, retaining address/UB checks in exercised code.

## Pending acceptance

`tests/run-nvme-pipeline-qemu.py` builds private depth variants and runs a 64 KiB
raw-I/O helper on a disposable sparse 5 GiB NVMe namespace: below/above 4 GiB,
96-command sequential and four-process concurrent write/fsync/readback, followed
by controller restart and verification. Stopped QMP snapshots use the target
controller layout and each image's own kernel symbol address. Current run:
`temp/q139-qemu`. Do not mark this phase complete before reviewing its outcome.
QEMU results do not establish physical throughput or latency improvements.

### QEMU fixture correction

The first `temp/q139-qemu` campaign passed all raw read/write/restart oracles at
1/2/4 and raw first-boot oracles at 8, but failed depth-8 high-water acceptance.
All depths had high-water 4: `devfs` submits raw transfers a sector at a time,
so four concurrent processes alone explained that number. Those passes do not
prove this phase's single-BIO pipeline. No production data failure was observed.

The maintained fixture now also links a test-only `disk_ioctl` wrapper, as in
the earlier native BIO tests. It invokes normal `disk_write_direct`, `disk_sync`
and `disk_read_direct` with 128-sector requests, below/above 4 GiB, and checks
full patterns. It leaves normal admission/claims/driver dispatch intact. QMP
checks exact command deltas (32 * (16 write + 1 FLUSH + 16 read) = 1056;
restart verification 32 * 16 = 512) before the separate raw/concurrent workload.
No wrapper or private ioctl is installed in ordinary builds. A first wrapper
build (`temp/q139-qemu2`) failed due to incorrect test API names; corrected to
`atomic_compare_exchange` and `disk_sync`. Current corrected campaign is
`temp/q139-qemu3`.

The corrected direct-BIO fixture (`temp/q139-qemu3`) then exposed a production
integration omission: `d_max_transfer_blocks` still advertised the 4 KiB command
limit, so `disk_transfer_direct` split before driver dispatch. q139 now publishes
`KERN_IO_BATCH_MAX / block_size` (64 KiB BIOs), retaining MDTS/single-PRP/4 KiB
limits on each internal command. The whole-driver host suite passes again in
`temp/q139-pipeline-host-7`; `temp/q139-qemu4` validates the actual large BIO path.
Initial native snapshots now show 1056 commands and outstanding high-water 1/2
for depths 1/2 respectively, rather than mistaking cross-process concurrency for
single-BIO pipelining. Final three-platform builds will follow this integration
change. Raw devfs sector-by-sector batching is a separate remaining performance
observation, not changed by this phase.

## Final acceptance

| Per-BIO depth | Write/FLUSH/read commands | Restart read commands | Native outstanding high-water (both boots) | Evidence |
| --- | --- | --- | --- | --- |
| 1 | 1056 | 512 | 1 | temp/q139-qemu4/d1 |
| 2 | 1056 | 512 | 2 | temp/q139-qemu4/d2 |
| 4 | 1056 | 512 | 4 | temp/q139-qemu4/d4 |
| 8 | 1056 | 512 | 8 | temp/q139-qemu5/d8 |

Each cell additionally passed the raw-devfs 64 KiB helper below/above 4 GiB,
96 sequential requests and four-process concurrent requests with fsync, then
all verification modes after restarting the QEMU controller. Source disk hashes
remained unchanged. q139-qemu4 depth 8's native write passed with high-water 7
but the original oracle required 8, incorrectly rejecting normal early CQ
completion. The corrected oracle permits overlap within the selected bound;
only depth 8 was rerun, in q139-qemu5, where both full boots passed (wrapper 0).

Final explicit amd64/pcat/pc98 builds passed after publishing the 64 KiB BIO
limit: /tmp/zedbsd-q139-final-{amd64,pcat,pc98}.log. Ordinary builds have no
private ioctl wrapper or depth override. temp/q139-writeback passed the existing
NVMe UFS native writeback, synchronous/overlay write, each-fsync, age flush,
unmount/remount and verification gates; final dirty/reserved/tickets/memory and
errors are zero (wrapper 0). Host ordinary/sanitizer depth gates passed on the
final source in temp/q139-pipeline-host-7.

The default window is 4 to bound one BIO's share of the existing 63 slots; this
is not a physical performance claim. Physical optimum/latency measurements and
raw-devfs batching remain separate observations, not failed functional gates.

Final NVMe source SHA256: `afc7be380a3acb42b4ec52a28f126a55f6e0a45abbf2ed759d78f598cba53528`.
