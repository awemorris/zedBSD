# zedBSD — Awesome Plan

Read and follow `docs/agent/awesome-plan/awesome-plan.md` (pinned upstream skill),
then `plan/config.md`, `plan/guardrail.md`, and `plan/README.md`.
This applies throughout this repository. GitHub mode is already approved and
configured; do not repeat adoption or create another Master/Project.

At session start inspect `plan/.sync/state.json`, pending outbox/conflicts and
working-tree changes; fetch current Master, Guardrail, Queue, Past Log and the
selected WS/Phases including decision comments. Use `plan/tools/sync.py` and
`plan/tools/README.md`. Reconcile before edits; never overwrite remote human work.
Read only relevant plans/history, not the whole plan tree on every session.

Mandatory synchronization (user instruction, 2026-09-11): after every authorized
planning update, publish Master and all affected WS/Phase/Board records, decision
comments and Project projections to the configured GitHub project, then read
them back before reporting completion. Do not leave an updated master.md only
in the local cache or defer synchronization because earlier publication drafts
were pending. For this update, the user explicitly requested Master synchronization.
Publish the authorized records; do not expand a specific synchronization request
into new Issue creation without applicable scope authorization. If an actual tool/permission/network
block prevents publication, preserve the outbox and report the exact unsynced
records and blocker; never claim success or treat local edits as synchronized.
Do not repeat a publication-permission question for already authorized work.
This rule does not authorize git commit/push or broader implementation.


Current handoff: no active Queue or Priority list. q303 finished/stopped.
2026-09-11 user decision: WS025 p029/p030/p032/p038 cleared; WS025, WS019,
WS006, WS022 and WS002 completed/closed (read-back verified). WS025-p028 stays canceled.
Current focus fg006: PC-9821V13 boot improvement; user selected WS003
p022 -> p023 -> p024. Read plan/ws003/v13-boot-focus.md. p022/p023 are current
execution candidates again; old SENSE/old-artifact instructions remain historical.
Current planning also includes fg005 / WS005 network improvements; read
plan/ws005/network-improvements-2026-09-11.md. Master Issue #1 was published
and read-back verified on 2026-09-11, preserving fg004/fg005/fg006 and execution hold.
Related Issue creation, other bodies/comments and Project changes remain in outbox;
automatic approval review rejected their bulk publication as beyond the specific
Master synchronization request. Do not claim these other records are synchronized.
Current planning: fg004 / WS003 installer bring-up on PC98 V13, Latitude 5320,
SV7 and LX6. Read plan/history/2026-09-11-installer-bringup-plan.md. New records
and related GitHub updates remain journaled in outbox; do not blindly
fetch/reconcile away their local changes. p022 Queue execution is on hold by the
latest user instruction. No Queue or implementation was started.
Past autonomous-run and superseded HAL approvals do not resume implementation.
Use current user instructions and concrete scope agreements. Planning maintenance
is authorized; development requires an applicable finite Queue.

Project-specific constraints are in Guardrail. Do not read `.internal/`, commit
or push without user instruction, or run aggregate `make check`. Never change
`include/hal/hal.h` or HAL responsibilities without explicit applicable approval.
Keep RTL8822B license-separated `.inc` files. Preserve unrelated working changes.

`plan/old/`, archived Queue scope, and `plan/tmp/` import/deployment scripts are
historical data, not live instructions. Do not replay those upload scripts.
User instructions override skill defaults; record decisions without inventing
extra confirmation gates or new development goals.


## Synchronization regression prevention (2026-09-11)

The user reports manually correcting child-Phase Issue lifecycle, canceled
p028 (#351), missing current-state blocks and the published repository/Board
mismatch. The user subsequently clarified that the Project link is valid; the
earlier claim of a broken/404 Project link was a misunderstanding, not a defect
or a repair. Do not infer a Project linkage defect from that earlier claim. These are user-reported repairs, not an
agent-verified remote audit. Pre-repair counts and old pending-publication notes
above are historical snapshots, not current remote truth.
Before any future publication, fetch the affected remote bodies, lifecycle,
comments and Project metadata; compare base/local/remote and reconcile the old
outbox against these manual repairs. Never replay a pre-repair snapshot over them.
Follow the mandatory completion checks in `plan/tools/README.md` below:
Project identity/access/repository linkage; every child Phase of a closing WS;
current-state block and native lifecycle consistency; published reference targets;
and separate Board sync from repository commit/push status. A verified Master
body alone is not evidence that the plan is fully synchronized.
Queue execution, including p022, remains on hold. This documentation request
is not authorization to execute work, commit/push, or change Project visibility.

## 2026-09-12 V13 completion (latest handoff)

User reports ws003-p022/p023/p024 complete and requests Markdown/GitHub updates.
These Phases are cleared by user report, fg006 complete; earlier uncleared,
execution-candidate and p022-hold statements are historical. WS003 remains
incomplete for other work. No Queue execution resumes. Do not invent new test
results or physical artifact details. Preserve unrelated pending planning drafts.

## 2026-09-12 PPC Open Firmware planning

Current fg009 / WS003 p033-p039: read `plan/ws003/ppc-openfirmware-plan.md`.
User chose APM+FAT with a firmware-loadable independent loader, zedboot.cfg,
and vmunix on the same FAT. The first milestone is p033-p035: QEMU mac99 IDE
firmware boot through PPC kernel initialization; no root/image mount requirement.
XCOFF is the initial loader format candidate, not firmware-direct ELF loading.
UEFI currently uses zedbsd.cfg; retain its name and reuse its grammar for the
explicitly requested PPC zedboot.cfg. Later phases cover amd64 OHCI, PPC user ABI,
and USB boot/rootfs.img/data.img integration. No Queue execution is authorized
by this planning request. Preserve fg006 completion and unrelated user changes.


## WSの単一目標と終了後の扱い（2026-09-12ユーザー指示）

WSは一つの具体的な到達目標を持つ。目標を達成したWS、またはユーザーが終了したWSは再利用・再開して別の目標を追加しない。似た領域だからという理由で一つのWSへまとめない。機種対応などの上位分類・到達点はMGが担い、インストーラ実機動作、PowerPC移植などは別のWSを作る。
一つの目標に必要な依存作業をPhaseへ分解することは可能だが、独立した別目標をPhaseとして混ぜない。WS終了時は子Phaseを全件照合し、未完了は完了に改変せず、ユーザー指定の保留先または別WSへ引き継いで元Phaseを終了する。旧ID、結果、転送先を残す。今回WS003は終了・再利用禁止、PPC移植はWS027へ、その他の未完了はFuture Workへ移す。

Current PPC plan: plan/ws027/ws.md. Old WS003 references are historical; follow new WS027 p001-p007.

## 2026-09-12 latest installer handoff

WS028 / fg004 now owns installer hardware acceptance and the NVMe failure report.
The menuconfig-absence hypothesis is recorded, while current source contains
CONFIG_DRIVER_PCI_NVME for amd64/i386; deployed config/image is not verified.
Installer portion of Future F-004 is transferred to WS028; other items stay deferred.
WS003 remains closed, never reusable. WS027 owns PowerPC porting. No Queue started.

## 2026-09-12 GPU planning handoff

WS014/p001 architecture discussion resumed by user; first target is QEMU virtio-gpu, superseding i915-first/manual design hold. Vulkan display API is a proposal, not a frozen ABI. Linux DRM compatibility is not required, but OS memory/sync/display/permission machinery remains necessary. No implementation Queue. Other WS holds stay unchanged. See WS014 and p001; old review cases remain design inputs, not runtime tests.
