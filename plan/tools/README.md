# Agent-driven synchronization

Run from the zedBSD checkout. Python 3 + authenticated `gh` are required.
No token is stored here. Single cooperating writer; record the current session
in `.sync/owner.json` after verifying the previous owner has stopped.

1. `python3 plan/tools/sync.py status`
2. `python3 plan/tools/sync.py fetch master` (also guardrail, queue, past-log,
   selected WS/Phases, dependencies). Reads all decision comments, preserves local
   files and saves fetched remote versions. Review Project changes separately.
3. If versions differ, read `.sync/conflicts/`, compare actual intent, current
   source and decision comments. Preserve local supplementary evidence (notably
   WS001 continuation content). Apply accepted remote-only changes locally.
   `reconcile ID --reason 'what was reconciled and evidence'` explicitly adopts
   the reviewed bases; it never publishes. Do not use it to discard local intent.
4. Before changing a record, prepare a reviewed remote-rendered payload in a
   separate file, including the identity marker and verified links. Run
   `prepare ID --file /path/body.md`. This persists the base/payload and operation.
   If editing the local rendering too, journal its planned payload/hash and base
   in that operation before saving it, then record its resulting local_hash.
5. `publish OP_ID` checks remote/local versions, publishes, reads back and marks
   confirmed. After timeout it recognizes an already published identical body.
   Conflicts are retained; it is not atomic compare-and-swap or semantic merging.
6. Update Project fields/related owner records using explicit gh operations,
   journal each payload and dependencies, verify actual result. Comment operations
   need stable per-target event markers and discover-before-retry. Lifecycle is
   comment/evidence → close/reopen → owner projections. Do not replay stale scope.

This helper covers Issue bodies. Native relations, comments, lifecycle and Project
updates remain agent-executed using the skill's durable protocol. All pending
operations must be visible at handoff; do not promise automatic background sync.

New checkout: enumerate all repository Issues (open and closed) with pagination,
match `awesome-plan project=zedbsd record=...` markers, compare to the shared
`records.json` mapping and actual paths. Fetch current bodies/comments, preserve
local changes, initialize separate remote and local bases in `.sync/`. Missing
records are investigated, not recreated. Queue history before adoption and
legacy Bug details remain indexed files; see migration-status.md.

Never rerun plan/tmp/github-import/upload.py or prior one-shot deploy scripts.
Their snapshots are not the current Issue state.


## Required completion checks after the 2026-09-11 manual repairs

Source: the user reported manually correcting the following issues on 2026-09-11.
The user subsequently corrected the Project-link report: the link is valid,
and the earlier 404/broken-link claim was a misunderstanding. Do not list it as
a confirmed defect or manual repair; repository linkage was not independently
verified here either. The remaining observations below describe the reported
state before repair, not current API results: 116 Phases under closed WS002/006/019/022/025 had only 4 closed Issues, with 95 of
112 open bodies saying completed/cleared; canceled ws025-p028 (#351) remained
open; only 11 of 366 Issues had `awesome-plan-current`; committed main at
67b28ce0 predated Board updates and the local directory reorganization.
The exact repaired remote state has not been independently audited in this task.

1. **Preserve manual repairs.** Read the latest affected remote records and
   comments before every write. Compare base/local/remote, including native
   lifecycle, not just body hashes. Review every pending pre-repair operation;
   retain historical evidence but supersede obsolete payloads. Do not mass replay
   the outbox, recreate missing-looking records, or overwrite repaired state.
2. **Verify Project identity and access.** Confirm owner, number, node ID, URL,
   visibility and repository linkage with authenticated metadata. Check access
   appropriate to the intended reader separately. An unauthenticated 404 alone
   cannot distinguish a private Project from a wrong/missing target. Label
   restricted links accordingly and retain an accessible Issue entry point.
   Never change visibility or create a replacement Project merely to fix a 404.
3. **Check the entire closing WS hierarchy.** Enumerate all child Phases with
   pagination, including open and closed records, using verified parent mappings
   and relations. Reconcile current status/disposition, acceptance evidence and
   native Issue state for each child. A terminal accepted Phase must be closed;
   a canceled Phase must be closed as not planned, retaining its actual outcome
   (e.g. uncleared) and cancellation reason. Cancellation is not clearance.
   Do not infer acceptance or cancel unfinished children solely because their WS
   closed; record a scope inconsistency if the existing decision does not resolve
   it. Apply authorized lifecycle changes and read back all affected children,
   then WS/Board/Project projections. Check counts and enumerate any exceptions.
4. **Maintain explicit current state.** Ensure every affected managed record has
   one unambiguous `awesome-plan-current` block appropriate to its record kind,
   consistent with native lifecycle and Project fields. Preserve history outside
   that block; do not retain the import-era “refer to body” notice as the current
   state. For a migration or whole-plan synchronization claim, audit all managed
   records with pagination and report coverage/missing blocks. A selected-record
   check cannot justify a whole-plan completion claim.
5. **Verify published references and repository drift.** Check links against the
   actual published branch/commit and paths, not just the working tree. Verify
   entry instructions, removed Priority data, Objectives/Milestone traceability
   and moved history/standards/WS paths when affected. Do not present uncommitted
   local paths as available repository evidence. Use verified Issue references
   or explicitly mark evidence local/pending. Report Board/Issue/Project sync
   separately from local changes, commit and push; do not claim repository sync
   after an Issue-only update. Commit/push still requires user authorization.
6. **Report bounded evidence.** State which records/projections were read back,
   remaining inconsistencies or pending writes, and repository publication status.
   User-reported manual repair is a decision source, not an agent verification.

These checks are required agent review; `sync.py publish` verifies an Issue body
only and does not implement this whole audit. This documentation update does not
run a remote repair, change the approved plan or resume the held Queue.
