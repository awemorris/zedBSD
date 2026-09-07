# ws025-p020 results

Status: completed (q110), 2026-09-07. Automatic bounded prefetch, lifecycle,
useful accounting and public counters are implemented. Cached-demand serialization
is repaired; focused host/sanitizer, controlled cold workloads, supported builds
and final FS50/Wi-Fi30/native regression pass. Earlier sections retain historical
checkpoints and their then-current limitations. Final acceptance is recorded below.

## Conditional VM publication

`vm_object_prefetch_prepare` pins an existing cache operation and disk-cache
admission, captures content/size generations and bounds a missing prefix by EOF,
resident pages and 64 KiB. It allocates optional frames privately; demand faults
never encounter speculative BUSY pages waiting for background I/O. Requests must
cover whole pages; only authoritative EOF may shorten the last page. This avoids
publishing zero padding over valid but unrequested file data.

`vm_object_prefetch_complete` accepts only a full successful bounded payload and
an unchanged generation/range with no competing resident page. It initializes
private frames before atomic publication under the object lock. A stale/error/
short fill is discarded entirely, with no error page added to the demand cache.
Abort is idempotent and releases actual frame/accounting ownership, disk admission
and the retained object operation. The eventual worker must retain a live token
until its backend I/O has actually completed; this API is not transport cancel.

Focused real file/VM host tests (`temp/p020-prefetch-publication.log`) pass
414527 / 411898 ordinary/sanitizer checks, including missing cache, invalid range,
private allocation refusal, short/error reads, competing demand publication,
ordinary content mutation, resize, explicit abort, final partial EOF page and
warm demand hits after adoption. `temp/p020-prefetch-resolved-host.log` repeats
the relevant gate after the adapter correction below: 413483 / 411576 checks.
Initial ordinary/sanitizer compile/regression and amd64 builds also pass.
No production worker calls these APIs yet, so this is not READ01–READ04 completion.

## Resolved-open adapter correction discovered during identity inspection

Existing overlay descriptions keep a referenced real open file and stable
`f_vm_inode`; cache-only object creation already rejects a clone whose content
inode differs from the captured identity. Inspection exposed a p018 omission:
public open accepted O_SYNC/O_DSYNC, but `file_open_resolved` still rejected them,
so overlay's forwarded real-file open failed. q110 explicitly added this finite
p018 regression repair. The resolved adapter now accepts the same synchronous
flags. The native fixture adds root/overlay write/read/unlink with both flags;
`temp/p020-resolved-sync-native/results.json` and its log pass, including the new
`WRITEBACK OVERLAY SYNCHRONOUS PASS` marker and the existing full writeback/unmount
sequence. `temp/p020-resolved-sync-build.log` passes. This corrects the earlier
p018 direct-mount-only native flag coverage without hiding that gap.

Remaining implementation: per-description stream state; bounded independent
filesystem workers with demand priority; actual private backend reads followed
by conditional publication; close/seek/pressure/unmount/shutdown retirement;
useful/wasted/error byte observations and sequential/random p95 measurements;
READ/CACHE/ASYNC acceptance and native regression. q110 remains in-progress.

Supported pcat/pc98 builds and ordinary amd64 restoration pass
(`temp/p020-prefetch-pcat.log`, `temp/p020-prefetch-pc98.log`,
`temp/p020-prefetch-amd64-restore.log`). The q110 p018 adapter repair is complete;
p020 remains active. No build or runtime remains running at this checkpoint.

## Sequential stream state

Added a per-open-description `readahead_state` and production predictor in
`readahead.c`, linked in every existing kernel platform list. It requires a
second contiguous successful read, starts with a 64 KiB window, and grows to
128 KiB only after 64 KiB of useful speculative bytes are reported. Each candidate
is page-aligned and at most 64 KiB; the issued horizon remains bounded relative
to completed demand bytes, including one-byte reads. EOF clipping is delegated
to the already checked VM fill after requesting complete pages.

Random/reordered access, explicit reset, pressure and EOF invalidate pending
stream generations and reset confidence. Generation exhaustion permanently
refuses prediction for that open description rather than reusing an old token.
Invalid ranges reject without endpoint overflow. Prediction performs no I/O and
does not allocate. Callers must serialize observations; scheduler/seek/close hooks
are not connected yet and no automatic prefetch is claimed.

`temp/p020-state-host` passes ordinary and ASan/UBSan tests: independent states,
sequential/useful growth, discontinuity/pressure cancellation, 32768 one-byte
reads with a bounded issued horizon, 50000 mixed sequential/random observations,
EOF tails, signed file-offset limit and generation exhaustion. The ordinary
amd64 build passes (`temp/p020-state-amd64.log`). The next implementation step
is the asynchronous owner/scheduler and actual demand completion hookup.


## Bounded asynchronous service checkpoint

Implemented `readahead-worker.c` and linked it in all six existing platform lists.
There are four reusable physical workers, two total lifecycle slots each, and a
64 KiB payload plus control-page allowance per active scratch owner. Optional
shared accounting charges the actual HAL size. Each registered preparation owns
its physical token and then private VM fill; worker execution never dereferences
the original user file description. Admission checks stream generation while the
origin file lock and queue registry establish a race-free cancellation identity.
Control/file lock contention and full queues refuse optional work without waiting
for admission. No ordinary read hooks are connected yet.

The worker sends actual `file_pread_internal(..., FILE_IO_VM_OBJECT)` calls from
independent kernel threads and uses conditional VM publication. Global demand
priority postpones queued device reads but permits canceled cleanup. Mount/global
boundary tokens cancel and join preparation, queued/running I/O, adoption and final
resource release. A terminal adoption claim precedes VM publication; boundary
completion still waits for it. Idle trim returns scratch or preserves a failed
HAL free as a charged reusable owner. Setup failure cannot strand a disk token;
failed thread creation can retain bounded scratch for reuse/explicit trimming.

`temp/p020-worker-host-gates` passes 221 checks in ordinary and ASan/UBSan modes,
using actual production predictor/service with controlled VM, HAL and backend
edges. Cases include allocation/accounting/thread failures, failed HAL disposal,
two-slot and four-device limits, demand priority, stale origin generation,
cancellation with a device transfer paused, independent-device progress, teardown
while preparation/adoption/VM cleanup is paused, short/error/stale reads, nested
global/mount gates, interrupted boundary rollback, and final charged-memory/token
balance. Gate handshakes use the pthread bridge rather than unsynchronized polling
of the production boundary token. `temp/p020-worker-host` is the earlier passing
fixture before that test synchronization refinement. The existing private-VM
fixture separately validates real VM generation/content publication; this service
fixture deliberately controls those dependency edges.

The first amd64 service build and supported pcat/pc98 builds pass
(`temp/p020-worker-first-build.log`, `temp/p020-worker-pcat.log`,
`temp/p020-worker-pc98.log`). Remaining: hook automatic observations and lifetime
boundaries into file/descriptor/mount/shutdown owners, useful-page accounting,
pressure feedback, READ sequential/random measurements and native regressions.
p020/q110 remain in-progress; no automatic prefetch or acceptance PASS is claimed.

Ordinary amd64 restoration also passes (`temp/p020-worker-amd64-restore.log`);
its kernel has no diagnostic wrapper symbols. All three supported x86 builds
therefore include the final service admission handshake and demand-init refusal.
No build or runtime is left running at this checkpoint.


## Mount and shutdown lifecycle integration

Public/private unmount now owns a combined local boundary whose independent
readahead token is acquired before writeback pause, VM sync/cache drain, and
external mount-reference checks. Every failure path restores writeback policy
then ends the read boundary. Successful filesystem teardown releases both tokens;
the existing idempotent finish covers bind and ordinary mount finalization. A
readahead begin failure preserves the mounted device and returns its error before
writeback or VM teardown. The writeback subsystem does not own the read token.

Shutdown installs a permanent global readahead boundary before writeback and mount
barriers, joins every job, then trims idle scratch. Failure of optional admission,
scratch disposal, writeback begin or final mount sync leaves hardware live and
reopens the read gate for retry. Successful shutdown retains the static boundary
through and after network/USB/PCI teardown. Competing callers still join the same
shutdown owner; they cannot accidentally reopen its committed gate.

Evidence:

- `temp/p020-shutdown-lifecycle`: actual shutdown serialization, controlled
  readahead/writeback endpoints; ordinary and ASan/UBSan pass admission/trim/begin
  failures, sync failure and retry, concurrent callers, device ordering, and the
  committed read gate remaining installed.
- `temp/p020-unmount-lifecycle`: actual VFS/inode/name/cache teardown, controlled
  lifecycle providers; 153 checks per ordinary/sanitizer variant pass public and
  private rollback, read-boundary refusal, retained policy references, and exact
  read-token balance. Endpoint assertions require read quiescence before writeback.
- `temp/p020-worker-shutdown`: actual readahead predictor/worker AND shutdown code;
  246 checks per ordinary/sanitizer variant pass, including a backend read paused
  during real shutdown, no writeback before job/scratch release, a failed final
  barrier reopening admission, successful retry, and permanent post-shutdown
  refusal. VM/HAL/backend edges remain controlled as in the earlier worker fixture.
- `temp/p020-lifecycle-native-build.log` and
  `temp/p020-lifecycle-native/results.json`: ordinary amd64 build and QEMU 512 MiB
  USB/UFS writeback gate pass, including overlay synchronous flags, age drainage,
  disabled/enabled unmount, remount byte verification and zero final policy memory.
  No automatic read submissions exist yet; this is lifecycle/regression evidence,
  not sequential READ acceptance or physical hardware measurement.

Remaining implementation is automatic file transaction observation/priority,
close/seek/reset invalidation (including descriptor versus temporary reference
semantics), useful-page/pressure feedback and public speculative counters, followed
by READ/CACHE/ASYNC workload acceptance. Preserve the already connected mount and
shutdown boundaries when adding automatic jobs. p020 and q110 remain active.

Supported pcat and pc98 lifecycle builds and ordinary amd64 restoration pass:
`temp/p020-lifecycle-pcat.log`, `temp/p020-lifecycle-pc98.log`, and
`temp/p020-lifecycle-amd64-restore.log`. `git diff --check` passes. All build/runtime
handles are terminal at this checkpoint; the current kernel is ordinary amd64.


## Automatic file observations and descriptor invalidation

Ordinary regular-file transactions now retain their initial offset and stream
witness, balance demand priority across all successful/failed begin paths, and
sample authoritative EOF while the coherent content read lease remains held.
`file_io_complete` ends all file/content ownership before observing or submitting
optional work. Both witness capture and completion observation use try-locks;
a contended stream skips optional prediction rather than adding a wait to a
positional read. A seek/descriptor reset between completion and observation
invalidates the saved witness. Queue refusal cannot change the demand return.
Zero-length/failed reads reset confidence without reading EOF outside its lease.
Shared position, positional reads and the outer vectored syscall completion all
use the same owner. Existing synchronous-write completion still executes fsync.

Successful seek and ordinary writes invalidate old streams. Descriptor take/close,
dup2 displacement, close-on-exec and descriptor-table destruction explicitly
invalidate after dropping the table spinlock. A routine temporary file reference
drop does not invalidate; the last file reference resets before backend close and
pool reuse under exclusive ownership, avoiding a new sleeping file lock during
VM/file retirement. Closing one duplicate conservatively resets the shared stream;
remaining duplicates can establish a new stream. Active content generations still
protect publication independently of this heuristic cancellation.

Boot failure found and fixed: `temp/p020-auto-native` timed out during overlay
journal load. `temp/p020-auto-diagnose` retained a monitor stack capture and was
explicitly stopped after diagnosis. The boot/idle thread was waiting in
`file_regular_io_lock` from `overlay_validate_slot`, while CPU 2 was in
`drv_usb_urb_wait`; the boot thread cannot sleep like an ordinary scheduled task
behind an independently running filesystem owner. Worker admission now refuses
NULL/idle callers before allocating or publishing jobs. This preserves bootstrap
synchronous demand progress and admits asynchronous work from ordinary threads.
Loop/formatter backing-owner operations also skip independent stream prediction
and priority tracking; they run inside their outer I/O ownership and must not
create another prediction stream or acquire its description lock there. The
original failed run and diagnostic evidence are retained rather than overwritten.

`temp/p020-sleepable-native/results.json` passes the QEMU 512 MiB USB/UFS native
writeback, synchronous-overlay, enabled/disabled unmount and remount byte gate
with automatic file hooks and the corrected admission context. Its build log is
`temp/p020-sleepable-native-build.log`. This is regression evidence; it does not
by itself demonstrate a useful sequential prefetch hit or READ latency target.

Focused evidence:

- `temp/p020-file-observation-host.log`: real file/VM/claim/accounting paths plus
  actual predictor and controlled optional submission; ordinary 430886 and
  sanitizer 428962 checks pass the completion/reset boundary.
- `temp/p020-file-fd-host.log`: adds actual filedesc ownership paths; ordinary
  431877 and sanitizer 428989 checks pass temporary-reference versus descriptor
  close/dup2/close-on-exec/table-destruction behavior.
- `temp/p020-file-nonblocking-host.log`: final try-lock capture/observation and
  all real file/VM/FD regressions pass (431275 ordinary / 430280 sanitizer).
  Includes a positional read with the optional description state contended;
  the demand still completes with balanced priority and no new submission.
  `temp/p020-file-source.json` retains the final production/fixture source hashes.
- `temp/p020-worker-sleepable`: actual asynchronous service plus shutdown passes
  247 checks in both modes, adding idle-admission refusal before allocation.

Automatic prediction currently uses a 64 KiB window because useful-prefetch-byte
feedback has not yet been connected. Per-description useful consumption, public
speculative counters, pressure/queue tuning and READ/CACHE/ASYNC workload acceptance
remain required. Do not mark p020 complete from these hook/regression tests alone.

Final file-hook pcat/pc98 builds and ordinary amd64 restoration pass
(`temp/p020-file-pcat.log`, `temp/p020-file-pc98.log`,
`temp/p020-file-amd64-restore.log`); diff whitespace check passes. The native
sleepable-caller regression precedes the final optional-state try-lock tuning;
that final tuning is covered by the nonblocking host fixture and these builds.
Native READ workload acceptance remains pending. No build/runtime remains active.


## Confirmed usefulness feedback

Published speculative pages retain a bounded forward-consumption frontier, credited
byte count, valid EOF-clipped byte count and size-generation witness. Coherent
reads report newly credited spans through `vm_object_read_coherent_useful`; the
original API remains a wrapper for existing callers. File transactions accumulate
that feedback across copy/iovec chunks and feed it to their open-description
predictor. Real sequential use of 64 KiB now permits its 128 KiB lookahead window.

Counters have deliberately conservative semantics: forward consumption is exact,
repeated/overlapping bytes never count twice, and backward reads into earlier
skipped gaps are not retrospectively credited. `useful_bytes` is therefore a lower
bound for arbitrary within-page ordering; `unused_bytes` means retired uncredited
bytes, an upper bound on unused data, not proof the application never touched them.
This uses fixed per-page metadata rather than allocating a byte-coverage bitmap.
READ sequential workloads use exact forward accounting; random reports must expose
this limitation rather than claiming an exact unused-byte measurement. Mapped pages
stop ordinary-read attribution, since hardware writes cannot be inferred from a
later copied range. Ordinary dirty publication, content replacement, size-generation
changes and storage release also retire attribution before bytes can be reused.
Private failed/stale fills never create published attribution.

The real VM/file fixture passes 431640 ordinary and 427854 sanitizer checks in
`temp/p020-useful-eof-host.log`. It demonstrates actual 64 KiB speculative population,
16 forward page reads growing the actual file predictor to 128 KiB, no repeat-read
credit, partial forward consumption, exact unused retirement for those spans,
and truncate invalidating even a surviving cached prefix. Earlier useful-only
passes are retained in `temp/p020-useful-feedback-host.log`; the original regression
run is `temp/p020-useful-first.log`. Actual worker/shutdown lifecycle regressions
also pass 247 checks in both modes (`temp/p020-useful-worker`).

Remaining: expose/version speculative counters, measure sequential useful-hit and
random latency/waste bounds, tune demand/physical queue admission if needed, and
complete READ/CACHE/ASYNC acceptance plus final native regression. No READ acceptance
or phase completion is inferred from host feedback tests alone.

Useful-feedback pcat/pc98 builds and ordinary amd64 restoration pass
(`temp/p020-useful-pcat.log`, `temp/p020-useful-pc98.log`,
`temp/p020-useful-amd64-restore.log`). `temp/p020-useful-source.json` retains
production/fixture hashes. Diff whitespace check passes; no runtime or build
remains active. This checkpoint does not add new native READ acceptance evidence.


## Public counters and native measurement checkpoint

Added the read-only `vfs.readahead.stats` OID and CLI formatting. The UAPI is a
versioned fixed-width 88-byte report with explicit `confirmed_useful_bytes` and
`retired_uncredited_bytes` names; it does not mislabel conservative attribution as
exact application waste. Requested/started/published/discarded-fill bytes, errors,
queue refusals, actual scratch bytes, jobs/running/demand are also reported.
`temp/p020-report-host` passes 252 ordinary/sanitizer checks, including inactive
version/size/offsets and final counter/owner balance. The native fixture validates
size query, undersized output without overwrite, unaligned output guards, version,
write rejection and normal CLI output through the real syscall/metadata path.

`readahead-native` reads all bytes of two 1 MiB fixtures in 256 positional 4 KiB
calls, in sequential order or a non-adjacent permutation. The runner's
`--readahead` option performs unmount/remount boundaries between cases and retains
ordinary writeback/overlay/unmount regressions. Important measurement corrections:

1. `temp/p020-read-usb` demonstrated useful prefetch, but its p95 was zero because
   CLOCK_MONOTONIC resolves only 10 ms. This is not acceptable latency evidence.
2. The amd64/QEMU fixture now records serialized raw TSC cycles, a 250 ms
   CLOCK_MONOTONIC calibration, reported clock resolution, and explicitly estimated
   microseconds. The estimates have calibration quantization error; raw cycles are
   retained. `temp/p020-read-usb-cycles` measured sequential p95 about 431 us and
   random about 231 us, with 823296 confirmed useful bytes in the sequential case.
3. Prediction had been issuing a small new request as every demand page extended
   its horizon. It now waits for at least half the minimum window (32 KiB), except
   at EOF. This preserves lookahead while avoiding page-at-a-time optional refill.
   `temp/p020-refill-state` passes ordinary/sanitizer horizon/EOF/random/overflow and
   minimum-refill tests; `temp/p020-refill-file-host.log` passes 431239 ordinary /
   428511 sanitizer real file/VM/FD checks. `temp/p020-read-refill-usb` had 0 queue
   refusals, 974848 confirmed useful bytes, and estimated p95 351 us sequential /
   403 us random. However, added I/O counters showed 0 driver reads: guest prepare
   writes had warmed the lower buffer cache. Those results are VM-cold, not
   storage-cold, and must not be claimed as a cold device improvement.
4. Final fixture data is created offline in two different files/allocated ranges.
   Guest preparation only checks lengths; it never reads/writes the payload.
   Native cases now require at least 1 MiB of actual driver reads, and report
   syscall bytes/calls, filesystem-content bytes/calls and driver bytes/calls.

`temp/p020-read-cold-usb/results.json` passes the corrected ABI, data correctness,
positive sequential usefulness, zero optional errors and surrounding native
regressions at 512 MiB. Its storage-cold measurements are:

| Workload | Requested syscall bytes/calls | FS content calls | Driver calls/bytes | Confirmed useful | Estimated total read time / p95 |
| --- | --- | --- | --- | --- | --- |
| sequential | 1048576 / 256 | 157 | 75 / 1052672 | 643072 | 144007 us / 1651 us |
| random | 1048576 / 256 | 256 | 129 / 1052672 | 0 | 95239 us / 504 us |

The sequential workload still has excessive latency despite fewer driver calls.
This is NOT a performance acceptance PASS and p020 remains in-progress. Random
order is a different workload, not a controlled no-prefetch baseline; nevertheless
the large demand tail warrants investigation before further RAM/device cells.
Inspection shows speculative backend reads currently take the same inode I/O mutex
that ordinary demand reads acquire before checking cached pages. A cached demand
can therefore wait behind future-page backend I/O. The next implementation must
review that serialization, backend per-file cursor ownership, and shared content
read leases together. Do not simply set INODE_IO_OWNED without actually owning the
required contract or remove protection from mutable filesystem state. After a
sound read-ownership change, repeat cold measurements, then low-RAM/NVMe and the
remaining READ/CACHE/ASYNC audit. Public counter/refill builds are separate from
that unresolved performance gate.

Native build logs: `temp/p020-report-build.log`, `temp/p020-read-native-build.log`,
`temp/p020-read-cycle-build.log`, `temp/p020-refill-native-build.log`, and
`temp/p020-read-cold-build.log`. Earlier runs and their limitations remain retained.

Public-report/refill supported builds pass: `temp/p020-report-refill-pcat.log`,
`temp/p020-report-refill-pc98.log`, and ordinary amd64 restoration in
`temp/p020-report-refill-amd64-restore.log`. Final source hashes are retained in
`temp/p020-report-refill-source.json`; whitespace check passes. No build/runtime
remains active. Performance/read-ownership work remains open; p020 is not complete.


## Cached demand ownership correction

`temp/p020-cached-progress-before.log` deterministically fails `CHECK(progress)`:
a real internal backend read holds inode I/O for future pages while an independent
ordinary description cannot read an already resident page. Cleanup releases the
paused backend before reporting failure. This confirms the serialization problem.

Ordinary cached reads now acquire shared content leases and a published-object
operation/reference pin before generic inode I/O acquisition. The pin spans every
outer syscall chunk and prevents eviction/final-mapping teardown between chunks.
Stacked reads retain both visible and final content gates. Cache misses still use
the existing serialized internal backend; claims/internal requests and unavailable
cache identities retain the old acquisition path. No backend ownership flag is
forged. A missing supposedly pinned identity returns EIO instead of entering an
unprotected backend. Pin release occurs after outer file/content locks are dropped.

The unchanged regression passes after the correction. Extended coverage verifies
two transfer chunks under one transaction, cache drain refusal while pinned,
write/resize refusal during the shared lease, and successful resize admission after
release. `temp/p020-cache-pin-leases-host.log` passes 425201 ordinary and 425869
sanitizer file-cache checks, plus 626 formatter checks each. Earlier unchanged
regression results are in `temp/p020-cached-progress-after.log`.

The ordinary amd64 native build (`temp/p020-cache-pin-build.log`) and corrected
cold USB run (`temp/p020-cache-pin-cold-usb`) pass data/ABI and writeback/remount
checks at 512 MiB. Sequential: 256 calls/1 MiB, 74 driver calls/1052672 bytes,
675840 confirmed useful bytes, 196608 discarded fill bytes, zero errors; estimated
total 105397 us, p95 881 us (2198747 cycles). Random: 129 driver calls/1052672
bytes, no speculative bytes, estimated total 94477 us, p95 577 us. Final jobs,
running and demand counters are zero. The former sequential 144007/1651 us run
improves, but this is a between-run comparison with timing variation. Random order
is still not a controlled sequential baseline. Keep performance acceptance open
pending that comparison and remaining device/RAM/READ coverage.


Additional ordinary-kernel cold cells pass data/ABI/writeback/remount checks:

| Cell | Sequential total / p95 estimate (us) | Random total / p95 estimate (us) | Sequential confirmed useful / discarded fill bytes |
| --- | --- | --- | --- |
| NVMe, 512 MiB (`temp/p020-cache-pin-cold-nvme`) | 170000 / 2300 | 115547 / 671 | 679936 / 229376 |
| USB, 128 MiB (`temp/p020-cache-pin-cold-usb-128`) | 155030 / 2019 | 98095 / 507 | 712704 / 229376 |

Each cell performs real cold driver reads; speculative errors and final live jobs
are zero. The lower-RAM cell is a boot/runtime check, not injected reclaim pressure;
that behavior is separately exercised in focused state/worker/cache fixtures.

A controlled baseline fixture now links `--wrap=readahead_submit` from
`tests/readahead-baseline.mk`: it retains the same demand, cache and prediction
paths and rejects only optional prefetch admission. It is never included by the
production build. The guest `sequential-baseline` mode requires zero started/useful
speculative bytes, rather than relaxing the ordinary useful-hit requirement. Use
`--readahead --readahead-baseline` with the QEMU runner, record the diagnostic build,
and force ordinary kernel relinking before subsequent production verification.
The first baseline (`temp/p020-no-prefetch-usb`) passes with sequential estimated
173796/3117 us and random 69867/460 us. Execution variation is significant, so
repeat comparable cells before drawing a performance conclusion.


The second matched USB/512 MiB baseline (`temp/p020-no-prefetch-usb-2`)
passes with sequential estimated total/p95 154776/2890 us. Forced ordinary
kernel restoration is logged in `temp/p020-cache-pin-normal-restore.log`;
`nm build/amd64/vmunix` confirms no `__wrap_readahead_submit` symbol. The second
ordinary run (`temp/p020-cache-pin-cold-usb-2`) passes with sequential
142175/2632 us, 745472 useful bytes and 196608 discarded fill bytes; random
91273/558 us and zero speculative bytes. Across these two runs per configuration,
ordinary sequential total spans 105397–142175 us versus 154776–173796 us for the
no-admission baseline, and p95 spans 881–2632 us versus 2890–3117 us. This supports
retaining bounded admission for this measured workload after fixing cached-demand
serialization. It is a small QEMU sample, not a universal throughput or real-device
latency guarantee. Do not compare random and sequential as the same workload.

The pin correction passes all supported builds: `temp/p020-cache-pin-pcat.log`,
`temp/p020-cache-pin-pc98.log`, `temp/p020-cache-pin-amd64-restore.log`. Source hashes
are in `temp/p020-cache-pin-source.json`. The ordinary kernel is restored.
The remaining final gate is the full storage/Wi-Fi/native regression and a final
READ/CACHE/ASYNC coverage review; this checkpoint alone does not complete p020.


## Final acceptance

`plan/ws018-kernel-architecture/temp/ws025-p020-final/results.json` reports
FS50/50 PASS with no unrun scenarios. The retained command manifest and logs cover
ordinary/sanitizer storage providers, syscall/claim/image cells, Wi-Fi30 in both
variants and the two-boot USB native persistence gate. Build provenance is
`temp/p020-cache-pin-fs50-build.log`; runner output is
`temp/p020-final-regression.log`. All testing is on host fixtures/disposable QEMU
images. Physical acceptance remains user-accepted, not agent-measured.

| Contract | Evidence and scope |
| --- | --- |
| READ01 | Actual per-description observer + real VM usefulness tests; offline-cold USB/NVMe measurements distinguish demand/content/driver counters from requested/started/published/useful speculative bytes. |
| READ02 | Predictor seek/random/pressure/tiny-read/overflow cases; actual worker queue, allocation/accounting/thread refusal and demand-priority cases; USB 128 MiB boot/runtime; deterministic cached-demand progress while future-page backend is paused. |
| READ03 | Actual VM private-fill tests reject content mutation, truncate, competing demand publication and short/error results; EOF tail is zeroed only past authoritative EOF. Stable referenced overlay content identity and clone mismatch rejection preserve old versus copied-up domains; final native overlay storage regression passes. |
| READ04 | Optional submission follows completed demand ownership release and cannot alter its result; real file/VM observer tests retain successful reads across admission refusal, and worker error/short/stale cases adopt no pages or late demand error. |
| CACHE03–CACHE05 | Real file/VM coherence/lifetime fixtures cover writes, mappings, partial pages, claim exclusions, generation and EOF changes. New pin tests cover outer multi-chunk read versus truncate/write/drain. Native overlay and mount/remount regressions pass. |
| CACHE06–CACHE09 | Existing p016/p018 budget/dirty/claim contracts are retained; optional frames and worker storage use their shared actual-RAM accounting. Focused allocation/pressure/failed-free tests keep unreleased ownership charged; per-device worker isolation and mount/shutdown rollback are tested with controlled delays. |
| ASYNC01–ASYNC08 | Completed p019 owned-BIO transport evidence remains applicable. The filesystem worker has separately tested bounded nonblocking admission, origin retirement, accepted-running cancellation, short/error/stale completion, multi-device progress and full preparation/completion/cleanup joins. No running I/O is treated as safely canceled or freed early. |

The source/config and ordinary-kernel restoration evidence above completes p020.
A running bounded backend transfer can still delay an actual cache miss; the
change removes needless waiting for already resident data. Two-run QEMU timing
ranges characterize the measured USB workload only. Useful accounting remains a
lower bound for non-forward page revisits; retired-uncredited bytes remain an
upper bound. These are reported limitations, not unimplemented phase requirements.
The previously recorded USB shutdown controller-retention issue remains owned by
p025/p026; this phase does not claim physical HCD detach success.
