# ws025-p018 results

Status: completed (q109), 2026-09-07. Existing allocated regular-file data writeback is opt-in; through remains the default. Final evidence and coverage appear below; earlier checkpoints retain their historical incomplete status.

Implemented the first credit stage in writeback.c/writeback.h and linked it into
platform builds. Four canonical device budgets independently limit optional
admission. A zero-initialized ticket reserves 64 KiB before leases; commit moves
page-aligned bytes to dirty ownership without allocating; release returns unused
credit. Quiesce prevents new tickets and detach refuses pending or dirty owners.
A live ticket cannot be overwritten by a second reservation. Global high/low
limits cap safely even at UINT64_MAX targets. The budget retains its leaf device.

`temp/p018-credit-2` passes ordinary/sanitizer tests, including 40000 concurrent
commits, per-device isolation, target shrink, quiesce/resume, admission refusal,
unused reservation return and final device-reference retirement. The first test
link accidentally used the target assert header with host libc; the pure host
runner now uses the host assert interface without weakening assertions.

VM pages now optionally retain a dirty-credit owner. Confirmed clean and legitimate
orphan destruction retire that credit; failed persistence does not. The owner
is separate from the physical page/slab charge. `temp/p018-dirty-credit-vm-3.log`
passes 413911 / 407482 file/VM/claim checks plus an injected failed fsync followed
by successful retry, asserting dirty credits survive failure and clear only after
success. Two initial fixture compile errors (wrong mark_dirty arity and missing
page-size header) were corrected. The intermediate amd64 build passed (`temp/p018-credit-vm-build.log`).

Remaining: mount policy/status control, private writable cache owner, eligibility,
coherent delayed content commit, bounded device filesystem syncer and batched
writeback, disable/unmount/shutdown drain, full WB/CACHE/native acceptance.
No delayed behavior or p018 completion is claimed by these first-stage tests.

## Coherent delayed commit, eligibility and batched mount drain

The private write-capable VM cache handle and delayed content transaction are
implemented. Preparation reads old bytes even for full-page overwrite, so abort
cannot publish zeros in place of the old image. Allocation refusal leaves no
published pages; repeated small writes retain one page credit, preserve ordinary
read coherence, and do not keep the user file description open. Failed persistence
retains the private writer and dirty bytes. `temp/p018-delayed-content.log` passes
411994 / 408871 checks (ordinary / sanitizer).

UFS/FAT now expose an allocated-existing-range query. UFS rejects holes, EOF growth
and read-only operation without writes; FAT validates the complete cluster chain
and rejects early tails/cycles. `temp/p018-ufs-range` and `temp/p018-fat-range-2`
pass ordinary/sanitizer checks. FAT's initial fixture used the wrong embedded inode
field name, corrected before rerun. `temp/p018-range-build.log` amd64 passed.

VM sync first revokes/captures its pages and then writes adjacent dirty pages in
at most 64 KiB runs. It preserves the first failure, including a revoke failure;
no later successful revoke can erase an earlier error. A failed data operation or
final barrier leaves captured pages dirty. Optional pool refusal takes the scalar
path without waiting. Callers can supply independent scratch; it is neither
borrowed again nor released by the VM layer.

Mount drains capture a monotonic registry-publication frontier, pin one inode at
a time and require no heap snapshot. New objects cannot indefinitely extend a
pass. Mount sync now drains VM data before filesystem barriers, and unmount drains
before testing references held by optional cache handles. Publication identity
saturation refuses new shared objects rather than wrapping.

`temp/p018-mount-drain-accept.log` passes 411655 / 410099 checks. The added test
compares 16 KiB adjacent data: one backend write with scratch versus four with the
shared pool unavailable; caller-reserved scratch restores one call even while
that pool is unavailable. Injected barrier failure retains all credits, and retry
persists identical bytes and retires credits. `git diff --check` is clean.
Production amd64 build is in progress (`temp/p018-drain-amd64.log`).

Remaining: mount policy/status control, file_io delayed admission/commit hookup,
per-device filesystem syncer, off/drain policy transactions, dedicated WB/CACHE
and native acceptance. Ordinary writes remain through; these primitives do not
by themselves enable an opt-in policy. p018 is still in progress.

### Native drain correction

The first QEMU run (`temp/p018-drain-native`) stopped at recursive inode I/O mutex
acquisition. A temporary link-only mutex wrapper reproduced the call in
`vm_object_sync_range_buffer`; no production diagnostic switch was added. The
cause was overlay internal `mount_sync` barriers reentering the VM mount drain
while its current inode was already being written back. `mount_sync_backend`
now provides the filesystem-only internal barrier. Overlay internal calls use it;
it does not certify a VM/mount frontier or consume the public error observer.
Real full `mount_sync` still drains VM and certifies its captured target. Existing
isolated overlay fixtures name the corresponding backend adapter explicitly.

After removing the link probe and rebuilding the ordinary kernel,
`temp/p018-drain-fixed-native` passes on 512 MiB QEMU xHCI USB root: normal I/O,
MAP_SHARED/msync, copy-up/truncate, warm reads (zero lower reads), cache pressure
and root sysctl shrink. The intermediate amd64 build passed. This fixes the
observed recursion; delayed mount opt-in/worker hookup remains unimplemented.

`temp/p018-frontier-backend` passes 77 checks in each ordinary/sanitizer variant.
New checks show internal success does not advance the mount's stable frontier,
internal failure does not consume its public cursor, and subsequent full sync
reports that failure before a clean repeat. Full FS50/Wi-Fi30/native regression
is running at WS018 `temp/ws025-p018-drain`.

Full regression completed: WS018 `temp/ws025-p018-drain/results.json` is FS50/50
PASS, with Wi-Fi30 ordinary/sanitizer and native USB acceptance. pcat build passed
(`temp/p018-drain-pcat.log`); pc98 build is running. No delayed policy is enabled.

The pc98 build also passed (`temp/p018-drain-pc98.log`). An additional production
VM/file test proves the full 64 KiB boundary: sixteen delayed pages are persisted
by exactly one backend call carrying 65536 bytes, with byte-for-byte verification
and all dirty credits retired. `temp/p018-64k-batch.log` passes 414708 / 409139
checks. Ordinary amd64 artifacts are being refreshed after the 32-bit build gates.

Next implementation boundary: complete mount policy and per-physical-device
workers before activating file_io delayed admission. Canonical physical budgeting
must resolve loop backing ownership explicitly; `disk_cache_acquire(loop)` alone
returns a loop cache domain and must not be mistaken for the physical leaf.
Keep separate lifetime pins for the mounted content and physical worker domain.
The filesystem syncer runs above the BIO worker, using its own scratch through
`vm_object_sync_mount_buffer`, so nested drain never depends on its own BIO queue.

Checkpoint closed with ordinary amd64 rebuild PASS (`temp/p018-checkpoint-amd64.log`),
clean `git diff --check`, and no mutex/disk-ioctl diagnostic wrapper in vmunix.
No test/build/runtime remains running. q109 / p018 and the WS025 goal stay active;
this is a verified implementation checkpoint, not phase completion.

## Physical writeback domains

`writeback_domain_acquire` now resolves partitions and explicit loop backing
references to one physical cache token, retaining intermediate lifecycle guards
until resolution ends. `loop_backing_disk_ref` recognizes only the loop driver,
rejects a detaching/missing backing and transfers an ordinary disk reference.
Neither routine changes logical disk ancestry or block offsets. Resolution is
bounded by the I/O context depth limit and rejects cycles without leaking pins.

`temp/p018-physical-domain` passes the production BIO/lifecycle fixture with
1196 / 1197 ordinary/sanitizer checks. New cases use the verbatim production loop
resolver and real disk cache admission to verify nested loops over a partition,
the same physical domain, detach refusal, a cyclic backing graph and exact token
retirement. The amd64 production build passed (`temp/p018-domain-amd64.log`);
`git diff --check` is clean and no test/build/runtime remains running.
Policy/control/workers and file_io hookup remain the next implementation step.

## Policy/worker and public file I/O implementation

Added `writeback-policy.c` and linked it into platform source lists. The bounded
mount table shares four physical workers. Enable obtains the real domain and
charged private 64 KiB + control page before publishing a retained mount. Threads
start lazily and are reused. The control mutex is separate from the short policy
registry guard; filesystem drains hold neither guard. Age and pressure schedule
passes with a bounded mount snapshot in the reserved control page. Independent
physical devices use independent payloads and threads.

Off pauses the device budget, waits for live tickets and worker activity without
file/content leases, and drains the target. Failure restores policy/admission;
HAL free failure retains charged memory for retry. Shared-device sibling mounts
keep their worker and dirty credits. Failed initial setup releases resources, or
retains an explicit bounded worker owner if resource release itself fails; a
subsequent on/off reuses and releases it. The background drain uses VM sync plus
filesystem-only barrier, avoiding consumption of the public mount error cursor.
`mount_sync_buffer` separately supports full public sync with supplied scratch.

`temp/p018-policy-host-pressure` passes 1329 / 1291 ordinary/sanitizer checks:
shared physical budget, four-worker bound, allocator/accounting/thread refusal,
failed setup plus failed free and retry, dirty/fsync failure during off, free
failure during off, thread reuse, age/pressure wake, one blocked device while
another progresses, and an off waiter with both a live ticket and active pass.
The first fixture compile used the wrong host thread callback return type;
corrected to its void callback contract. The initial policy amd64 build passed.

`file_io` now reserves before position/inode leases, prepares an independent VM
writer, proves allocation inside the published content gate and performs delayed
content commit through the existing coherent owner. Optional refusal uses the
through path; eligibility errors abort without mutation. Every begin-error/end
path releases its optional owner after ordinary locks. Append, synchronous flags,
internal drains, claims, stacked visible/content owners and growth are excluded.
Set-id handling remains before delayed commit. Worker scheduling occurs while the
last ticket still protects its budget.

`temp/p018-fileio-delayed.log` passes 414227 / 410052 checks with the actual public
file I/O and VM code. The admission endpoint is controlled by this fixture (the
real policy is tested separately above). Thirty small writes + one fsync produce
one backend write; thirty write/fsync pairs produce thirty. Eligibility refusal,
validation error, append/O_SYNC/O_DSYNC bypass, internal drain, close after dirty
and failed/successful retry are covered. This tests synchronous-flag bypass of
delay, not a new general O_SYNC syscall durability implementation.

Remaining before p018 completion: user-visible control/status, mount/unmount and
shutdown policy transaction integration, full native opt-in scenarios and all
WB/CACHE regression gates. Default policy remains off. The latest file_io amd64
build passed (`temp/p018-fileio-amd64.log`).

Additional fallback case passes: optional page allocation failure under active
admission completes through, with no published VM page, dirty credit or live
ticket left. `temp/p018-fileio-fallback.log` passes 412389 / 411761 checks.
`temp/p018-policy-default-native` passes the existing 512 MiB QEMU cache, mapped
I/O, truncate/copy-up and sysctl tests after the file_io hookup. This is default-off
regression, not proof of native opt-in control/worker operation. `git diff --check`
is clean. No test/build/runtime remains running at this checkpoint.

## Versioned user control and native opt-in checkpoint

Added root-only `vfs.writeback.control=/absolute/mount:on|off`, a versioned
pointer-free UAPI and readable `vfs.writeback.stats` with mount paths, physical
devices, credits, worker progress/errors and charged memory. Syscall new-value
limit is 512 bytes to hold the 264-byte control; the larger report uses the
existing bounded output allocation. Exact sizes, versions, terminated absolute
paths and root credentials are checked before policy mutation. CLI supports
both named controls and status in its ordinary listing.

`temp/p018-control-host` ordinary/sanitizer passes include malformed controls,
privilege checks, short output, unaligned report storage and duplicate enable.
`temp/p018-control-amd64.log` and native fixture build pass.

New reusable `writeback-native.c`, `.mk` and QEMU runner use a disposable 64 MiB
canonical UFS volume. Initial attempt incorrectly used the file-only guest mkfs
on a raw device (EINVAL); runner now creates the image with the canonical host
formatter. The checker validates that image. USB `/dev/sdb` then returned EIO
at mount, before enabling the policy (`temp/p018-control-native-2`). This is an
unresolved gate, not a successful writeback test.

A diagnostic NVMe comparison of the identical geometry passes all commands
(`temp/p018-control-native-nvme/results.json`): 30 small writes and one fsync
produce one UFS data write (8192 bytes), each-write fsync produces 30, mixed
mapped/ordinary writes remain coherent, the age worker drains dirty bytes,
disable releases all policy/credit/memory resources, and remount verifies bytes.
NVMe is evidence for the shared writeback implementation; it does not substitute
for the failed USB gate. Investigating initial USB media-error latching before
lifecycle work. Phase remains in-progress; no physical measurements are claimed.

## USB opt-in native gate and initial readiness fix

A disposable link-only URB observation (`temp/p018-sense-probe.*`, run
`temp/p018-control-usb-sense`) confirmed SCSI 06/29/00 before publishing sdb.
The bounded TEST UNIT READY loop subsequently succeeded, but its first attention
had permanently latched `media_error`; all later BIOs returned EIO without a
wire command. Boot firmware had already consumed the boot disk attention, which
explains the second-disk-only symptom. No UFS geometry workaround was needed.

`storage-reserve-host.c` now injects initial readiness attention through actual
production BOT commands. Before the fix it fails at `s->media_error == 0`
(`temp/p018-usb-initial-ua-before`). The driver now exempts only pre-publication
TEST UNIT READY from the media-error latch; its bounded probe must still succeed.
Any later command/public disk retains media-change failure semantics. The host
case proves successful initial reads and permanent EIO/no further wire commands
after an unexplained post-publication read attention.

`temp/p018-usb-initial-ua-after` passes USB core, xHCI and storage ordinary plus
ASan/UBSan fixtures. Existing no-medium production tests also pass all three
ordinary/sanitized/analyzed runs, 165 checks each (`temp/p018-usb-no-media.log`).
Ordinary amd64/native fixture rebuild passes; no diagnostic wrappers remain in
`build/amd64/vmunix` (`nm` has no `__wrap_` symbols).

`temp/p018-control-usb-fixed/results.json` passes the same full native opt-in
sequence over USB sdb: one batch write versus 30 each-fsync writes, age drain,
mapped/ordinary coherence, successful disable with zero retained resource
counts, and remount verification. NVMe comparison is retained as independent
evidence. These runs disable explicitly before unmount; they do not yet prove
unmount with the policy still enabled or shutdown durability. Those remain the
next implementation gate together with synchronous-open completion semantics.
All runs are stopped at this checkpoint; q109/p018 remain in-progress.

## Reversible unmount ownership

Implemented a caller-owned `writeback_unmount` token. Begin joins the physical
worker and admission tickets, drains with reserved scratch, and retains the
policy mount reference. It releases control serialization before the caller's
filesystem teardown checks; conflicting controls on the same physical worker
return EBUSY. Finish either restores admission without allocating or removes the
policy after successful teardown checks. Shared siblings keep their worker;
a failed clean resource free after commit retains an idle charged owner for later
reuse, with explicit budget reopening on the next enable.

Public and private VFS unmount now include the retained policy reference in both
namespace and filesystem preparation checks. Every early refusal aborts the
token; successful preparation retires it before filesystem destruction. Through
mounts use an empty token. This keeps the original policy on EBUSY and drain
failure instead of disabling it as a side effect of a refused unmount.

`temp/p018-unmount-token-host` passes ordinary/sanitizer policy tests (3839 / 1527
schedule-dependent checks), covering failed begin, abort/re-admission, shared
physical conflicts, other-device admission, sibling commit and failed final free
followed by reuse. `temp/p018-unmount-frontier` retains all 77 ordinary and 77
sanitizer disk/mount synchronization checks. These host policy tests model the
filesystem drain boundary; native evidence below covers the VFS hookup.

The native fixture now attempts unmount while its writable descriptor is open:
EBUSY must preserve LIVE policy and allow another delayed write. Closing it then
unmounts without an explicit off; resource/credit counts must reach zero.
`temp/p018-unmount-native/results.json` passes over QEMU USB, including this new
case and remount byte verification, alongside batch, per-write fsync, age and
mapped-coherence cases. `temp/p018-unmount-build.log` and native fixture build
pass; pcat and pc98 supported builds pass (`temp/p018-unmount-pcat.log`,
`temp/p018-unmount-pc98.log`). amd64 restoration is being completed separately.

Remaining p018 gates include injected filesystem prepare-unmount failure through
the integrated VFS path, shutdown admission/drain/error propagation and checked
O_SYNC/O_DSYNC completion, then full phase regressions. Current init stops services
and calls sync before its system ioctl, but the kernel shutdown boundary does
not check storage durability and init pauses forever after a failed final ioctl;
retry behavior must be addressed with the shutdown change. p018 remains active.

Ordinary amd64 restoration completed successfully (`temp/p018-unmount-amd64-restore.log`).
No runtime or build is left running at this checkpoint.

## Shutdown storage boundary checkpoint

Added `writeback_shutdown_begin/finish`: close optional admission and competing
policy/unmount controls, join every worker/ticket, drain enabled mounts with
reserved scratch, then retain paused ownership across the caller's final mount
barriers. Failure restores admission and keeps dirty owners. Success retires all
policy references and clean worker resources, leaving admission permanently
closed before device shutdown. An outstanding unmount token refuses begin with
EBUSY. Control serialization is not held over the final filesystem drain loop.

`system_shutdown_prepare` now returns an errno. It drains writeback and calls
`mount_sync_all` before network/USB/PCI shutdown; failures reset preparation state
and leave devices available. Concurrent callers join completion or can retry a
failed attempt. Both halt/reboot ioctls return that error instead of proceeding.
Init retains the requested system action and retries a failed final ioctl every
five seconds instead of pausing forever. As before, init stops service producers
before the boundary; optional admission closure is not a general freeze of all
ordinary through I/O from arbitrary kernel producers.

`temp/p018-shutdown-policy-host` passes 1353 / 1391 ordinary/sanitizer checks:
active unmount conflicts, failed dirty drain and admission recovery, paused
controls, explicit abort and successful final retirement with zero owners.
`temp/p018-shutdown-order.log` passes the actual kernel preparation function with
controlled endpoints: begin failure and mount barrier failure call no device
shutdown, failure aborts policy, retry orders storage before network/USB/PCI,
and success is idempotent. This single-thread fixture does not yet test concurrent
preparation callers. `temp/p018-shutdown-amd64.log` build passes.

Still required: native shutdown invocation and persistence proof (the previous
native unmount run predates this change), concurrent preparation/failure retry,
supported remaining architecture gates, integrated filesystem teardown fault,
O_SYNC/O_DSYNC completion and full p018 acceptance/regressions. Do not treat the
host endpoint-order model as a full native shutdown acceptance. q109 remains
active and WS025 is not complete. No test/runtime/build remains live here.

## Native shutdown and synchronous-open completion

A reusable link-only shutdown probe observes the real USB shutdown entry and
platform halt, without changing the kernel's operation. Native init poweroff
starts with an enabled mount and 4096 dirty bytes. The probe confirms no retained
policy mounts, active worker, dirty credits or tickets before USB teardown and
reaches the real halt (`temp/p018-shutdown-native/guest.log`). Init's normal sync
also runs; this is end-to-end shutdown evidence, not an assertion that dirty data
first reaches the kernel shutdown hook. USB reports mounted devices busy and
retains its controller, as noted for the storage lifecycle review below.

The first runner exited nonzero during offline verification: the generated-image
checker requires identical backup superblocks, whereas runtime UFS updates only
primary clean/summary fields. Byte differences were exactly offset 209 and the
counters at 1016/1024. Added an explicit runtime reader mode that permits only
clean (209) and summary (1008..1039) differences; all geometry stays strict and
the default generated-image check is unchanged. The retained shutdown image
passes byte verification (64 bytes 0xcd at offset 3, 0x9a/0x5b at 8193), and a
corrupted backup geometry is still rejected. Results are separately recorded in
`temp/p018-shutdown-native/shutdown-offline-results.json`; the original failed
runner log is retained, not relabeled PASS. No extra boot was needed to inspect
the stopped disk. The updated runner performs this verification on future runs.

Added `file_io_complete` after ordinary content/position lease release. It uses
full fsync for O_SYNC and O_DSYNC, only once per outer file I/O transaction, and
returns durability errors rather than a successful count. A preexisting negative
transfer error remains primary while a flush failure is still recorded. Internal
flags and inherited DRAIN/ORDERED avoid recursion. Scalar/positional/vector
syscalls and public single-transfer helpers use this completion; VM internal
transactions keep their plain end. Open now accepts both synchronous flags.

`temp/p018-sync-completion-host.log` passes 411782 / 409458 ordinary/sanitizer
checks. Expanded `...-host-2.log` passes 412857 / 409635: two chunks receive one
outer barrier, failed sync/retry for both flags, preservation of earlier EFAULT,
and inherited DRAIN avoiding recursive sync. Native initially failed opening
O_SYNC (EINVAL), exposing that flags validation still rejected it despite host
flags injection. After fixing the public open mask, `temp/p018-sync-open-native`
passes write/pwrite/writev with both flags, driver flush observations and zero
dirty credits, plus batch/age/mapped/unmount/remount regression. Ordinary amd64
build and the strict generated UFS image checker pass; shutdown wrappers are
absent from the restored ordinary kernel. Remaining architecture builds are
being recorded separately, and p018 is still in-progress.

Remaining: concurrent shutdown preparation/failure retry, integrated filesystem
prepare-unmount failure, coverage mapping for WB/CACHE/FLUSH and final full
regressions. Native shutdown's retained HCD message does not prove physical USB
controller shutdown; storage bytes were verified, and the busy-device lifecycle
finding is handed to p025 for review rather than hidden by the PASS markers.

Latest pcat/pc98 builds and ordinary amd64 restoration pass (`temp/p018-sync-pcat.log`,
`temp/p018-sync-pc98.log`, `temp/p018-sync-amd64-restore.log`). All runtime/build
processes are stopped at this checkpoint.

## Final p018 acceptance and coverage

Additional gates: `temp/p018-shutdown-concurrent` ordinary/sanitizer tests the
real shutdown state machine with a failed first owner, waiter retry, and a third
caller joining while USB teardown is still paused. Exactly one device shutdown
sequence completes. `temp/p018-unmount-private-fault` runs actual VFS, inode,
namei and namecache with a controlled policy endpoint: both public/private mounts
survive sync and prepare-unmount errors with retained policy references, then
retry successfully (122 checks per mode). Policy resource/worker rollback itself
is separately exercised by the real policy tests, not by this endpoint model.

`temp/p018-policy-interrupted` passes 1553 / 1537 ordinary/sanitizer checks,
including EINTR while off/unmount/shutdown waits on a real outstanding credit
ticket. Admission reopens and the ticket remains intact. `temp/p018-redirty-host.log`
passes 415277 / 410453 checks with a deterministic new race: the old backend
payload is held in flight, an independently opened description obtains a new
admission ticket, the old immutable byte remains unchanged, and the newer dirty
byte survives completion until its own fsync.

Final storage regression `plan/ws018-kernel-architecture/temp/ws025-p018-final-2` passes
FS50/50, no unrun cases, Wi-Fi30 ordinary/sanitizer and native USB. The first final
attempt stopped at the extracted-syscall fixture's obsolete plain-end endpoint;
updated it for checked completion and added scalar/positional/vector completion
error propagation assertions. `temp/p018-syscall-completion.log` passes those
verbatim production syscall paths. Production code did not change after the
latest supported x86 builds/native synchronous gate.

| Contract | Evidence |
| --- | --- |
| WB01–WB03 observers/cursors | p017 completed ledger evidence, retained `test_file_error_observers` in latest file-cache ordinary/sanitizer gate; FS50 regression |
| WB04–WB05 failure retry and close ownership | latest dirty-credit, delayed/file-I/O, orphan-index and failed-fsync tests; disable/shutdown fault fixtures retain owners |
| WB06 batching and explicit durability | native USB/NVMe one content write vs 30 per-write fsync, 64 KiB host batch; native synchronous write/pwrite/writev |
| WB07 newer dirty vs old completion | deterministic `test_writeback_redirty` in latest host gate; existing mapped coherence tests and native mmap/msync |
| WB08 upper drain through loop/FAT | p017 context propagation and p018 native cache/mapped root drain through existing loop/FAT backing; filesystem-only overlay barriers; explicit physical-domain fixture; FS50 native regression |
| WB09 resource/credit/signal bounds | four-worker/fifth-device refusal, prepare/free failure rollback, actual outstanding-ticket EINTR tests, bounded credit tests and optional allocation fallback |
| WB10 off/unmount/shutdown failure | actual policy EIO/EINTR tests, public/private VFS sync/prepare fault, shutdown failure/retry/order/concurrency; native enabled unmount and observed shutdown bytes |
| CACHE06–CACHE09 | p016 completed memory/reclaim gates plus latest cache pressure/dirty retention, two-device blocked-worker progress, failed off and resource accounting |
| FLUSH01–FLUSH06 | latest 77/77 frontier gate; VM-before-backend ordering and backend-only overlay barriers; FS50 native journal/rename/fsync regression; metadata delay remains p021 |
| Build/runtime policy | amd64/pcat/pc98 latest builds pass, ordinary amd64 restored, no diagnostic wrappers; physical acceptance user-approved, no agent physical measurements |

Completed q109/p018. Explicit limitations remain the selected design: only
allocated existing file data delays; growth/new allocation/metadata/internal
ordered transfers remain through; overlay visible/content aliases do not acquire
ordinary delayed admission. Metadata delay belongs to p021. Native shutdown
preserved busy mounted USB controllers; p025/p026 own the recorded lifecycle
review. This does not claim WS025 completion. No process remains running.

## q110 adapter follow-up

p020 identity inspection found that `file_open_resolved` still rejected synchronous
flags forwarded by overlay, although direct public UFS opens passed p018 tests.
q110 explicitly queued and repaired this adapter omission. Native root/overlay
O_SYNC/O_DSYNC write/read/unlink now passes together with direct-mount tests
(`temp/p020-resolved-sync-native`), and the real file/VM ordinary/sanitizer gate
passes (`temp/p020-prefetch-resolved-host.log`). See p020 results for the retained
scope/evidence. The previous native synchronous test covered only the direct mount.
