# Queue q109: opt-in existing-data writeback

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes, record findings and continue.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p018](ws025-io-memory-cache/phase018-data-writeback/phase.md) | completed | Add per-mount opt-in, pre-lease dirty credits, coherent delayed data and independent device syncers. p017 and p019 complete. |

M/W/P, q108 evidence and current file/content/cache/page-sync and mount controls
were inspected. Implement the selected staged design in the P book; keep the
through default and defer new allocation/metadata delay to p021.
No commit, aggregate make check or .internal access. Serialize builds/tests;
make -j16 and disposable QEMU images. Physical gate remains user-accepted.

Previous: [q108](queue-q108.md), owned asynchronous BIO complete.
Next dependency-ready work: p020 readahead, p022 exec sharing and p023 kernel vmap.

Progress checkpoint: credit and coherent VM delayed transaction primitives,
UFS/FAT allocated-range admission queries, bounded batched page sync and
allocation-free mount drain are implemented. Host ordinary/sanitizer and native
512 MiB cache gates pass. Native verification exposed overlay barrier reentry;
filesystem-only internal barriers now preserve VM ordering and public observers.
Mount policy/file_io hookup/device workers remain pending; p018 is not complete.

Physical-domain checkpoint: nested loop/partition resolution now retains explicit
lifecycle pins and shares the real physical leaf. Host ordinary/sanitizer and
amd64 build pass; worker/control ownership refinement is recorded in p018.
q109 remains in-progress with policy/workers/file_io integration still required.

Policy/file_io checkpoint: bounded physical syncers, retained resources on failed
off, pre-lease admission and coherent delayed public file I/O are implemented.
Focused ordinary/sanitizer tests and default-off native cache regression pass.
User control/status, transactional unmount/shutdown and native opt-in acceptance
remain; phase and WS completion are not claimed.

90-minute review: root-only versioned control/status and CLI are implemented;
focused ordinary/sanitizer controls pass. Opt-in native NVMe acceptance passes
(batch 1 backend write vs 30 with per-write fsync, age drain and remount bytes).
The identical 64 MiB image fails USB mount EIO before opt-in. Investigate the
USB initial media-error latch before continuing lifecycle integration. q109
continues under existing autonomous authorization; p018 remains incomplete.

USB checkpoint: isolated initial TEST UNIT READY attention latch caused EIO before
mount; production BOT host reproduction failed before the fix and passes after.
Ordinary/sanitizer USB reserves and no-medium tests pass. Native opt-in USB and
NVMe both pass batch/each-fsync/age/coherence/disable/remount. Diagnostic link was
removed and ordinary amd64 restored. Remaining: policy-preserving unmount and
shutdown integration, synchronous-open durability, complete phase regressions.

Unmount checkpoint: reversible policy tokens are integrated into public/private
VFS teardown reference checks and failure rollback. Policy ordinary/sanitizer,
frontier regression and native USB enabled-unmount tests pass, including open-fd
EBUSY preserving LIVE policy, later success and remount contents. amd64, pcat and
pc98 builds pass; restoring amd64 after architecture gates. Shutdown, synchronous
open completion, injected VFS prepare failure and full regressions remain.

Shutdown checkpoint: storage admission closure/drain now precedes device teardown;
errors restore policy and propagate through halt/reboot, and init retries its
pending action. Focused policy ordinary/sanitizer and endpoint-order failure/retry
checks pass; amd64 build passes. Native shutdown and concurrent preparation,
synchronous-open completion, VFS teardown fault and final regression gates remain.

Native/synchronous checkpoint: init reaches observed clean storage boundary and
real halt; retained image bytes verify with explicit runtime UFS backup-summary
handling (original strict-reader failure retained). O_SYNC/O_DSYNC open and
checked whole-operation completion now pass host failure/retry/chunk tests and
native USB write/pwrite/writev; full native unmount sequence still passes.
Remaining concurrent shutdown, VFS preparation fault, coverage map and final
regressions prevent phase completion. Ordinary kernel has no diagnostic wrappers.

Final: p018 completed; coverage and retained failures/corrections recorded in its results.
Next dependency-ready selection is p020 bounded asynchronous readahead.
