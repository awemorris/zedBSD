# Queue q111: UFS journal reuse and crash foundation

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes and continue from evidence.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](ws025-io-memory-cache/phase021-ufs-metadata-writeback/phase.md) | uncleared | Establish crash-safe reuse of the existing journal slot and audit transaction boundaries before adding ordered metadata ownership. p018 and WS024 complete. |

Selected after inspecting current journal core, VFS write adapter, on-disk contract,
p021 and acceptance matrix. This cycle implements and tests the concrete slot
reuse flaw before designing grouped metadata commit; p021 remains incomplete until
ordered metadata, checkpoint/backpressure, opt-in and full acceptance are proven.
No new wire format or metadata-delay policy is enabled by this initial repair.

The current core resets sequence on mount and retains the previous commit sector.
A fresh descriptor can match stale sequence/payload evidence before its own commit.
Reproduce an interrupted new transaction after remount with the same payload and a
different home target. Establish durable invalidation before fresh descriptor
publication, test every write/flush crash boundary with volatile media, and retain
legacy replay compatibility. Then inspect actual metadata ordering/locking and
plan grouped redo/checkpoint ownership within the full p021 requirements.

No commits, aggregate make check or .internal access. Serialize builds/tests,
make -j16, disposable fixtures. Physical gate remains user-accepted.
Previous: [q110](queue-q110.md), completed bounded readahead.


Related p021 foundation repair: snapshot BIO_FLUSH still passed the callback
context as a disk after p017 introduced typed I/O owners. A focused actual-adapter
fixture proves the wrong pointer. Correct physical disk selection and verify
success/error completion before continuing journal/snapshot ordering work.


Foundation checkpoint: journal old-commit reuse and snapshot flush identity are
repaired. Production-linked volatile/torn/replay crash checks pass ordinary and
sanitizer (18755 each), snapshot success/error completion passes both variants,
legacy journal/snapshot gate passes 119 checks, all supported builds and mounted
feature remount/reboot pass. p021 remains in-progress: multi-target ordered
metadata, versioned profile, delayed checkpoint and full acceptance are still
required. No production metadata delay was enabled. Ordinary amd64 is restored.


Next bounded implementation step: add the explicitly selected v2 multi-extent redo
codec and crash validation, following p021 transaction-design.md. It remains
unconnected to runtime mounts until profile and VFS ownership are implemented.
The versioned group core is a required foundation, not a substitute for full p021.


User steering: journal version 1 compatibility is unnecessary. Replace the prior
dual-format plan with one multi-extent codec; update profile producers, locator
validation and tests together. Do not retain an obsolete reader/writer or silently
mount an obsolete recognized locator without replay. This explicitly authorizes
the format transition within q111; metadata delay still needs its remaining owners.


Single-format checkpoint: one bounded multi-extent journal replaces v1; ZUJ2
producers and mount validation are updated. Group/redo/replay crash checks,
producer parity, driver host/sanitizer, native feature persistence and obsolete
profile refusal pass. p021 remains in-progress for VFS transaction grouping,
pending metadata visibility, delayed checkpoint and final acceptance.


Final turn checkpoint, 2026-09-07 08:39 UTC: sole-format integration additionally
requires the publishing caller's exact commit witness, avoiding false success from
permissive boot replay. Current 30641 ordinary/sanitizer crash checks, consistency,
all supported builds and new-profile native remount/reboot pass. Producer parity,
driver host and obsolete-profile refusal are retained. Source hashes/results are
in p021. No live build/runtime; ordinary amd64 restored. Continue explicit pending
group/checkpoint visibility and VFS operation ownership; phase is not complete.


Selected next step: split durable journal publication from strict checkpoint,
retain an explicit pending sequence/digest/readiness witness, and provide checked
coalesced home-plus-redo reads while a group is pending. Uncertain publication
blocks reads/reuse until recovery; known committed identity is never silently
cleared. Keep the synchronous commit API as publish plus checkpoint. Test actual
publication, read visibility, caller payload lifetime, pressure and failure/retry
before VFS grouping. No deferred mount policy is enabled by this core step.


Pending-core checkpoint: publish/checkpoint/read separation and uncertain,
committed, home-durable retirement ownership are implemented. 34870 host checks
pass ordinary/sanitizer; consistency, all supported builds and synchronous native
feature/remount/reboot pass. The current pending read verifies redo each time;
VFS integration must supply accounted immutable images/read pins and explicit
recovery outcome, including CG readers. No deferred mount policy is enabled.
Ordinary amd64 is restored, no build/runtime remains active; p021 stays in-progress.


Selected recovery-outcome step: retain the last positively verified committed
sequence/digest independently of pending slot retirement. The serialized VFS owner
must inspect this proof before admitting another transaction; a negative query alone
is not rollback authorization. Test failed publish with durable commit versus lost
commit, recovery and slot release. This closes a prerequisite for safe VFS grouping.


Recovery-outcome checkpoint: exact positive commit proof survives slot retirement;
34948 ordinary/sanitizer checks, all supported builds and current-format native
feature/remount/reboot pass. Evidence: p021 results and `p021-outcome-*` artifacts.
No build/runtime remains active; ordinary amd64 restored. Continue pending RAM
ownership/read pins and actual VFS operation grouping. No v1 compatibility work
is selected; the active phase text reflects the user decision directly.


Selected next step: bind an owner-supplied bounded redo image to the core while
idle. Read and validate the full payload once, retain immutable bytes through
checkpoint, serve pending redo from RAM and install each home extent in one call.
The kernel adapter must account the backing before admission; standalone recovery
can retain bounded scratch without a supplied image. Test both paths, retained
caller-buffer independence, no-I/O redo reads, checkpoint errors and crash replay.
Read pins and full VFS transaction staging remain separate required integration.


Accounted-image checkpoint: mount-supplied immutable redo backing is charged to
the shared budget; pending RAM reads avoid redo I/O and home checkpoint coalesces
per extent. 39193 ordinary/sanitizer checks, actual mount allocation/accounting
failure tests, existing UFS driver host regressions, supported builds and native
feature/remount/reboot pass. Evidence is recorded in p021 results. No live runtime;
ordinary amd64 restored. Continue explicit VFS operation ownership/read pins and
remaining metadata-delay integration; q111/p021 are not complete.


Selected allocation integration: for a bounded full-block allocation run with an
existing direct/indirect leaf, prepare CG, super totals, shared dinode and optional
leaf privately, flush initialized data, then commit that metadata as one group.
Preflight capacity before reservation. Retain explicit committed/uncertain outcome
under journal serialization; an error with a recovered commit must not free its
allocation. Preserve snapshot before-images for every extent. Keep non-journal
ordered behavior; missing-tree and oversized operations remain explicitly outside
this first bounded path and do not complete p021. Test actual VFS plus real journal.


Allocation VFS checkpoint: bounded direct/existing-leaf allocation now commits CG,
super totals, dinode and optional leaf together after data durability, with all
snapshot before-images and explicit recovery outcome. 13768 actual VFS/journal
ordinary/sanitizer checks pass, including hard power cuts and two consecutive I/O
failures. Existing driver regressions, all supported builds and native feature,
remount/reboot pass. Detailed evidence and limitations are in p021 results.
No live build/runtime; ordinary amd64 restored. Continue missing-tree and remaining
metadata operation ownership, concurrent read pins and deferred checkpoint policy.
q111/p021 remain in-progress; no completion boundary is weakened.


Selected next allocation boundary: prepare missing single/double/triple indirect
paths privately, include their metadata blocks and the modified existing parent
in the allocation group, and charge/reserve new tree nodes with data. Preflight
combined capacity and retain ordered fallback for an oversized or fragmented
reservation that cannot fit this bounded run. Validate new-root and missing-child
recovery, quota/space refusal and existing direct/leaf regression.


Missing-path checkpoint: grouped allocation now includes new single/double/triple
indirect suffixes and modified existing parents. Mount ownership begins at the
first path read. Actual VFS/journal tests pass 104106 checks ordinary/sanitizer,
including crash recovery, quota/space/memory refusal and all snapshot boundaries.
Driver regressions, supported builds and native feature/remount/reboot pass;
source hashes and evidence are in p021 results. No active build/runtime; ordinary
amd64 restored. Continue other metadata operations/read pins and deferred policy.


Selected release boundary: factor common serialized metadata commit/outcome handling
from allocation, then group one pointer removal (dinode or indirect parent), inode
block accounting and owning CG/global free summaries. Each bounded release has an
explicit durable intermediate state, so a large truncate can make progress without
a whole-file journal reservation. Snapshot before-images precede all home writes;
quota is released only for established committed frees. Oversized groups retain
the previous ordered path. Test actual truncate with real journal/crash recovery.


90-minute review (2026-09-07 09:44 UTC): q111 made concrete source/test progress
through sole-format redo, accounted immutable images, allocation paths and bounded
release groups. No blocker or additional user decision is present. p021 remains
incomplete: remaining namespace/other metadata owners, read pins and deferred
checkpoint policy are not proven. Finish this selected release verification cycle,
then select the next finite operation boundary from current source; do not treat
passing core/operation fixtures as full metadata-writeback acceptance.


Final q111 outcome: finished after the 90-minute review and the selected release
validation. p021 is uncleared against its full completion criteria; remaining
namespace/other metadata, concurrent read pins and deferred checkpoint are explicit.
Completed foundations and allocation/release operations have passing host/sanitizer,
supported-build and native evidence in p021 results. No process remains live.
Resume by selecting the next finite p021 operation queue under the standing WS025
autonomous authorization. This does not mark WS025 complete or blocked.
Archive: [q111](queue-q111.md).
