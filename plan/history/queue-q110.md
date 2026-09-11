# Queue q110: bounded asynchronous readahead

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes, record findings and continue.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p018](../ws025/phase018/phase.md) | completed | Repair synchronous flags in the resolved-open adapter used by overlay; add stacked native regression discovered during p020 identity inspection. |
| 2 | [ws025-p020](../ws025/phase020/phase.md) | completed | Add bounded optional sequential prefetch with demand priority and stale-completion rejection. p016/p019 complete; reuse p018 coherent lifecycle. |

M/W/P, q109 final evidence, current VM coherent reads, file transactions and owned
BIO interfaces were inspected. Follow the selected staged design in the P book.
Important uncertainty is measured demand latency under non-preemptible device
transfers; reduce windows/admission if evidence requires it, never hide the cost.
No commit, aggregate make check or .internal access. Serialize builds/tests;
make -j16 and disposable QEMU images. Physical gate remains user-accepted.

Previous: [q109](queue-q109.md), opt-in existing-data writeback complete.
Next dependency-ready work: p021 metadata journal, p022 exec sharing, p023 vmap.

Checkpoint: private optional VM fills now capture lifetime/generation, remain
unpublished during I/O, and discard short/error/stale/competing results. Host
ordinary/sanitizer gates pass. Resolved-open synchronous flags missed by p018's
direct-mount test are repaired; root/overlay native regression passes. Automatic
prefetch, scheduler/lifecycle hooks and READ workload measurements remain pending.

Stream-state checkpoint: per-description bounded predictor is implemented and
passes ordinary/sanitizer sequential/random/tiny-read/EOF/overflow checks; amd64
build passes. No automatic scheduling yet. Queue ownership must use independent
VM read lifetime and distinguish FD close from temporary syscall reference drops;
this refinement is recorded in p020 before connecting asynchronous work.


Worker checkpoint: bounded asynchronous service and mount/global drain tokens are
implemented; production service fixture passes 221 ordinary/sanitizer checks with
paused preparation, read, adoption and cleanup. pcat/pc98 builds pass. File read,
descriptor close, seek, mount/shutdown hooks, useful accounting and READ workload
acceptance remain pending; this is progress within p020, not phase completion.

The ordinary amd64 kernel has been restored after pcat/pc98, with no diagnostic
wrapper. Next: connect actual file/mount/shutdown owners before READ measurement.


Lifecycle checkpoint: mount/private-unmount and shutdown now join speculative
ownership before VM/device teardown and restore admission on failed boundaries.
Actual worker+shutdown fixture passes 246 ordinary/sanitizer checks; VFS rollback
passes 153 each; QEMU USB/UFS writeback/unmount/remount regression passes. Automatic
file observations, descriptor invalidation, useful/pressure feedback and READ
measurement remain pending. This turn is implementation/verification progress.

Lifecycle changes also pass pcat/pc98 builds and ordinary amd64 restoration.
No build/runtime remains active. Continue p020 file/FD integration next.


Automatic-file checkpoint: ordinary read completion now schedules optional work;
FD close/dup2/exec/table teardown, seek and final retirement invalidate streams.
Real file/VM/FD host checks pass, with nonblocking optional-state capture. QEMU
exposed a boot-idle versus asynchronous USB reader deadlock; the sleepable-caller
admission fix passes native writeback/unmount/remount regression. Failed and
diagnostic runs are retained. Useful-hit feedback, public counters and READ
workload acceptance remain; p020 is still in-progress.

Final optional-state try-lock tuning passes real file/VM/FD host tests and all
three supported x86 builds. Ordinary amd64 is restored. Next p020 work: useful
consumption feedback, public speculative counters and READ workload acceptance.


Usefulness checkpoint: actual VM consumption feeds the file predictor and grows
its window after confirmed sequential use. Repeat/partial-read and truncate
attribution tests pass (431640 ordinary / 427854 sanitizer), as do worker/shutdown
regressions. Fixed frontier accounting is conservative for backward gap revisits;
this limitation is recorded in p020 before exposing counters. Public counters,
READ measurements/tuning and final acceptance remain pending.

Useful-feedback source hashes are retained and all supported x86 builds pass.
Ordinary amd64 restored; no running build/runtime remains. Continue public
counter exposure and measured READ acceptance; retain q110/p020 in-progress.


q110 review checkpoint (2026-09-07 07:37 UTC): public versioned read-only counters
and native measurement fixture are implemented. ABI sizing/unaligned output/write
rejection pass in QEMU; ordinary/sanitizer report/worker checks pass. Initial
wall-clock p95 was below the 10 ms clock resolution, so the fixture now retains raw
serialized TSC cycles plus explicitly calibrated estimates. A 32 KiB minimum
non-EOF refill reduces page-sized speculative submissions; state/file host tests
and a QEMU warm-buffer run pass with zero queue refusals. That run exposed that
prepare writes had warmed the lower data buffers (driver reads were zero), so it
is not accepted as storage-cold evidence. The current run uses distinct offline
sequential/random files and requires actual driver-read bytes. Continue within
authorized q110: cold USB/NVMe/low-memory measurements, remaining acceptance audit
and supported final builds. No human decision or physical measurement is required.


Cold USB result: sysctl ABI and data/optional-error/native regression checks pass,
with actual driver reads confirmed. Sequential prefetch used 643072 bytes and
reduced driver calls (75 versus random's 129), but estimated p95 was 1651 us versus
random's 504 us. This is an unresolved performance gate, not p020 completion.
Review inode I/O serialization and backend cursor/content-lease ownership before
further RAM/device measurement. Detailed records and earlier warm-buffer/clock
limitations are retained in p020 results. Continue within the authorized queue.

Public report/refill passes pcat/pc98 and ordinary amd64 restoration; no active
build/runtime remains. Next is the deterministic cached-demand versus paused
speculative backend regression and the corresponding read-ownership correction.


Cached-demand checkpoint: reproduced inode serialization behind paused speculative
I/O, then repaired ordinary cached reads with whole-transaction VM identity pins
and shared content leases. Backend cursor/I/O serialization remains enforced.
Host ordinary/sanitizer and cold USB native pass; tail latency improves, but p020
remains in-progress pending controlled sequential baseline and remaining acceptance.


Final result: q110 finished with both entries completed. p020 final results map
READ/CACHE/ASYNC acceptance to retained evidence. FS50/50, Wi-Fi30 in ordinary and
sanitizer variants, two-boot USB persistence, focused host/sanitizer, cold USB/NVMe
and low-RAM cells, controlled sequential baseline and all supported builds pass.
Ordinary amd64 is restored; no build/runtime is active. WS025 remains active with
p021–p026 and p027–p030 adoption decisions outstanding. Next queue must inspect
p021 actual UFS journal/ordering ownership and select a finite implementation scope.
