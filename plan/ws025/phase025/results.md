# p025 execution checkpoint

Status: completed in q121. Historical checkpoints below are superseded by the final acceptance section.

The detailed plan is in recovery-design.md. Initial actual-code audit confirms
that USB storage's media_error is local and common disk/cache admission remains
live. Also, current USB shutdown already calls checked HCD quiesce even if class
detach returns EBUSY, so retained memory and still-active DMA must be distinguished
before changing that path.

Implemented the production SCSI response classifier in
include/drivers/usb-storage-scsi.h and tested 4096 current/deferred/ASCQ combinations
plus the retained response-parser regression under ordinary and ASan/UBSan:
`../temp/p025-sense-1/`. T10 source and classification boundaries are in the design.

Remaining: common irreversible media admission, old synchronous/async completion
and cached-hit publication, partitions/loop/claim/mount ancestry; storage state and
same-medium bounded reconfiguration; independent idle retirement/reprobe;
shutdown boundary verification; full REC01–06/fault/regression/native/build gates.
Only the classifier has been implemented so far; it is not yet called by the
storage command path. Ordinary p024 artifact remains restored on disk but is
not a p025 build. No execution is live at this checkpoint.

## Common admission first slice

Added d_media_revoked and disk_media_revoke/status. Revocation is idempotent,
bumps the existing media/proof epoch once, and does not drain/flush/unlink the
active physical disk. Open (including post-driver recheck), block info, ioctl,
reload, BIO admission/dispatch, range resolution and cache admission reject the
revoked physical ancestry. bio_complete converts a revoked-medium completion to
ESTALE/zero confirmed bytes while releasing normal in-flight ownership.

`../temp/p025-disk-media-1.log` passes 27 checks per ordinary/ASan+UBSan variant:
real disk/partition/cache token, held synchronous BIO, revoke while open/inflight,
idempotence, refused new reads/opens/cache accesses, late success converted to
ESTALE, and release of existing references. `../temp/p025-media-async-1` passes
1196 existing asynchronous ownership checks per variant. No p025 native/build
acceptance has been run yet.

This slice is not fully connected or complete. In particular, loop backing and
claim/cache publication races still need audit. Revoked disk cleanup must not
reuse ordinary buf_sync/flush admission: buf_invalidate_disk(DISCARD) currently
calls disk_resolve_range and now rejects a revoked disk too. Add a deliberate
owner-retirement/discard geometry path before claiming idle media replacement.
Also inspect bot_reset's disk_persistence_invalidate: it changes d_media_epoch,
so successful same-medium reset recovery can still be rejected by the existing
async epoch check. Preserve the old generic stale-event contract while introducing
an explicit narrowly authorized same-medium transport-proof boundary; do not
simply remove generation validation. Storage still uses the old media_error and
has not called the new classifier or revocation API yet.

All focused commands are terminal at this checkpoint. No production edits were
made during live builds/tests. The p024 source snapshot is intentionally superseded
by these p025 header/disk changes; do not compare it as a current final snapshot.

The existing actual file-cache/formatter regressions also pass after the common
admission slice: `../temp/p025-media-file-cache-1.log`, 427028 ordinary / 426857
sanitizer file-cache checks and 626 formatter checks in each variant. These are
regressions, not proof of the still-unimplemented revoked-cache discard/loop and
storage state transitions. All tests are terminal.

## Revoked buffer discard and transport-proof separation

Implemented buf_discard_media in actual buf.c: only a revoked physical object
is eligible, immutable old-object geometry is used, no ordinary I/O resolution
or backend writeback is attempted, and referenced/busy/in-flight buffers remain
owned on EBUSY. The future disk retirement caller must first exclude external
users; this helper alone is not an idle-owner proof. New buffer-media-host.c and
run-buffer-media-host.py exercise the production cache plus the retained full run
regression. `../temp/p025-buffer-media-1` passes ordinary and ASan/UBSan, including
pinned dirty refusal, release then discard, unchanged backend writes, balanced
dirty/data ownership and idempotence.

Added disk_persistence_forget for a serialized locally initiated same-medium
transport reset: only persistence proof/epoch is invalidated; existing generic
disk_persistence_invalidate still invalidates media generation and keeps its
stale-event semantics. The USB driver has not switched to this API yet: connect
it together with actual media revocation/state transitions, not in isolation.
`../temp/p025-transport-proof-1` passes 1204 async checks per variant including a
successful owned request across proof-only invalidation and the existing rejected
late result across generic media epoch invalidation. `p025-disk-media-2.log`
adds direct proof-validity/epoch and unchanged in-flight identity checks (29 per
variant). All these commands are terminal; no p025 supported build/native run yet.

Next: implement the external-owner exclusion and idle retirement caller (including
partitions/claims and cache-owned disk references); connect storage state and
bounded mode refresh; propagate revoked ancestry through loops and cache-publication
boundaries; then native shutdown and full acceptance. The current USB state still
uses media_error, so REC01–06 is not yet complete.

Mode-refresh design concern to resolve: retrying a previously constructed WRITE10
must rebuild its FUA policy after refreshed mode parameters and honor newly enabled
write protection. A newly selected FUA-only policy cannot retroactively prove
persistence of older non-FUA writes; do not report an earlier flush frontier as
successful merely because the replacement policy supports future writes. Refresh
must stay inside the original command deadline without recursively taking the
storage mutex or overwriting its recovery budget.

## USB state/admission connection

The production storage object now has explicit ONLINE/RECONFIGURE/REVALIDATE/
ABSENT/FAILED state and retained last sense instead of the local media_error bit.
Published MEDIA/ABSENT/unknown or unauthorized reset notifications revoke the
common disk object before returning the command error. Initial pre-publication
readiness attention remains bounded and cannot poison a later published disk.
Deferred sense cannot authorize the one self-reset UA retry. bot_reset now uses
proof-only disk_persistence_forget; genuine media uncertainty uses disk_media_revoke.

`../temp/p025-storage-media-2` passes ordinary and ASan/UBSan USB core, xHCI and
actual BOT storage fixtures. Extended wire cases cover 28/00, 2A/09, 02/3A,
unknown 2A/02 and deferred reset sense; each verifies retained decoded sense,
correct terminal/revalidation state, common revoke publication, and refusal of
further driver data commands. The disk revoke in this BOT fixture is a controlled
boundary; actual disk/cache/late-completion behavior has separate production disk
and cache tests above. Do not conflate these with a full integrated native gate.

MODE changes currently select RECONFIGURE and fail admission until the remaining
bounded refresh implementation is connected. This is explicitly unfinished and
not REC02 acceptance. Independent media retirement/reprobe, loop/claim/cache
publication audit, full locally initiated reset wire scenarios, shutdown/native
and supported build gates remain. All focused tests are terminal.

Correction: p025-disk-media-2 reports 31 checks per variant (including additional
backend fixture checks), not the earlier prose's 29.

## Bounded MODE reconfiguration

Implemented storage_reconfigure_locked. It retains the original storage lock and
whole-command deadline, refreshes current mode/cache/write-protection data, and
attempts SYNCHRONIZE CACHE before changing policy. Unsupported flush only permits
a fallback when the old policy already guaranteed durability (FUA or write-through);
an old SYNCHRONIZE CACHE policy cannot become retroactively durable by selecting
FUA. Invalid mode headers or an unproved old frontier fail and revoke the owner.
No existing filesystem/flush sticky error or disk read-only flag is cleared.

The original CDB is copied into a bounded 16-byte local buffer. One MODE attention
may refresh then retry; WRITE10 FUA is rebuilt from the new policy, newly protected
writes return EROFS without retry, and repeated MODE attention fails boundedly.
No recursive storage mutex or reset of the original timeout budget is used.

`../temp/p025-mode-wire-2` passes ordinary and ASan/UBSan core/xHCI/storage fixtures.
Actual BOT wire scenarios cover successful retry, newly enabled write protection,
refusal to replace an unflushed sync-cache frontier with unsupported FUA fallback,
write-through-to-FUA transition with the rebuilt WRITE bit, and repeated MODE UA.
The earlier note that RECONFIGURE is unimplemented is superseded by this slice.

Still incomplete: independent idle media retirement/reprobe and external-owner
exclusion, loop/claim/cache publication races, full self-reset fault scenarios,
shutdown integration, and supported/native/full REC gates. All commands are terminal;
no p025 native image has been built or accepted yet.

## Resident pins and retirement exclusion

Added d_buffer_refs and disk_buffer_acquire/release, atomically paired with actual
registry references. buf.c's allocated resident lines use these pins; a buf_view
continues to hold an ordinary external reference. A candidate whose disk is
revoked during allocation is freed instead of published. This distinguishes
resident pins from external mounts/opens/claims without guessing from cache size.
Controlled buffer fixtures and the maintained FAT-story boundary were updated to
model the new pin pair rather than bypass it.

`p025-owner-disk-media-1` passes 34 ordinary/sanitizer checks, including separate
resident count/reference acquisition, denied acquisition after revoke, and valid
release after revoke. `p025-owner-buffer-media-1` passes full buffer-run regressions
plus failed post-revoke candidate allocation with balanced pages/resident pins.
`p025-owner-objects-1.log` compiles actual amd64 disk/buf/usb-storage objects with
make -j16 and supported config. This is an object gate, not full supported/native
acceptance.

Added backing_mutation_begin_retired_disk: retained revoked physical geometry
feeds the existing mutation/claim exclusion registry without permitting normal
I/O through a revoked object. Ordinary `p025-media-claim-1.log` and
`p025-media-claim-sanitize-1/` pass real backing registry tests: an existing claim
refuses retirement, an admitted retirement guard refuses a newly published claim,
and releases remain balanced. The fixture's raw-range boundary intentionally
allows a prevalidated old range, so the exclusion itself, not an earlier normal
I/O rejection, is exercised.

The final idle-retirement caller still must join these pieces, verify all external
references/partitions/inflight/cache users under the registry lock, retain its own
lifetime pin across buffer discard, and recheck before unpublishing. A separate
storage control context must invoke it after the submitted BIO retires. No media
reprobe worker has been implemented yet. All commands are terminal at this point.

## Idle physical ancestry retirement

Implemented disk_media_retire. It takes the retired-media claim exclusion guard,
checks root/partition external references and open/opening/closing/inflight/cache
users under the registry lock, distinguishes resident buffer pins, and holds an
extra root pin across buf_discard_media. After discard it repeats admission and
requires zero resident pins before atomically removing the root and its idle
ordinary partitions. All fallible checks precede namespace removal. The root
remains GONE with its registry-owning reference for the class owner to destroy;
children have no external references and their slots are released. A concurrent
retirement or new observer causes EBUSY rather than partial namespace removal.
Creating a child beneath a revoked parent now fails admission as well.

`../temp/p025-retire-owner-1` passes 49 ordinary/sanitizer checks on actual disk
code with controlled claim/discard boundaries: busy open/inflight refusal before
discard, claim refusal, busy discard, a new reference acquired during discard,
retry after release, atomic root/partition disappearance and final destroy.
Actual cache and claim implementations have their independent fault tests above;
a combined native/media-control gate remains necessary.
`p025-retire-objects-1.log` compiles actual amd64 disk/backing-claim/buf objects.
No commands remain live at this checkpoint.

Next is the separate storage control owner: it must serialize against class
teardown, wait until its submitted BIO has actually retired (without self-drain),
call the new retirement routine, destroy the old root only on success, and probe
and publish a new object. Additional ordinary references intentionally cause
EBUSY; this API is for the class's registry-owning handle, not a lookup reference.
Audit partition probing and loop/claim lifetime across this boundary, rather than
assuming every raw parent pointer already has an external pin. Full supported
builds/native shutdown/recovery and REC acceptance remain incomplete.

## Reusable medium publication owner

Extracted storage_publish_disk from initial attach. It prepares transfer capacity
and geometry/flush flags, publishes one new disk, and leaves the caller's storage
object and URBs intact on error. Failed disk_create now clears storage->disk before
releasing the failed slot. Initial attach remains responsible for its own cleanup
and interface publication; a future revalidation owner can retry without recreating
or accidentally freeing itself. Duplicate publication returns EBUSY.

`../temp/p025-publish-owner-2` passes ordinary/ASan+UBSan USB core, xHCI and storage
fixtures, including a caller-owned staged storage object, injected disk publication
failure with all three URBs retained, successful retry, duplicate refusal and
balanced final release. The previous attach/reserve/BOT/MODE scenarios also pass.
All commands are terminal. A media-control thread is still not implemented.

Control-owner design findings for next implementation: periodic readiness is
needed even for ONLINE removable media (otherwise cached reads never issue a
command that observes Unit Attention), and ABSENT readers need periodic reprobe.
Use the existing kernel thread lifetime API with a checked stop/join boundary;
never hold the storage command mutex while joining or call scsi_probe recursively
under that mutex. Thread creation failure must be handled before publishing a
disk, or rollback may race an already mounted new disk. Starting only after attach
is ready requires an explicit startup/stop handshake. After revocation, wait via
bounded control retries for disk_media_retire's external-owner check, rather than
waiting for the submitted BIO from inside its own storage_submit. Reprobe must
not publish new geometry until old disk retirement succeeds.

## Bounded control pass (scheduler connection still pending)

Added storage_control_step as the operation executed by a future exclusive control
owner. ONLINE checks TEST UNIT READY; REVALIDATE/ABSENT with an old disk first calls
disk_media_retire and disk_destroy, remembering successful retirement across a
failed destroy. Busy ownership returns before any probe or policy reset. Only when
the old identity is gone does it recreate per-medium URBs/policy, probe and call
storage_publish_disk. Publication failure retains the control object for retry.
Disconnected devices reject the pass before wire traffic; unknown FAILED states
are not blindly retried as a replacement medium.

`p025-control-step-2` passes ordinary and ASan/UBSan core/xHCI/storage fixtures,
including ONLINE readiness, actual BOT medium-change notification, busy retirement
with no probe and preserved old flush error, failed new publication, successful
fresh identity publication, and disconnected refusal. The disk boundary is
controlled here; actual registry retirement has its separate owner tests.
The helper has an explicit temporary unused annotation because no production
scheduler invokes it yet. Do not describe this as automatic media recovery.

Class detach now uses media retirement for revoked disks, ordinary removal for
ONLINE disks, and remembers completed removal before retrying a failed destroy.
It no longer tries to flush a revoked medium through ordinary disk_gone_if_idle.
Thread/class serialization remains to be connected before introducing concurrent
control execution. All tests are terminal.

Next implementation: a control mutex spanning the entire control pass and detach,
distinct from the command mutex (scsi_probe locks command operations internally;
ordinary detach can flush and therefore must not hold the command mutex). Create
and start the control thread before disk publication, with an explicit ready/stop
startup handshake so no probe races initial attach and allocation failure precedes
publication. Join without holding the control or command mutex, retain the class
owner on a failed stop, and arrange periodic readiness/reprobe rather than waiting
forever for an I/O-triggered notification. Full native/runtime and shutdown gates
remain pending.

## Control worker and media dependencies (2026-09-08)

Connected the bounded control pass to a kernel thread. Creation precedes disk
publication; ready/stopping atomics provide a startup handshake. A separate
device-rank control mutex serializes the whole pass against detach, without
holding the command mutex during joins or ordinary disk removal. Failed stops
and joins retain the class object and URBs for retry. `p025-worker-lifecycle-1`
passes ordinary and ASan/UBSan USB core/xHCI/storage tests, including creation
failure, bounded stop failure, failed join, failed attach with retained ownership,
and the actual worker loop with a controlled scheduler. This host scheduler test
is not a concurrent kernel execution claim.

`p025-worker-build-amd64-1.log` completed successfully. The real 8 GiB / four-CPU
USB native run `p025-worker-native-1` passes combined writeback, ZUJ2 journal and
readahead with the worker running. This supersedes the earlier notes saying the
scheduler connection is pending.

Added immutable `d_media_backing` for loop disks. It holds a reference from
successful publication to destruction, follows media admission through the
backing filesystem, and does not alter block-address translation. Registry-locked
admission can inspect it without recursively acquiring the loop or disk locks.
Failed publication does not release an unacquired dependency. The same audit
corrected unpublished-child destruction to release parent references only after
successful publication. `p025-media-dependency-1` passes 69 checks in both normal
and sanitizer execution, including loop cache/open/range rejection after physical
revocation, retained backing lifetime, publication failure and balanced retirement.

Partition records must retire with their disk slots. Added
`partition_retire_media` and serialized individual partition publication with the
existing pool replacement reservation. Failed retirement preserves the pool;
success clears the old records and count before releasing the reservation. The
USB class uses this partition-aware entry point. `p025-partition-media-1` passes
75 checks per variant using actual disk and partition code, including busy pool,
busy discard, preserved records and successful complete retirement.

Replacement publication now schedules `partition_reload` outside the command
mutex. Temporary failures retain a pending flag and retry on a later ONLINE pass;
unsupported/non-partitioned media retain their whole-disk interface.
`p025-partition-control-1` passes ordinary/ASan+UBSan core/xHCI/storage tests,
including a busy partition reload followed by successful retry and an assertion
that the command mutex is not held. `p025-partition-build-amd64-1.log` passes the
amd64 disk-image and native fixture build. A real removable-media native run is
being executed separately; its outcome is not yet counted as acceptance.

Still required: native media exchange outcome, self-reset fault coverage,
shutdown/quiesce confirmation, remaining cached-read/publication race review,
supported legacy builds and final REC evidence. p025 and q121 remain in-progress.

## Real media exchange, finite reset and shutdown

The first removable native run (`p025-media-native-1`) timed out waiting for new
publication. Its expected publication text also contained an extra colon; fixing
that alone was insufficient. Link-only `media-control-probe` first demonstrated
periodic TEST UNIT READY, but its verbose output interfered with login
(`p025-media-probe-native-1`). Narrowing the probe to sense/retirement showed
`06/3A/00` in `p025-media-probe-native-2`; the classifier previously treated this
as unknown FAILED. The diagnosed QEMU was explicitly stopped and its runner
joined before source edits. No failed run is counted as acceptance.

Added exact current `06/3A/00` classification as ABSENT (primary implementation
reference in recovery-design.md). Old admission still closes irreversibly; this
does not grant same-medium reset retry. `p025-sense-2` passes 5120 classifier
combinations plus retained parser tests in ordinary/ASan+UBSan.
`p025-eject-ua-host-1` passes actual BOT state/ownership tests in both modes.

Forced ordinary relink with `-W src/hal/amd64/space.c` and verified absence of all
media-probe wrapper symbols. `p025-media-native-2` then passes the combined
8 GiB/four-CPU USB writeback/journal/readahead suite and two real QEMU removable
device scenarios: idle equal-capacity replacement publishes a new disk and reads
the replacement marker; changing it again while mounted rejects a previously
cached marker with ENXIO and does not rebind the old mount. Original build image
hash is unchanged; only disposable media were exchanged.

`p025-self-reset-host-1` passes ordinary/ASan+UBSan actual BOT transport failure,
one self-issued reset plus one current reset attention and successful read;
repeated attention terminates with FAILED/revocation; repeated transport failure
terminates after exactly one reset. Worker and partition retry tests remain green.

Corrected shutdown diagnostics to distinguish retained class memory from checked
HCD quiesce. `p025-shutdown-native-1` passes combined native workloads, clean
writeback at USB shutdown, successful xHCI quiesce with mounted ownership retained,
HAL halt and offline image-content verification. The successful HCD callback is
the production xHCI path that requires submission closure, HCHalted, bus-master
disable, IRQ retirement and outstanding request drain; a quiesce failure would not
print the new success diagnostic. Shutdown link wrappers must still be removed
by the final ordinary relink.

Final relevant regressions so far: `p025-final-async-1` passes 1204 checks per
mode; `p025-final-file-cache-1` passes 424735 ordinary / 422644 sanitizer lifetime
checks and 626 formatter checks per mode. These variable concurrent check counts
are recorded as observed, not a fixed scenario count. Legacy builds, final
ordinary restoration and the final acceptance mapping are still pending.

## Final acceptance and restoration

p025 completed. `p025-final-build-{pcat,pc98,amd64}-1.log` all pass the
supported `make -j16` builds. Final amd64 explicitly relinked without shutdown
wrappers; `p025-final-evidence-1` records source/config and artifact hashes and
confirms no `__wrap_` symbols. `p025-final-native-normal-1` repeats the ordinary
8 GiB/four-CPU USB writeback, journal, readahead and actual media-exchange tests
with success. All commands are terminal; no probe remains in the normal kernel.

| Requirement | Final evidence |
| --- | --- |
| REC01 | p025-self-reset-host-1: one owned reset/current-UA retry, repeated UA and repeated transport failures terminate |
| REC02 | p025-partition-control-1 / p025-self-reset-host-1: actual BOT MODE/cache/protection refresh versus capacity revocation |
| REC03 | p025-sense-2, p025-eject-ua-host-1, p025-final-native-normal-1: current/deferred classification, absent and equal-capacity exchange |
| REC04 | p025-worker-lifecycle-1, p025-self-reset-host-1, p025-final-async-1: separate control lifetime, finite submitted-command recovery, queued/inflight ownership |
| REC05 | p025-partition-media-1, p025-media-claim-1 / sanitizer, native exchange: busy mount/claim/ref refusal and idle replacement |
| REC06 | p025-partition-media-1, p025-buffer-media-1, p025-final-native-normal-1: loop/partition admission, stale BIO, pinned discard, real cached-read rejection |
| FLUSH04–06 | proof-only reset, MODE policy tests, p025-final-async-1, native journal/writeback and shutdown; completed p014/p021 ordering evidence retained |
| ASYNC04–08 | final async and USB ownership/sanitizer gates plus earlier p019/p024 delayed DMA/cancel and mixed-device fixtures; storage worker never self-drains |

Lifetime audit: boot partition scans retain physical references while using raw
scan entries; replacement uses the partition pool reservation and physical reload
reservation. Loop publication pins its immutable media dependency independently
of address translation. Existing file/VM cache reads acquire common disk admission,
including loop backing, before serving hits. BIO dispatch/completion also checks
revocation; old objects cannot retire while external cache/request/claim owners
remain. New geometry and writable policy belong only to a newly published object.
Physical runtime is user-accepted, not agent-measured. The full cross-feature
acceptance/defaults and conditional adoption decisions remain p026 work.
