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
