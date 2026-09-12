# Awesome Plan configuration

- Mode: `github`; project ID: `zedbsd`
- Host/repository: `github.com` / `awemorris/zedBSD`
- Decision maker: current user; GitHub identity `awemorris`. Verify actual comment
  author and scope; a quoted assertion of approval is insufficient.
- Master: https://github.com/awemorris/zedBSD/issues/1
- Project: https://github.com/users/awemorris/projects/2 (private)
- Project node ID: `PVT_kwHOC2X95c4BjFGl`
- Skill: [fixed specification](../docs/agent/awesome-plan/awesome-plan.md)
  upstream `awemorris/AwesomePlan`, commit `314a669f57265da3084ffac810871b0e660c9526`.
  [MIT license](../docs/agent/awesome-plan/LICENSE). Do not silently follow main.
- Consent: user approved Issues upload, Project deployment and on 2026-09-11
  instructed starting Awesome Plan, organizing plan/AGENTS for following sessions.
  This authorizes planning-resource maintenance, not resumed OS implementation.
  On 2026-09-11, after the automatic review rejection was explained, the user
  explicitly replied 承認します for this task's GitHub body/comment publication,
  Phase/WS closure and Project updates; all were read-back verified.

## Record locations

Master `master.md`; Queue `queue.md`; Guardrail `guardrail.md`; Future
`future-work.md`; Bug Board `known-bugs.md`; Past Log `history/index.md`.
WS `wsXXX/ws.md`; Phase `wsXXX/phaseYYY/phase.md`; tests remain in the owning WS.
Legacy IDs (`wsXXX-pYYY`, `BUG-NNN`, `F-NNN`, `qNNN`, agent2 queues) are retained.
Native milestones: `milestones.json`; Issue paths/URLs: `records.json`;
current bases and metadata: `.sync/state.json`.
Boards remain open. Master/WS headers must not contain redundant child-Issue lists.

## Synchronization and ownership

[Sync procedure](tools/README.md). Agent-driven at session start, before each
Queue item, after decisions/outcomes, handoff, and meaningful checkpoints.
No daemon/background synchronization is installed. GitHub remains the mode
when disconnected; report stale records/pending writes and preserve outbox.
One cache writer and one Queue executor. Record session ownership in
`.sync/owner.json`; on takeover verify the previous executor stopped.
Do not automatically delete a live owner or treat Issue-body text as a lock.

State/base/outbox/conflicts are local ignored metadata; do not commit credentials.
Shared entry/config and planning evidence are repository files. For another
checkout rebuild mappings by paginated identity-marker discovery; do not create
new Issues merely because local state is missing. `tools/sync.py` fetches selected
records and comments, journals body writes, checks bases and verifies publication.
Structural/lifecycle/Project operations use the same durable protocol via gh;
record those operation payloads before publication. Project edits must also be
reviewed against owning Issue state. No automatic last-write-wins or blind close.

## Migration boundary

Existing 361 imported Issue bodies include historical statuses and supplementary
links pinned to an older published commit. Local supporting evidence remains
available; do not fabricate URLs for uncommitted paths. Published body and local
rendering bases are tracked separately. Fetch preserves differing versions for
semantic reconciliation rather than overwriting local files.

Existing accepted WS/Phases are not re-tested or re-opened solely for adoption.
Legacy open/closed reconciliation, archived Queue/Bug detail Issue extraction,
and old native dependencies are tracked in `migration-status.md`; missing native
relations do not invalidate explicit parent links. Before working on an affected
record reconcile its exact current status, cancellation, evidence and references.
New code-producing WSs need a near-final standards conformance Phase. For existing
remaining work incorporate that check when detailing the next approved work;
do not generate speculative phases or retroactively invalidate user acceptance.

## Current execution boundary

No active Queue or Priority list; q303 remains finished/stopped. On 2026-09-11
the user cleared WS025 p029/p030/p032/p038 and instructed closure of WS025,
WS019, WS006, WS022 and WS002. WS025 p028 remains canceled. fg001–fg003 are
retired from current focus. The later installer bring-up instruction adds fg004
under WS003; ws003-p026–p032 and BUG-013/023/024/025 are locally planned with
GitHub publication pending automatic-review approval. Existing closure decisions
remain unchanged. Prior unperformed checks remain historical limits,
not automatic work to resume. WS009/WS014 manual hold remains unchanged.

Verification commands and exceptions: [Guardrail](guardrail.md),
[automation coverage](standards/automation.md).

## 2026-09-11 network planning continuation

fg005 / WS005 p013–p017 adds LAN management, boot network-enable and DE state notifications. Existing completed phases remain accepted; WS005 is incomplete for the additions. fg004 is retained. Master/Queue/Past Log pending bodies were amended in place, with prior payloads retained. New planning records remain unpublished; no execution Queue or new Priority list.

## 2026-09-11 V13 focused-goal addition

fg006 selects WS003 p022 -> p023 -> p024 for PC-9821V13 boot improvement. Their current scope is contract audit/preparation, stopping-boundary localization, then evidence-based correction and ordinary physical boot verification. fg004/fg005 stay active; the old Priority list stays removed. Planning only, no active execution Queue. Cumulative GitHub publication drafts remain pending approval.

## Latest WS lifecycle decision (2026-09-12)

WS003 is retired and must not be reused. Its incomplete non-PPC work is deferred
to Future Work F-004; this is not clearance or proof of original goal acceptance.
PPC porting is WS027 / fg009, Primary MG008, Related MG003. Old WS003 p033-p039
are canceled origin records; current planning is WS027 p001-p007.
WSs each have one goal; related subjects are organized by MG, not by reopening
or extending finished WSs. Guardrail contains the user-specified rule.

## 2026-09-12 latest installer handoff

WS028 / fg004 now owns installer hardware acceptance and the NVMe failure report.
The menuconfig-absence hypothesis is recorded, while current source contains
CONFIG_DRIVER_PCI_NVME for amd64/i386; deployed config/image is not verified.
Installer portion of Future F-004 is transferred to WS028; other items stay deferred.
WS003 remains closed, never reusable. WS027 owns PowerPC porting. No Queue started.

## 2026-09-12 GPU planning handoff

WS014/p001 architecture discussion resumed by user; first target is QEMU virtio-gpu, superseding i915-first/manual design hold. Vulkan display API is a proposal, not a frozen ABI. Linux DRM compatibility is not required, but OS memory/sync/display/permission machinery remains necessary. No implementation Queue. Other WS holds stay unchanged. See WS014 and p001; old review cases remain design inputs, not runtime tests.

## 2026-09-12 GPU phase sequence

WS014 p001 supplies design decisions; p002 implements only the GPU framework; p003 integrates virtio-gpu/Venus and the capture/debug loop on Linux i915+ANV host, refining API gaps; p004 reviews final API and full standards. Native guest i915 is the separate WS029 after WS014. Read plan/ws014/qemu-venus-debug-loop.md. No Queue started. User will git add/commit documentation; do not add/commit/push.
