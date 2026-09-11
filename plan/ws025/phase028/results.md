# ws025-p028 q125 results

## 2026-09-10 現行状態

cancelled（2026-09-10ユーザー指定）。67b28ce0で専用実装を除去。性能未達のまま採用撤回。
旧Queue・設計・測定は履歴であり、下記の継続実装計画は失効。再実装しない。
今回の確認はソース読取りのみ。詳細は[修正後照合](../post-rollback-review.md)。

Status: uncleared
Date: 2026-09-09

## 確認した事実

HEAD 34a1f6dの現ソースとq122/p024の既存証拠を確認した。
vmspace_pin_user_pagesは存在するが、pinはDMAとの同時書換えを禁止するcontent leaseではない。file content leaseとuser aliasの寿命は別。p024 controlled比較はCPU改善を示していない。

## 未完了と再開

direct user I/Oを正当化する対象workloadのcopy律速測定がなく、既存pinだけではalias/COW整合を満たせない。p024のコピー削減結果をdirect I/OのCPU改善と読み替えない。

対象workloadのcopyコストを分離した測定、および共有aliasのfreeze/COW方式を検証するVM fixture。測定と契約が揃うまで既存copyで運用する。

production実装・新規性能測定を行ったとは主張しない。既存挙動と既定値を維持した。

## q231: current-source vmap baseline (2026-09-10)

Repaired `run-vmap-host.py` from removed `src/kern/io-scratch.c` to the
actual merged `src/kern/io.c`. Function/data sections and linker GC retain
actual scratch functions without requiring unrelated pool/statistics owners.
No production changes and no source extraction or copied implementation.

Ordinary and ASan/UBSan with leak detection each passed 13,548 assertions:
reservation exhaustion, every injected allocation/leaf failure and retry,
high/noncontiguous frames, supervisor/NX mappings, release while pinned,
translation retirement, scratch fallback and busy-release pin restoration.
The first sanitizer execution stopped at the sandbox ptrace/LSan restriction;
the exact same binary passed when rerun outside that restriction. Evidence:
`temp/q231-vmap1/source.json`, `result.json`, and compiled binaries. This is
controlled host ownership evidence, not native user-I/O or CPU measurement.

p028 remains uncleared. Next: separately owned borrowed-frame mapping API
with partial-map rollback and shootdown-before-unpin tests; then content lease,
alias/COW races, input snapshot and output-prefix publication, and native
copy/CPU measurement. No direct-user syscall enabled. No production build
required for this fixture-only repair; broader old-test cleanup stays WS026.

## q232: borrowed-frame vmap implemented (2026-09-10)

`hal_vmap_borrow` now implements explicit borrowed RAM ownership on amd64.
It validates the complete bounded vector, alignment/address width, mapped RAM
and compatible write-back attributes; duplicate frames are valid. Borrowed
pages are neither cleared nor freed. Supervisor/NX aliases have explicit
readonly/write permissions. Failed population clears PTEs and completes
shootdown before restoring the reservation. Successful release similarly
retires translations before the caller may release physical pins. Existing
optional HAL symbol convention permits portable callers to retain fallback.

Actual implementation fixture: ordinary and ASan/UBSan/leak detection each
PASS, 18,060 checks. Added malformed vector/cache attribute rejection, every
partial leaf failure with retry, high/repeated frames, readonly/write PTEs,
pinned-release refusal, no frame allocation/free and subsequent owned-slot
reuse. Original owned scratch tests remain. Host collaborators simulate page
tables/TLB; this proves controlled ownership, not native alias protection.
Evidence: `temp/q232-vmap2/source.json` and `result.json`. The earlier sanitizer
attempt was stopped by sandbox ptrace constraints; final pair ran outside
that constraint with leak detection enabled.

Sequential disk-image builds all exit 0: `/tmp/zedbsd-q232-amd64-final.log`,
`/tmp/zedbsd-q232-pcat.log`, `/tmp/zedbsd-q232-pc98.log`. PCAT was built before
the final amd64-only cache-attribute guard; its production sources did not
change afterward. No aggregate suite or native direct-user enablement.

p028 remains uncleared: mapping is a foundation, not a content lease. Next
implement the pinned-private-page try-upgrade and alias/COW freeze protocol,
then input/output syscall publication and copied-byte/CPU measurements. A
caller must not use this mapping to bypass those unresolved contracts.

## q233: nonblocking private pin upgrade (2026-09-10)

Added `vm_private_page_io_try_upgrade(backing, owned_pins)` in actual vm.c.
It rejects zero/missing input and refuses BUSY, absent residency, competing
operations or nonmatching pin multiplicity without waiting or changing state.
Success marks BUSY and advances generation without taking another reference;
existing io_release must precede the caller's unpins. This only reserves
metadata ownership and does not protect content from existing writable PTEs.

Actual vm.c fixture passes ordinary and ASan/UBSan/leak checking: 64 rounds
of eight competing threads each have exactly one winner; existing pin,
operation and I/O acquisition refuse while held; release wakes waiters;
invalid upgrades preserve flags/generation/pins/refs; generation wrap skips
zero; actual final unpins restore the original reference and untouched data.
Controlled host IRQ/wakeup collaborators do not prove alias/fault behavior.
Evidence `temp/q233-upgrade1/source.json`, `ordinary.log`,
`sanitize-retry.log`, `result.json`. Initial LSan hit the sandbox ptrace
restriction; same binary passed elevated. All supported disk-image builds
exit 0: `/tmp/zedbsd-q233-amd64.log`, `-pcat.log`, `-pc98.log`.

Source review additionally found that mprotect can republish PTE permissions
unless region/mapping reservations persist, and a fault reserves mapping
metadata before waiting for backing ownership. The next acquisition must
preflight/reserve all aliases under metadata and refuse conflicts without
waiting. See the q233 refinement in design-followup.md.

p028 remains uncleared for vector-level deduplication/rollback, alias freeze,
actual fork/unmap/protect race acceptance, syscall input/output integration
and native copy/CPU measurements. No syscall enablement in this queue.

## q234: private pinned-vector alias lease (2026-09-10)

Implemented opaque acquire/release in vmspace.c. A bounded vector deduplicates
private owners/pin multiplicity, verifies captured frame identity, upgrades
all owners under global metadata, and reserves all aliases/regions/vmspace
references. It then synchronously unmaps hardware aliases without VM locks,
conservatively preserves dirty state, and retains BUSY/region holds through
release. Revoked mappings keep descriptors/COW permissions and refault after
release. Error rollback returns all acquired references and reservations;
caller pins remain owned until explicit unpin. No borrowed view or syscall is
yet connected. Bounds are 16 input pages and 64 aliases; overflow falls back.

Actual vm.c/vmspace.c controlled owner fixture passes ordinary and
ASan/UBSan/leak detection. Coverage includes repeated pins, two vmspaces,
foreign pins, shared input, stale frame identity, BUSY alias, dying space,
allocation refusal, each of four HAL-unmap failures, already absent PTE,
COW flag retention, all-alias reservation before any HAL call, exact 64/65
capacity, detached pinned backing, region-count overflow, and full
pin/reference/hold/allocation conservation. Partial successful revokes stay
absent after rollback rather than restoring stale PTEs. Host locks, HAL
retirement and final vmspace-put collaborators are controlled; actual
fork/mprotect/unmap races and final vmspace destruction are NOT proven here.

Evidence: `temp/q234-lease2/source.json`, `ordinary.log`, `sanitize.log`,
`result.json`. Initial `q234-lease1` sanitizer was stopped by the sandbox ptrace
restriction, then passed as the same binary elevated; final expanded suite
ran both variants elevated. Sequential supported disk-image builds exit 0:
`/tmp/zedbsd-q234-amd64.log`, `/tmp/zedbsd-q234-pcat.log`,
`/tmp/zedbsd-q234-pc98.log`. No aggregate suite or native runtime claim.

p028 remains uncleared. Next reuse the actual VM/PTE backend in
`tests/exec-snapshot-vm-host.c` for mutator/lease races, rollback/refault and
exit/lifetime tests, then connect borrowed mapping and input/output syscall
paths and measure native copied bytes/CPU. Full lease acceptance is not
inferred solely from this controlled fixture.

## q235: actual VM mutators and alias lease (2026-09-10)

Reconnected `run-exec-snapshot-vm-host.py` to merged vm/cache/io translation
units, using linker GC and weak host-service substitutions. Actual lease,
fault, fork, protect, unmap, private backing, COW and vmspace destruction
implementations are linked unchanged. Added a host wait observer to the shared
fixture; it observes actual waitq entry and does not replace the VM operation.

Ordinary and ASan/UBSan/leak detection PASS, 11,073 checks each, including the
existing executable snapshot/ELF/COW regression. New cases fork an anonymous
private page into two vmspaces, pin one, acquire a lease and check both PTEs
absent. Real protect/unmap/write-fault/fork workers enter their wait path;
completion is absent while the lease is held, then succeeds after lease
release and unpin. Writing child faults COW and leaves the parent unchanged.
Dropping the child's normal reference while leased exercises actual final
vmspace destruction during lease release. A second-alias HAL unmap failure
rolls back and both actual VM read/refault and later COW write recover.
All cases return committed accounting to zero. HAL PTE/TLB behavior remains a
checked host backend, not real hardware shootdown or native timing evidence.

Evidence: `temp/q235-vm4/source.json`, `ordinary.log`, `sanitize.log`,
`result.json`. Wiring first required pthread feature definitions at object
compile time for O_DSYNC; no production change. Initial baseline sanitizer
hit the known sandbox ptrace restriction; final tests ran elevated with leak
detection. Production is unchanged since q234, so its three supported build
results remain applicable and were not repeated.

p028 remains uncleared. Next connect the completed pin/lease/borrowed-vmap
owners through a bounded uaccess view, then eligible input/output file syscalls.
Preserve early whole-request pin capture, file transaction semantics and
untouched output suffixes. Direct DMA/zero-copy or CPU benefit is not proven;
measure copied bytes and CPU natively before enabling the path by default.

## q236: readonly uaccess view (2026-09-10)

Implemented `uaccess_view_acquire/release` for complete aligned READ pins of
4–64 KiB. The view acquires the actual VM alias lease then reserves, borrows
readonly frames and pins the kernel VA. Failures retire the reservation
before releasing the lease. The parent records a live view and rejects a
second acquire; unpinning a live view is an invariant violation. Release
retires the mapping before ending the lease. A busy release restores the VA
pin and leaves lease/parent ownership intact for retry. Optional HAL absence
returns ENOTSUP before changing any ownership. Existing user pins remain
owned by the caller. This API is input-only; output-prefix publication has
not been implemented by this step.

Actual uaccess owner fixture with controlled HAL/lease collaborators passes
ordinary, ASan/UBSan/leak detection, and a separate binary with the borrowed
HAL symbol deliberately omitted. It verifies alignment/protection rejection,
lease/reserve/borrow/pin failures, reverse-order retirement, no data change,
duplicate acquire preservation and busy release/retry. Evidence:
`temp/q236-view2/source.json`, variant logs, `result.json`. These tests validate
composition/error ownership; they do not prove actual kernel VA contents or
syscall transfer. q232 and q235 retain the separate real-vmap/real-VM evidence.

Sequential disk-image builds all exit 0: `/tmp/zedbsd-q236-amd64.log`,
`/tmp/zedbsd-q236-pcat.log`, `/tmp/zedbsd-q236-pc98.log`.

p028 remains uncleared. Next integrate eligible regular-file write/pwrite
through the existing begin/growth-limit/transfer/complete contracts, holding
the view through completion; add counters and a controlled experimental
selection for native comparison before deciding default enablement. Reads
must preserve untouched suffixes on backend error/short result; no writable
view is exposed yet. Native mapped-content, performance and broader I/O
acceptance are outstanding. No syscall currently invokes the new view.

## q237: experimental scalar write/pwrite connection (2026-09-10)

Regular-file write/pwrite now optionally acquire the full-pin readonly view
before file_io_begin, skip copyin when acquired, and retain it through
file_io_complete. Begin failure and all transfer/completion exits retire the
view before unpin. Unsupported/refused input uses existing staging; a borrowed
VA is never returned to io_pool. Existing file transaction, growth-limit and
completion calls remain in their original order. `ZEDBSD_USER_INPUT_VIEW`
defaults to zero pending native acceptance and measured benefit.

I/O report version is 9 with appended IO_SCALAR_INPUT_COPY and
IO_SCALAR_INPUT_VIEW: scalar copied bytes versus attempted view backend bytes.
They do not represent committed bytes, total cache/device copies or CPU gain.

Updated existing WS018 syscall fixture wiring for the added helpers. It still
extracts complete production functions verbatim (not a reimplemented loop),
with controlled file/user-copy boundaries. Sanitized default and experimental
variants pass six existing syscall-direction stories and new no-copy
write/pwrite success, positional offset, short result, begin/backend/complete
failure cleanup, and view lifetime through completion. This boundary model
is not native O_APPEND, RLIMIT or hardware mapping evidence.

Experimental amd64 disk-image build exit 0 with
`ZEDBSD_TEST_CPPFLAGS=-DZEDBSD_USER_INPUT_VIEW=1`; exact image and configuration
saved in `temp/q237-experimental/` with `image.sha256`, `source.json`,
`syscall-host.log`, `result.json`. It has not yet booted in QEMU. Default
amd64/PCAT/PC98 builds then all exit 0, restoring the shared default amd64
artifact. Build logs: `/tmp/zedbsd-q237-amd64-view.log`,
`/tmp/zedbsd-q237-amd64.log`, `/tmp/zedbsd-q237-pcat.log`,
`/tmp/zedbsd-q237-pc98.log`.

p028 remains uncleared. Next run disposable QEMU images comparing the same
aligned write/pwrite/readback workload with view on/off, including append,
size-limit and sync/completion semantics. Observe actual view/copy counters
and process CPU/wall time; retain fallback/default-off if evidence does not
justify adoption. Output/read prefix publication remains unimplemented and
required for the full phase, not silently waived by input-path progress.

## q238: native input-view comparison (2026-09-10)

Both default and q237 experimental images booted in disposable four-CPU,
512 MiB QEMU with USB BOT root. Identical guest executable verifies aligned
write/pwrite, fsync/readback, unaligned fallback, append after seek-to-zero,
and RLIMIT_FSIZE partial write of 37 bytes with signal handling. Both pass;
source image hashes remain unchanged. No production changes in this queue.

Initial 32-overwrite cells q238-off2/on1 prove copy/view selection (2 MiB) but
90/80 ms CPU measurements were too short. Expanded q238-off3/on2 use the same
binary SHA256 and eight samples of 256 overwrites, 64 KiB each (128 MiB total):

| Mode | Scalar input copy | Direct view | Guest process CPU | Guest elapsed |
| --- | --- | --- | --- | --- |
| Default | 134217728 | 0 | 3.04 s | 3.02 s |
| Experimental | 0 | 134217728 | 4.76 s | 4.77 s |

Each 16 MiB sample independently has the expected exact copy/view counters.
View CPU is about 1.57x baseline. It removes the staging copy but makes this
warm-overwrite workload slower; do not infer a performance win or enable it
by default. Timing is guest accounting under QEMU and not physical hardware
CPU evidence; samples are within one boot per mode, not independent trials.

Evidence: `temp/q238-off3/` and `temp/q238-on2/` result.json, guest.log,
argv.json, test-source.json; `temp/q238-comparison.json`. Initial q238-off1
failed helper compilation before starting QEMU (SIG_IGN UAPI integer type);
helper now installs a typed SIGXFSZ handler. All subsequent cells terminate
normally from the runner after the complete guest PASS and source hash check.
No repeated build gate: q237 production and its build evidence are unchanged.

p028 remains uncleared. The native path now works but benefit is not proven.
Current lease unmaps every alias; subsequent reads/pins must refault, which
is a plausible cost source rather than a measured attribution. Next explore
an input-specific write-protect lease that retains readable aliases while
blocking writes, with actual COW/fault/mprotect/rollback tests and repeated
native comparison. Do not apply readable aliases to output publication,
which still needs untouched-suffix/partial-error guarantees and remains an
explicit unimplemented part of p028.

## q239: readable input aliases and native comparison (2026-09-10)

Added input-specific lease acquisition: same all-owner/all-alias BUSY/region
reservation, but READ/EXEC aliases retain mappings with WRITE removed;
write-only aliases still unmap. The full-unmap API remains unchanged. Readonly
uaccess views now use this input API. Existing descriptors/COW semantics stay
intact; no stale writable PTE is restored at release.

The added non-COW test caught a real integration gap: existing fault logic
returned success for already mapped non-COW pages without restoring WRITE.
Introduced VM_MAPPING_INPUT_PROTECTED and restoration on a subsequent write
fault; HAL restore failure leaves the marker for retry. Fresh COW replacement
clears it. Actual VM tests now pass 12,627 checks ordinary and sanitized,
including both lease modes, real mutator waits, partial protection failure,
read-pin reuse without remapping, non-COW write permission restoration/failure
retry, write-only fallback, final space release and existing exec/COW cases.
Uaccess and full-unmap focused fixtures also pass ordinary/sanitized;
absent-HAL fallback passes. Evidence q239-vm4, q239-view1, q239-lease1.

Experimental amd64 and default amd64/PCAT/PC98 disk-image builds exit 0:
`/tmp/zedbsd-q239-amd64-view.log`, `-amd64.log`, `-pcat.log`, `-pc98.log`.
Experimental image/config/source hashes are retained in q239-experimental.
QEMU off1/on1 use identical helper binary, eight 16 MiB overwrite samples,
plus write/pwrite/readback/append/unaligned/limit/fsync and a normal guest CPU
store after lease release (hardware-emulated write-fault restoration).
Both PASS with unchanged source images and exact 128 MiB copy/view selection.

| Mode | Input copy | View | Guest CPU | Guest elapsed |
| --- | --- | --- | --- | --- |
| Default | 128 MiB | 0 | 3.08 s | 3.07 s |
| Readable input view | 0 | 128 MiB | 3.87 s | 3.86 s |

This reduces the earlier full-unmap cost (4.76 s in q238), but remains slower
than this queue's baseline. Different queue timings are not a controlled
single-variable CPU attribution. Default remains off. Results/argv/guest logs
are in q239-off1/on1, with q239-comparison.json. No physical-hardware performance
claim; each mode has one boot with eight samples.

p028 remains uncleared. Next inspect batching contiguous same-vmspace alias
protection into one HAL range operation: current code synchronizes each page
individually. Validate HAL failure atomicity and all mapping reservations
before changing this. Output publication and final performance/adoption
acceptance are still required; do not mark the whole phase complete.

## q240: contiguous input permission ranges (2026-09-10)

Compatible adjacent input aliases now share a single hal_space_prot range:
same vmspace, contiguous virtual addresses, MAPPED and equal nonzero readonly
permissions. Holes/protection differences/space changes split ranges;
full-unmap and write-only paths remain individual. All alias reservations
precede the transition. INPUT_PROTECTED is conservatively recorded before
HAL work, so a partially applied failure remains repairable by write fault.
amd64/i386 HAL source prevalidates the full range before normal permission
updates and completes shootdown before returning success.

Actual VM fixture passes 13,648 checks in ordinary and ASan/UBSan/leak modes.
New cases prove four contiguous pages produce one protect call; differences,
reordered/gapped addresses and absent mappings split correctly. Injected HAL
failure after changing one PTE leaves every writable page restorable. Existing
VM mutator waits/COW/lifetime and input/full-unmap regression remain passing.
Evidence: q240-vm1 source manifest and logs. Supported default amd64/PCAT/PC98
and experimental amd64 disk-image builds all exit 0; logs are
`/tmp/zedbsd-q240-amd64.log`, `-pcat.log`, `-pc98.log`, `-amd64-view.log`.
Experimental image/config/source hash retained in q240-experimental.

Native QEMU off1/on1 both PASS unchanged q239 helper, exact counters, actual
guest store after lease, append, limit, fsync/readback; source images unchanged.
Eight samples / 128 MiB overwrite totals:

| Mode | Copied input | View | Guest CPU | Guest elapsed |
| --- | --- | --- | --- | --- |
| Default | 128 MiB | 0 | 3.06 s | 3.06 s |
| Batched input view | 0 | 128 MiB | 3.43 s | 3.45 s |

CPU is about 12% above baseline despite eliminated staging copies. The prior
q239 view used 3.87 s, but cross-queue comparisons are not isolated hardware
cost attribution. Preserve default-off policy. Native evidence is in
q240-off1/on1 result/guest/argv files and q240-comparison.json. No physical
hardware or statistical-significance claim from these single-boot samples.

p028 remains uncleared. Input semantics now work and its adoption decision
remains off for this workload. Next address the unimplemented output contract:
eligible coherent reads must preserve every unconfirmed suffix byte after
short/error completion before any writable user view can be exposed. Do not
silently treat input-only implementation or a fallback stub as phase completion.

## q241: coherent read prefix primitive (2026-09-10)

Source audit found a direct-output blocker: object_cache_read_missing gives
its destination directly to a backend when miss-run preparation fails. A
backend can modify that buffer before returning error/short. Generic coherent
read therefore cannot justify writable user output merely by its name.

Added vm_object_read_coherent_prefix sharing the actual existing read loop,
with strict refusal of that fallback after releasing scratch/prepared pages.
Resident and successful staged misses publish only confirmed bytes; later
failure returns an already completed prefix. Ordinary coherent reads retain
their existing low-memory fallback. No file transaction or syscall invokes
the new strict primitive yet.

Actual VM/file fixture passes ordinary and ASan/UBSan/leak checks, 45,660 each.
New cases use a backend that writes past a short result or dirties the entire
buffer before error: strict output keeps its unconfirmed suffix unchanged.
Scratch refusal leaves the whole destination unchanged without backend entry;
a positive control through the generic API observes the hostile modification.
A resident first page plus later failed miss returns exactly 4096 confirmed
bytes; complete and 5000-byte short reads also verify content and untouched
tails. All pool/disk/file/cache ownership retires. Evidence:
`temp/q241-prefix2/source.json`, ordinary/sanitize logs and result.json.

Sequential supported disk-image builds all exit 0:
`/tmp/zedbsd-q241-amd64.log`, `/tmp/zedbsd-q241-pcat.log`,
`/tmp/zedbsd-q241-pc98.log`. No native output or performance claim.

p028 remains uncleared. [Output integration design](output-prefix-design.md)
records the strict pinned-cache file entry, full-unmap writable uaccess view,
pre-file-lock acquisition and completion/suffix requirements. Next implement
that transaction boundary and test refusal rather than raw fallback; then
connect read/pread and perform native acceptance/measurements.

## q242: strict file transaction entry

`file_io_transfer_prefix` accepts READ/PREAD with internal flags zero, pinned
read_object and held coherent content lease. Unsupported transactions return
ENOTSUP before touching output. Shared transfer implementation preserves existing
position/readahead/completion accounting and forbids raw fallback for this entry.

Actual VM/file host fixture exercises both READ and PREAD with hostile backend
errors, 5000-byte short reads, no scratch, resident-prefix/later failure and full
reads. Internal VM-object transactions and noncoherent state are refused without
backend calls or output/position changes. Confirmed bytes alone advance position;
PREAD preserves descriptor position; completion preserves the result.
Ordinary: 191650 checks; ASan/UBSan/leaks: 191654 checks (concurrent checks vary).
Evidence: `../temp/q242-prefix3/{source.json,ordinary.log,sanitize.log}`.
Earlier fixture attempts failed during compilation (misplaced loop) and READ
setup (fixture lacked read callback); both fixture issues corrected before PASS.

Three supported disk-image builds exited 0:
`/tmp/zedbsd-q242-amd64.log`, `/tmp/zedbsd-q242-pcat.log`,
`/tmp/zedbsd-q242-pc98.log`. No native output acceptance claimed.
Next: writable uaccess ownership before file locks, syscall read/pread integration,
and native correctness/CPU comparison. p028 remains uncleared; input default off.

## q243: writable uaccess view

`uaccess_output_view_acquire` requires an aligned full WRITE pin (up to 64 KiB).
The existing input/output preparation shares HAL rollback and retirement, but
output acquires the full-unmap VM lease and a writable borrowed mapping. All
captured private owners are marked dirty before publishing output_address,
including owners without live mapped aliases. Input exposes only const address.
Busy retirement restores the mapping pin and preserves both pointers/ownership.

Focused actual uaccess tests with controlled VM/HAL collaborators pass ordinary,
ASan/UBSan/leaks, and absent optional HAL symbol variants. Both directions check
lease selection, protection, failures at each acquisition stage, duplicate acquire,
busy-release retry, and preserved parent pin. Output checks dirty publication and
writes surviving release. Evidence: `../temp/q243-view2/` with source manifest and
three logs. This fixture verifies orchestration, not real VM/HAL or native output;
previous actual VM lease evidence remains separate.

Three supported disk-image builds exit 0:
`/tmp/zedbsd-q243-amd64.log`, `/tmp/zedbsd-q243-pcat.log`,
`/tmp/zedbsd-q243-pc98.log`. No output syscall uses this API yet.
Next connect READ/PREAD behind an experimental default-off switch, acquiring
output view before file locks; use strict file transfer, stage on unsupported
transactions without dropping user ownership until completion. Then native
correctness and copy/CPU measurements. p028 remains uncleared.

## q244 partial result and user HAL review

READ/PREAD output views and confirmed copy/view byte counters (I/O stats v10)
implemented behind default-off ZEDBSD_USER_OUTPUT_VIEW. Actual extracted syscall
stories pass with flags off/on, including short/error/EOF/fallback/position/suffix
and cleanup. Source manifest and log: `../temp/q244-syscall/`.
Native output-enabled QEMU q244-on1 passed READ/PREAD, EOF suffix, unaligned and
fork/COW/exit; 128 MiB confirmed view, zero copyout, CPU 2.59 s, wall 2.60 s.
No baseline comparison or performance improvement claim. Guest/image hashes and
8 samples in `../temp/q244-on1/result.json`; source image unchanged.
Experimental and default amd64 builds passed; default image restored.
PCAT/PC98 q244 builds not run. No live build/QEMU remains.

User requested architectural review before proceeding. [HAL review](hal-vmap-review.md)
confirms duplication: current hal_page_* reject HAL_SPACE_SYS, but the correct
boundary is to complete that existing path and move lifetime/VA allocation to
common VM, not grow a separate HAL subsystem. q244 is uncleared; resume after
replacement design/queue addresses all scratch/DMA/uaccess consumers. No production
refactor performed in response to this review-only request.

## q265: stage timing unavailable on current guest

Read current syscall paths: successful input view already skips staging-pool
borrow; no redundant input staging allocation to remove. Added link-only four
stage wrappers (input lease acquire, kernel borrow/release, lease release) using
existing hal_rtc_read_counter. No production/API changes or weakened leases.

Probe build passes, but native profile fails at first clock sample before stage
measurements. Guest advertises invariant=0, no TSC ratio/crystal; all CPUs report
TIMECOUNTER unavailable. hal_rtc_read_counter correctly returns false. This is
a failed instrumentation prerequisite, not evidence of an input-view regression.
No stage costs were obtained; do not estimate them from this failed run.
Evidence ../temp/q265-profile/{result.json,guest.log}, /tmp/zedbsd-q265-profile.log.

Forced ordinary build restored (/tmp/zedbsd-q265-default.log exit 0), no __wrap_
symbols remain. Experimental image retained at q265-probe-image. QEMU terminal.
Resume by verifying a supported precise-counter guest configuration or by
count-only instrumentation to attribute map/lease operations first. Do not bypass
HAL counter validation or treat coarse 10-ms samples as precise per-stage timing.
p028 remains uncleared; no production default changes.

## q266: clock-free operation attribution

Replaced unavailable stage timing with link-only external HAL call/page counts.
Native input oracle passes; ../temp/q266-count/{result.json,guest.log,operations.json}.
At 2048 completed input leases: SYS map 18432 calls / 32768 pages, user protect
2048 / 32768, SYS unmap 2048 / 32768, user unmap zero. Thus observed mean SYS
map calls is 9 per 16-page view, despite q252 contiguous-run coalescing. Physical
fragmentation limits grouping. Counters start with input acquisition and include
concurrent external calls; not CPU attribution or hardware shootdown counts.

This identifies a concrete remaining cost candidate: multiple map publications
for scattered pages. Next inspect whether each publication needs the existing
TLB synchronization when installing previously absent entries, against actual
architecture/rollback guarantees; alternatively a future vector-based common I/O
path could avoid temporary contiguous aliases. Any new HAL API requires approval.
Do not weaken content leases or enable direct views based on call counts alone.

Probe/default builds pass (/tmp/zedbsd-q266-build.log, -default.log); ordinary
vmunix has no wrappers. Experimental image retained separately; runtime terminal.
No production source changes; p028 stays uncleared/default off.

## q267: retained input flag is not a permission certificate

Source audit and actual VM lease regression completed; p028 remains uncleared.
`vmspace_user_lease_acquire` sets INPUT_PROTECTED before HAL protection so a
partially failed transition can recover on a later write fault. Therefore the
flag may be present even when the HAL rejected the transition. Additionally,
`vmspace_protect_locked` applies region/COW permissions without clearing this
flag, and `vm_page_effective_prot` masks WRITE only for COW. A retained flag
cannot certify current hardware readonly permissions after lease retirement.
Do not optimize input acquisition by skipping protection based on this flag.

Extended actual `user-lease-host.c`: fail the first input protection, verify
lease cleanup and retained flag, then retry and require all four cross-space
aliases to be protected. Ordinary PASS; sandbox sanitizer run hit documented
LSan ptrace limitation; same sanitizer binary outside sandbox PASS with leak
detection and UBSan halt enabled. Evidence: `temp/q267-lease/ordinary.log`,
`sanitize.log` (environment failure), `sanitize-unsandboxed.log`, `source.json`.
No production or HAL interface changes, no full suite or QEMU rerun needed.

Next: audit internal HAL permission publication under its existing serializer
for redundant transitions, including stale-translation acknowledgement and
A/D observations; alternatively evaluate direct page-vector transfer. Any
optimization must preserve these contracts and then undergo controlled native
CPU comparison; flags alone or operation counts are not adoption evidence.

## q268: identical amd64 PTE permissions

Implemented in `hal_space_prot_query`: avoid writing an identical leaf. Track
actual changed leaves; skip shootdown only when all leaves were unchanged and
`flags == NULL`. Preserve full-range validation, existing A/D bits, and final
observation. `flags != NULL` continues to force the existing acknowledgement
boundary, including unchanged permissions. No public HAL declarations changed.

Correctness argument from current source: `space_lock_enter/leave` serialize
mutations; map, unmap, protection and clear-flags complete their invalidation
before releasing ownership. Thus a matching PTE under that lock does not stand
for a still-pending earlier permission transition. Hardware A/D changes are
preserved; they cannot change the requested protection. Any actual permission
or cache/execute-attribute change still invalidates before unlock.
Architectural reference: [Intel SDM Vol. 3, section 4.10.4](https://cdrdv2-public.intel.com/789582/325384-sdm-vol-3abcd.pdf).
The implementation does not apply optional invalidation exceptions to changed
permissions or cleared A/D bits; the omitted operation changes no PTE at all.

Focused extracted-actual-HAL fixture passes SYS/user identical permissions,
NX transition, mixed changed/unchanged leaves, flag-query acknowledgement,
invalid-range prevalidation and existing rollback/query checks. Ordinary PASS;
ASan/UBSan PASS using the same binary outside sandbox (sandbox LSan ptrace
failure retained). Evidence: `temp/q268-space/{source.json,ordinary.log,
sanitize.log,sanitize-unsandboxed.log}`. Host fixture is not SMP/native proof.

All three ordinary image builds PASS: `/tmp/zedbsd-q268-amd64.log`,
`/tmp/zedbsd-q268-pcat.log`, `/tmp/zedbsd-q268-pc98.log`. q268 ends with
p028 uncleared: controlled native input flag-off/on comparison must follow
on this source. Both direct-view defaults remain disabled.

## q269: native input after q268

Both eight-sample QEMU input runs PASS, same guest SHA256 and unchanged source
images. Off: 134217728 copied bytes, 0 view bytes, CPU 2780000 us. On:
0 copied bytes, 134217728 view bytes, CPU 5200000 us. CPU ratio 1.871.
Data/readback, post-lease writes, alignment, append and short-limit oracle PASS.
Evidence: `temp/q269-{off,on}/result.json`, guest logs;
`temp/q269-images/comparison.json` retains source hashes and aggregate metrics.
Ordinary restore build PASS (`/tmp/zedbsd-q269-default.log`), no probe wrappers.

This is functional acceptance of q268 on this workload, not performance adoption.
The on result is also above q253's historical 4680000 us, but those runs are not
a controlled before/after pair; do not attribute that difference to q268.
Both direct-view defaults remain off. Next audit absent-PTE publication and
its per-fragment shootdown: q266 measured nine SYS map calls per input view.
No new vector HAL API is approved; distinguish publication from retirement
and prove table lifetime / invalidation invariants before changing the path.

## q270: non-executable fresh mappings avoid shootdown

`hal_space_map` now omits success-path invalidation for non-executable mappings.
Executable mappings retain instruction serialization. Whole-range absent-leaf
validation, overlap rejection, partial rollback, unmap and detached-table
retirement are unchanged. No public HAL declarations changed.

[Intel SDM Vol. 3 section 4.10.4.3, page 4-51](https://cdrdv2-public.intel.com/789582/325384-sdm-vol-3abcd.pdf)
permits P=0 to P=1 without invalidation, provided prior removal was invalidated.
Current source satisfies this under the same space serializer: prior unmap and
rollback finish shootdown before freeing detached tables/unlocking. Newly
allocated tables are initialized before linking. Instruction serialization
remains explicit for executable maps; this is not a relaxation of retirement.

Extracted actual HAL fixture ordinary and ASan/UBSan PASS: fresh data map has
no shootdown; executable map retains it; partial rollback/unmap still flush;
VA reuse maps new PA; retained protection/query/overlap checks pass. Evidence
`temp/q270-space/`; sanitizer sandbox ptrace failure retained, same binary
outside sandbox passes leak detection. amd64 off/on/default builds PASS in
`/tmp/zedbsd-q270-{off-build,on-build,default}.log`.

Both 4-CPU native input oracles PASS, same guest hash/source images unchanged.
128 MiB: off CPU 2820000 us (all copied), on CPU 3210000 us (all viewed).
Ratio 1.138; on is below q269 historical 5200000 us, but across-cycle timing
is not a paired controlled causal estimate. Evidence `temp/q270-{off,on}` and
`temp/q270-images/comparison.json`. Ordinary restored, no wrappers.

p028 remains uncleared: CPU adoption condition unmet; both defaults off.
Next strengthen actual multi-CPU mapping/reuse/rollback verification with the
existing kernel-map native probe on this source, then assess output direction
and remaining input ownership costs. Current 4-CPU workload does not prove
every cross-CPU translation lifetime race. No need to repeat unchanged non-amd64
builds for this amd64-only internal implementation change.

## q271: actual SMP publication / VA reuse regression

8 GiB/4 CPU QEMU PASS on q270 source. Extended existing native probe explicitly
requires identical VA and different first PA across successive rounds. Three
rounds each map sixteen fragmented >4 GiB frames, verify existing/new user
spaces and AP notification readers, then unmap and verify free-byte balance
(8574959616 bytes each round). Two real subordinate-table allocation failures
roll back without a leak. Fragmented scratch fallback and root login PASS.
Evidence `temp/q271-smp/{result.json,guest.log,argv.json}` includes source hashes
and unchanged source-image proof. Image retained under `temp/q271-image`.

Probe build `/tmp/zedbsd-q271-probe-build.log` and forced ordinary restore
`/tmp/zedbsd-q271-default.log` PASS; nm confirms no __wrap_ symbols. No production
changes this queue. This tests sequential VA reuse with acknowledged AP readers,
not exhaustive arbitrary concurrent access after ownership expiry. p028 remains
uncleared on performance; next compare the output direction on the optimized HAL
with its existing short/EOF/COW oracle before deciding further ownership work.

## q272: output direction after HAL synchronization optimization

Both eight-sample native output runs PASS with identical guest binaries and
unchanged source images. 128 MiB read/pread CPU: off 850000 us, on 2010000 us
(ratio 2.365). Off accounts all bytes as copyout; on all as output views.
EOF/short-read untouched suffix, unaligned read, fork COW and exit oracle PASS.
Evidence `temp/q272-{off,on}/result.json`, guest logs and
`temp/q272-images/comparison.json`. Ordinary restore build PASS
(`/tmp/zedbsd-q272-default.log`), no probe wrappers. No production change here.

Both directions remain default off; output performance is not accepted.
Next inspect lease acquisition's per-page output unmap: input adjacent aliases
already batch protection, while output invalidates one page per call. Evaluate
batching compatible adjacent aliases under the existing busy/region lifetime
reservations, preserving partial-failure and mapped-flag reconciliation; verify
actual VM fixture before native comparison. This avoids adding a new HAL API.

## q273 output alias unmap runs

Common VM now groups adjacent mapped aliases in the same vmspace for output
unmap, using the existing HAL range operation. Input retains grouping by
readonly permissions. Every alias stays busy and holds its region/backing
through retirement; all members of a successful run clear MAPPED. Earlier
successful runs remain reflected when a later range is refused.

Reviewed all current HAL unmap implementations (amd64/i386/arm64/m68k/sparcv9):
recoverable error returns precede mutation; admitted ranges finish retirement
and return success. Thus a refused run retains mapped metadata; this relies
on the current implementations and must be revisited if partial-error unmap
is introduced. No new interface or architecture-specific branch.

Actual VM lease fixture ordinary and ASan/UBSan PASS: four adjacent aliases
produce one range unmap, gaps split runs, absent aliases are skipped, first
and second-run failure preserve exact mapped state and release busy/region/
backing ownership. Existing cross-space and input refusal checks remain PASS.
Evidence `temp/q273-lease/` (sandbox LSan ptrace failure retained; same binary
passes outside sandbox). Three image builds PASS in
`/tmp/zedbsd-q273-{amd64,pcat,pc98}.log`. No native comparison yet on this
change; p028 uncleared/default off pending existing output oracle off/on.

## q274: native output alias batching acceptance

Both output off/on oracles PASS on q273 common VM source; same guest hash,
source images unchanged. 128 MiB CPU off 850000 us, on 1560000 us (ratio 1.835).
Exact copyout/view accounting, short/EOF suffix, unaligned and fork COW PASS.
Compared with q272 historical on 2010000 us this is lower, but the across-cycle
comparison is not a controlled paired timing attribution. Adoption remains
unmet; both direct directions remain disabled by default. Evidence
`temp/q274-{off,on}/result.json`, guest logs, `temp/q274-images/comparison.json`.
Builds off/on/ordinary restore PASS (`/tmp/zedbsd-q274-*.log`), no wrappers.

Remaining output work: measure pin/fault/remap and lease/map retirement costs
before further changes. Full user-alias revocation remains part of the current
ownership contract; do not remove it solely to make the benchmark pass. A
page-vector transfer design must be assessed against the agreed I/O ownership
plan, not introduced as an unreviewed new HAL interface. Native input regression
for shared alias grouping can be combined with the next justified input change;
existing cross-space/input refusal host checks passed q273.

## q278: output staging is a reserve borrow, not allocation

Actual `syscall_regular_buffer` invokes `io_pool_borrow`; io.c uses preallocated
slots, without allocation/wait/VFS/VM entry. Successful input view already avoids
the borrow, output retains it for prefix ENOTSUP/ENOMEM fallback. Existing
`storage-syscall-stories.c` explicitly asserts no file transaction is held when
borrowing. A lazy borrow would change this tested ordering and require handling
capacity shrink without mistaking it for EOF/short transfer. No evidence yet
attributes material CPU to this reserve borrow, so no production change made.

A distinct cost remains visible in source: output acquisition revokes user
aliases; `vmspace_user_lease_release` releases backing/mapping reservations but
does not restore PTEs. Later uaccess pin/fault paths must republish them. Do not
restore user access while the borrowed kernel writer is active. Before modifying
release, quantify next-pin faults/maps separately from view acquisition/release.
The existing input profiler activates only on input leases and cannot establish
output attribution unchanged. A next bounded test-only step should activate on
output lease entry and count user map/unmap plus pin calls, with unchanged
output oracle and ordinary restoration. Timer unavailability from q265 still
applies; operation counts alone do not prove time savings.

No repeat runtime/build required for this source audit; p028 remains uncleared.
This rejects an unmeasured ordering change, not direct-I/O completion.

## q279: output pin/map counts

Link-only probe and unchanged native output oracle PASS; source image unchanged.
At 2048 completed leases (64 KiB each), external calls after first acquisition:

| Kind | Calls | Pages |
| --- | ---: | ---: |
| SYS map | 16384 | 32768 |
| user map | 32754 | 32754 |
| SYS unmap | 2048 | 32768 |
| user unmap | 2048 | 32768 |
| user pin | 2229 | 32934 |

User remapping is approximately sixteen single-page calls per view; grouped
unmap is one per view. SYS publication averages eight physical runs. Pin count
includes other calls (stats/console etc); activation excludes the initial pin.
These are aggregate operation counts, not CPU-stage attribution or strict
per-syscall isolation. Evidence `temp/q279-output/{result.json,operations.json,
guest.log}`, probe image `temp/q279-image/probe.img`. Ordinary forced relink
PASS (`/tmp/zedbsd-q279-default.log`), nm confirms no wrappers. No production
changes. Native data/EOF/COW oracle passes under instrumentation.

Next design candidate: restore only aliases this lease actually revoked, after
kernel writer retirement and before dropping its reservations. Preserve original
COW/effective permissions, do not populate previously absent aliases, and allow
failed restoration to remain faultable without turning a successful I/O into a
false failure. Acquisition-failure unwind must be distinct from completed-output
release where necessary. Count evidence motivates this audit but does not prove
that eager restoration outperforms faults; require controlled CPU comparison.

## q280: best-effort restoration after output writer retirement

Lease records only aliases it successfully revoked for output. Release restores
compatible consecutive VA/PA runs with vm_page_effective_prot (COW retained),
while busy/region/backing reservations still stabilize metadata. uaccess caller
retires borrowed writer first; failed acquisition has no published writer.
Original absent aliases and input leases are not restored. A failed map leaves
that run absent for normal later fault handling, without changing I/O result.
Current HAL range map failure paths roll back partial publication.

Generic VM implementation and kern/vmspace.h contract comment updated; no HAL
public API change. Actual lease fixture tests contiguous run, COW split, original
absent alias and map refusal, with reservations held throughout map callback
and ownership balanced after release. Existing input/output refusal checks pass
with optional restoration refused by the controlled HAL. First test compilation
found signedness in a fixture mask; corrected to uint32_t. Final ordinary and
ASan/UBSan/leak checks PASS (`temp/q280-lease2`; sandbox LSan ptrace failure
retained and same binary passes outside sandbox). Three image builds PASS:
`/tmp/zedbsd-q280-{amd64,pcat,pc98}.log`.

No native runtime/CPU acceptance yet on this change. Next run unchanged output
short/EOF/COW off/on oracle and compare CPU. Restoration may move costs rather
than reduce them; defaults remain disabled until adoption evidence.

## q281: native output restoration acceptance

Current q280 source passes unchanged output oracle off/on with identical guest
hash and unchanged source images. 128 MiB CPU off 830000 us, on 1230000 us
(ratio 1.482). Exact copy/view totals, EOF/short untouched suffix, unaligned
read and fork COW PASS. Ordinary restore build PASS, no wrappers. Evidence
`temp/q281-{off,on}/result.json`, guest logs, `temp/q281-images/comparison.json`;
build logs `/tmp/zedbsd-q281-{off-build,on-build,default}.log`.

On is below historical q274 1560000 us, but this cross-cycle comparison alone
is not a paired causal estimate. q280 is functionally accepted on this native
workload; default adoption still lacks a CPU win. Both directions remain off.
Remaining investigation must distinguish transfer/cache-copy expense from
ownership overhead before another change. Do not weaken COW, alias lifetime
or prefix semantics to force a favorable benchmark.

## q282: remove destination shape constraint, preserve transfer batching

Source audit identifies two output publication copies in common VM: resident
page and confirmed scratch prefix. File/VM currently take contiguous pointers,
forcing temporary SYS mappings despite stable pinned-page kernel addresses.
Per-span file calls would regress miss batching; rejected. Added concrete
[page-vector output design](page-vector-output-design.md) retaining one file/VM
transfer, all content/media/prefix guards and current bounds. Next implement
bounded destination helper plus actual VM consumer; no HAL/API approval needed
outside this internal kernel scope. No production change or repeated tests in
this design queue. p028/performance acceptance remains uncleared.

## q283: shared destination / actual VM strict read

Added bounded borrowed kernel spans in io-destination.h and io.c with full
shape/capacity/pointer/sum overflow validation before publication, and offset
copy across span boundaries without allocation or locks. VM destination entry
shares object_read_coherent/object_cache_read_missing; resident and confirmed
scratch prefix copy sites accept spans. Raw backend fallback remains forbidden
for strict output; generic contiguous read behavior retained. No HAL changes.

Actual VM/file prefix fixture selected via WS025_PREFIX_ONLY=1 passes ordinary
and ASan/UBSan/leaks outside sandbox; sandbox LSan ptrace failure retained.
Scattered destination split at3000 bytes verifies hostile error/short suffix,
scratch refusal and resident-prefix failure. Full8 KiB miss still uses exactly
one backend call. Existing contiguous VM and file prefix cases also pass.
This is not the whole exec-snapshot suite or64 KiB native acceptance.
Additional helper test linked against actual io.o passes nonzero offset, empty
span, exact end, rejected later span before any write, pointer/sum overflow.
Evidence temp/q283-vm including scope.json. Three image builds PASS:
/tmp/zedbsd-q283-{amd64,pcat,pc98}.log.

Next connect file_io strict destination to this VM entry and validate transaction
offset/completion/fallback invariants. uaccess/syscall integration and64 KiB
backend/native CPU tests follow. New VM entry works but is not yet selected
by production syscall; p028 remains uncleared/default off.

## q284: strict file destination entry

file_io_transfer_destination validates spans and delegates to the existing
file_io_transfer_impl with strict prefix semantics. It selects the new VM
destination entry while retaining read identity/content guards, retry policy,
position accounting and completion. Non-read/internal/noncoherent transactions
are rejected before publication; raw backend fallback stays forbidden. Existing
contiguous entries use the same implementation with no destination descriptor.

Actual file/VM targeted prefix fixture PASS ordinary and ASan/UBSan/leaks
outside sandbox: scattered READ/PREAD, hostile short/error, no scratch, resident
prefix/later failure, exact position and completion. New64 KiB fixture copies
into16 reversed page spans with exactly one backend call and exact data.
Evidence temp/q284-file/{scope.json,source.json,ordinary.log,
sanitize-unsandboxed.log}; sandbox LSan ptrace failure retained. No full exec
fixture replay. Three image builds PASS in /tmp/zedbsd-q284-{amd64,pcat,pc98}.log.

Next uaccess/syscall integration must retain output leases without creating a
temporary SYS mapping, preserve pin lifetime/fallback and remove the superseded
output mapping path. Native functionality/CPU adoption remains unproven; both
direct-view defaults off. No public HAL declarations changed.

## q285: mapping-free uaccess output owner

Added uaccess_output owner with bounded spans, immutable pinned kernel addresses
and exclusive output lease. Rejects malformed/nonprivate/unaligned pins before
lease, marks every captured private owner dirty after acquisition, publishes
self-owned descriptor pointers correctly, releases only after final destination
access/file completion. No temporary SYS mapping, no optional map capability
dependency. Parent view_active excludes duplicate leases until release.

Actual uaccess.c fixture with no map symbols passes ordinary and ASan/UBSan/
leaks outside sandbox: invalid metadata, permission, null page address, lease
refusal, reversed spans, dirty counts, duplicate acquire/release and preserved
parent pin. Evidence temp/q285-output; sandbox LSan ptrace failure retained.
Three image builds PASS in /tmp/zedbsd-q285-{amd64,pcat,pc98}.log.

Existing mapped output view is temporarily retained for current syscalls and
will be removed during the next switch. Need logical-offset slicing of spans:
SYSCALL_IO_CHUNK is512, so pool exhaustion must not assume page-aligned done
or chunk. Implement validated destination slicing, switch read/pread, preserve
staging fallback and complete-before-release, update actual syscall fixtures
and remove superseded output map code. Native acceptance remains pending.

### q286: syscall output destination switch

READ/PREAD now borrow pinned-page destination spans under the exclusive output
lease; file completion precedes output release. Removed the old mapped-output
API and field; input mapping remains. Destination slicing preserves offsets for
512-byte staging-pool exhaustion. Default output/input experimental flags remain0.

Focused actual syscall extraction PASS with ASan/UBSan for default and enabled
paths: short/error/EOF/refusal, positional preservation, cleanup, and128 successive
512-byte output slices, both direct and ENOMEM/ENOTSUP fallback. Actual io.c slice
fixture ordinary/sanitizer PASS: cross-span offsets, empty range, invalid late
span and overflow. Actual uaccess input fixture ordinary/sanitizer/absent borrow
PASS; new output fixture ordinary/sanitizer PASS. Sandbox LeakSanitizer ptrace
failure retained; elevated syscall runner PASS. Supported amd64/pcat/pc98 image
builds PASS. Evidence: temp/q286-destination, temp/q286-input, temp/q286-output.
Production src/include search has no output_address/uaccess_output_view_acquire.

This closes the bounded implementation queue, not p028 adoption. Next: native
output correctness and paired CPU comparison against the current default image.
No native timing claim for this change yet; p028 remains uncleared.

## q287: ordinary native boot failure before measurement

Current default image reached login, but root/empty-password attempt reported
Login incorrect, then getty exec of /bin/login repeatedly returned ENOSPC.
Runner timed out waiting for root shell, terminated its QEMU and retained
result.json/guest.log in temp/q287-off. No performance samples or enabled run.
Host disk has642GiB available; generated rootfs shadow root entry is empty and
unlocked (checked without exposing contents). This does not prove guest image
credentials or identify ENOSPC origin. Current default still off; no enabled
artifact was built.

Next bounded diagnosis must identify guest ENOSPC source (exec/backend claim or
filesystem/resource path) and distinguish current kernel from image contents.
BACKING_CLAIM_MAX=16 and claim_insert can return ENOSPC, but this is only a
candidate, not a proven cause. Preserve original limits and ownership until
evidence identifies the failure. Resume pair only after normal root login.

## q288: ENOSPC not reproduced with diagnostic or restored kernel

Temporary ENOSPC-expression diagnostics in backing claim, VM, tmpfs, quota,
mount, writeback, swap, devfs and UFS were compiled, run, and fully removed.
These expressions include initial values/comparisons, so emitted UFS lines
do NOT establish actual error origin. Diagnostic QEMU login and complete output
guest PASS; restored ordinary QEMU also PASS, copy134217728/view0, CPU840000us.
Both source images unchanged. Evidence temp/q288-trace and temp/q288-ordinary.

Extracted vmunix from failed q287 disposable image and current ordinary source
has identical SHA256856b4f54f4e7dc666e7554c7b36f8b3000472ef43c855d737ef4ef5e55c7186c.
Thus no kernel fix or stale-kernel-build explanation is proven. Guest data/image
state and timing remain candidates. Ordinary rebuild PASS; source search finds
no diagnostic macro/message. q288 diagnosis remains uncleared with reproducible
artifact/log evidence; do not mark the intermittent failure fixed.

Resume diagnosis if reproduced using origin tracing also covering overlayfs
and loop error sites, which this diagnostic did not cover. The normal-output
cell is now measured; enabled-output functionality can proceed, but intermittent
boot failure must remain explicit in acceptance limitations.

## q289: native output spans correct, CPU benefit not established

Enabled output spans PASS native read/pread, EOF, untouched suffix, unaligned
fallback, fork/COW/exit and eight16MiB samples. Copyout counter0/direct134217728;
CPU870000us, wall860000000ns. q288 ordinary copy134217728/direct0, CPU840000us.
Guest executable SHA256 identical; both source disks unchanged. Enabled run
reached root login without q287 failure. Evidence temp/q289-on/result.json and
comparison.json against temp/q288-ordinary.

CPU ratio1.036 does not establish benefit. Do not infer a reliable3.6% regression
from one noisy pair, nor infer adoption from near parity. Earlier q281 output
map timings are historical different-build results, not this controlled pair.
Ordinary amd64 image rebuilt successfully after enabled test; no source default
changed. q287 intermittent login/exec ENOSPC remains unresolved.

Next source-backed candidate: io_destination_copy validates every span and
rescans from span0 for each resident page while object lock is held. The full
destination was already validated at file and VM entries; immutable descriptor
lifetime is guaranteed by output owner. Consider a validated cursor initialized
once per VM operation, consuming sequential confirmed prefixes, without removing
public validation or allowing unchecked destination APIs. Measure only after
focused cross-span/short/error and ownership tests; no attribution claim yet.

## q290: validated sequential destination cursor

Coherent VM read now captures a validated16-span descriptor once per operation.
Resident-page and confirmed miss-prefix copies consume the same cursor, without
rescanning/revalidating all spans under each object lock. Cursor owns descriptor
metadata only; existing output lease and pins still own write exclusion/residency.
Random-offset public copy remains fully validated; no unchecked API introduced.
Cursor rejects oversized/null/overflow source requests before state/data changes.
Initialization clears state on invalid descriptor; source descriptor edits do
not alter captured spans. EOF and refused miss do not advance the cursor.

Actual VM/file focused prefix and64KiB16-reversed-span/one-backend fixtures PASS
ordinary and ASan/UBSan (scope.json explicitly limits run). Actual io.c cursor
fixture PASS ordinary/sanitizer: uneven/empty spans, multi-step copies, exhausted
cursor, invalid init, request rejection and metadata independence. Evidence
temp/q290-vm and temp/q290-destination. All three image builds PASS; logs retained.

Defaults remain0; native CPU comparison after this change is still required.
No performance attribution or p028 completion claimed; q287 intermittent boot
ENOSPC remains open. Next run ordinary/enabled same-guest pair and restore normal
artifact, retaining failure evidence if login issue recurs.

## q291: cursor native comparison, no adoption benefit

Ordinary and enabled cursor builds both PASS native read/pread, EOF/suffix,
unaligned fallback, fork/COW/exit. Identical guest hash, eight16MiB samples each,
source immutability verified. Ordinary CPU860000us/wall840000000ns with134217728
copyout bytes; enabled CPU960000us/wall960000000ns, copyout0/direct134217728.
Evidence temp/q291-{off,on}/result.json, on/comparison.json and ordinary-restore.log.

Both booted normally; this does not resolve q287 intermittent ENOSPC. Ordinary
amd64 rebuilt successfully; defaults remain off. One noisy pair does not quantify
a reliable cursor regression against q289, but neither comparison demonstrates
the required CPU benefit. p028 remains uncleared.

Next bounded work: measure remaining ownership cost before another optimization.
Compare timed pin, output lease acquire/release, file transfer and copy paths
with disabled/enabled modes. Use non-overlapping totals or explicitly document
nested time to avoid double counting; separate instrumented timing from adoption
measurements. Existing q279 operation counts preceded output SYS-map removal
and alias restoration, so cannot be reused as current attribution. Preserve
all pin/COW/unmap guarantees; no permission bypass to achieve benchmark speed.

## q292: precise timing prerequisite unavailable; operation profiling selected

Revalidated q265 rather than rerunning known-failing hal_rtc_read_counter probes.
Current host lacks /dev/kvm. Diskless paused QEMU10.0.11 with max,invtsc=on
explicitly warns TCG does not support CPUID.80000007H:EDX.invtsc. QMP query and
quit complete exit0; argv/stdout/stderr/result retained in temp/q292-prerequisite.
No disk or production artifact changed. Current HAL frequency qualification
requires invariant counter metadata; do not override it or treat10ms ticks as
precise per-stage timings. No stage-time figures obtained.

Next finite profiling queue: compile existing tests/output-profile-kernel.c as
a link-only probe, enable output, count current SYS/user map and unmap plus
vmspace pin at2048 output lease completions using native guest. q279 predates
map removal/alias restore and is not current evidence. Record concurrent-caller
limitation, do not equate call counts with CPU costs. Restore ordinary via
forced relink and confirm no __wrap symbols. Use results to choose a concrete
source/ownership audit; if another method is needed for actual timings, establish
its clock prerequisite first. p028 remains uncleared; q287 boot issue remains.

## q293: current output external-operation profile

Existing link-only wrappers with enabled output run the native data/EOF/COW
oracle successfully. At2048 lease releases: SYS map0/pages0; user map18434/
pages32770; SYS unmap0/pages0; user unmap2048/pages32768; pin2225/pages32930.
Evidence temp/q293-count/{result.json,operations.json,guest.log}. Counts include
concurrent external callers after first lease, not exact isolated per-syscall
statistics or CPU-stage timing. Zero observed SYS maps supports actual removal
of temporary output mappings; user restore averages about9 calls per lease.
User aliases remain revoked during exclusive writes and restored after them.

Ordinary amd64 forced relink/image build PASS; nm confirms zero __wrap_ symbols.
No production defaults changed. Profile latency is instrumented, not adoption
evidence. p028 remains uncleared; output uninstrumented CPU benefit unproven.

Next source audit: input still uses temporary SYS mappings and has demonstrated
input CPU overhead (q2702.82s/3.21s). Inspect file/VM write-source consumers for
a borrowed-span path analogous to output while retaining input COW snapshot,
single file transaction and full64KiB backend batch. Document where a contiguous
backend still requires fallback before implementing. Do not remove user alias
protection merely to reduce calls, and do not claim input feasibility without
following the write-cache publication/rollback contracts.

## q294: input source feasibility recorded

Followed uaccess input mapping, shared file delayed/immediate write dispatch and
VM content commit. Source spans are feasible for delayed cache publication, but
backend requires contiguous fallback and VM commit walks unordered pages. Saved
[design](page-vector-input-design.md) with const borrowed source, arbitrary-offset
gather, pre-lock fallback storage, no mutation-then-refusal replay, maintained
append/growth/set-id/epoch and full64KiB transaction, bounded implementation gates.
No production code changed or new functionality claimed; p028 remains uncleared.
