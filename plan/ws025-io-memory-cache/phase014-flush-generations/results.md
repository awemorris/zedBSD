# ws025-p014 execution record

Status: completed, Queue q099.

Initial implementation adds embedded outstanding-write ordering and
accepted/completed frontiers in disk/BIO state. Completion removes references to
caller BIO storage and finishes leaf inflight/reference accounting before
publishing COMPLETED. Submission refusal also retires its accepted write.
Errors and short writes invalidate the persistence epoch. No flush elision is
implemented or claimed yet.

An initial amd64 frontier build is running (`../temp/p014/build-frontier.log`).
No tests have passed for p014 yet. Pending: lock/lifetime review and focused
regressions, target/epoch-aware flush serialization, explicit driver eligibility
and reset hooks, upper logical dirty integration, full supported builds and native
acceptance. Do not mark this intermediate frontier adapter complete.

## Integration progress

Implemented target-aware serialized flush ownership. Only DISK_FLUSH_PROOF drivers
can reuse a successful proof; unknown/stacked devices forward every flush. USB
sets eligibility from its existing cache/flush policy and invalidates before BOT
reset. Disk removal invalidates ancestry proof. NVMe and other drivers retain
ordinary flush until their reset integration establishes eligibility.

Added a separate saturating 64-bit logical epoch in mount state, marked by generic
file writes and FAT/UFS/overlay metadata owners before lower I/O. Mount sync always
runs the FS callback and records only its captured target after success. A failure
marks a new unresolved generation. No upper drain is elided from leaf evidence.

Completed bookkeeping precedes terminal publication; callback-owned BIOs reject
bio_wait. Cache admission now also holds a leaf reference until leave, covering
physical-I/O-free proof reuse and post-I/O accounting against device destruction.

Evidence so far:

- `build-frontier.log`, `build-integration.log`: amd64 builds PASS at their
  respective source snapshots (final lifetime-pin change needs final build).
- `storage-foundation-initial.log`: existing disk/partition/mount/ABI PASS.
- `frontier-thread-host/`: ordinary and ASan/UBSan PASS; out-of-order write holes,
  new write during flush, same-target repeated and four concurrent fsync callers,
  unknown-policy non-elision, short/error/reset/epoch exhaustion, terminal wake
  lifetime, callback frees its BIO, upper dirty/no-new-leaf-write and sync failure.
- `usb-reserve-host-2/`: core, xHCI and storage reservation regressions PASS in both
  variants. First storage fixture link failed because its new disk-reset boundary
  mock was inside a legacy-only conditional; moved the mock to the shared boundary.

Still pending: final focused lifecycle rerun, broader FS/overlay/syscall regression,
USB recovery policy assertions, final three builds, native acceptance/counters and
M/W/P/Q synchronization. This phase remains in-progress.

## Logical owner review correction

A dirty counter alone could label a captured generation stable while its writer
had marked dirty but had not yet reached BIO submission. Added active logical
ownership: generic file backend calls, UFS writes including snapshot/journal work,
overlay journal append/compaction, and FAT's retained mutable sector hold an active
reference across their operation. Successful mount sync publishes no logical
stable target while any such owner remains active. This is conservative bookkeeping;
it does not skip or weaken the existing filesystem sync callbacks.

`frontier-owner-host/` passes ordinary and sanitizer variants, including an active
upper owner across mount_sync and later completion. `build-logical-owner.log` passes.
The final broad host regression (`acceptance-final-host.log`, source/commands in
`plan/ws018-kernel-architecture/temp/ws025-p014-final/`) passes all 39 host storage
items plus all 30 WiFi scenarios in both variants. Its exit 1 denotes only the
11 not-yet-run native items; no host test failed.

## Final acceptance

All required gates are complete. Earlier pending descriptions above are retained
as execution history; this section is the final state.

- `ufs-owner-host/`: both UFS drivers' metadata/allocation rollback gates PASS,
  ordinary and sanitizer, after logical owner integration.
- `bot-final/`: actual BOT engine recovery tests PASS, both variants; every class
  reset calls persistence invalidation for its disk exactly once.
- `build-pcat.log`, `build-pc98.log`, `build-amd64-final-owner.log`: final supported
  builds PASS. No test-only kernel wrapper remains.
- `native-acceptance.log`: two grouped QEMU boots PASS, including saved settings,
  shared-map/file coherence, overlay journal/rename/directory sync and reboot.
  `acceptance-final.json` combines storage 50/50 and WiFi 30/30 in each variant.
- `native-16g-aligned/`: 202 repeated write/fsync/readback samples PASS with source
  image hash unchanged, UEFI/xHCI/16 GiB. Both modes p50/p95/p99=10/20/20 ms.
  Compared with p012's controlled 4 KiB alignment, total driver flushes fall from
  6 to 5; loop flushes remain 3. The single physical USB leaf therefore changes
  from 3 to 2 flushes. This preserves distinct upper work/commit boundaries;
  it is not an unconditional one-flush claim or measured latency improvement.
- Final source hashes: `final-source.sha256.json`. `git diff --check`: PASS.

DISK_FLUSH_PROOF is enabled for USB's established write-through, FUA, or working
SYNCHRONIZE CACHE policy. Other physical drivers and stacked disks retain ordinary
flush behavior until their reset/persistence capability is explicitly integrated.
Deferred data and sticky dirty-error policy remain p017/p018 work; active logical
owners prevent this stage from publishing optimistic upper stable generations.
No commit, real-device test, or whole-WS completion is claimed by this phase.
