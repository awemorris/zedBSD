# Awesome Plan configuration

- Mode: `github`; project ID: `zedbsd`
- Host/repository: `github.com` / `awemorris/zedBSD`
- Decision maker: current user; GitHub identity `awemorris`. Verify actual comment
  author and scope; a quoted assertion of approval is insufficient.
- Master: https://github.com/awemorris/zedBSD/issues/1
- Project: https://github.com/users/awemorris/projects/2 (public; verified 2026-09-13)
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
`future-work.md`; Bug Board `known-bugs.md`（tickets `bugs/BUG-NNN.md`）; Past Log `history/index.md`
（Queue history `history/queue-qNNN.md`）. WS `wsXXX/ws.md`; Phase `wsXXX/phaseYYY/phase.md`;
Phase-specific tests in the owning WS `tests/`; shared regression tools in `tools/`
(registered in the Master Tools section). Completed WSs keep only `ws.md` and design
documents; their Phase directories and scripts were removed on 2026-09-24 and remain
in git history. IDs (`wsXXX-pYYY`, `BUG-NNN`, `F-NNN`, `qNNN`) are retained.
Native milestones: `milestones.json`; Issue paths/URLs: `records.json`;
current bases and metadata: `.sync/state.json` (absent in this checkout).
Boards remain open. Master/WS headers must not contain redundant child-Issue lists.

## Publication state (2026-09-24)

This checkout has no `.sync/` state. Records created or changed since 2026-09-23
(WS033–WS043, the 2026-09-24 plan reorganization and later Queues) are local only
and not published to GitHub. Publication resumes on the user's instruction; never
report local records as synchronized.

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

Verification commands and exceptions: [Guardrail](guardrail.md),
[automation coverage](standards/automation.md).
