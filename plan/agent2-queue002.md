# Agent 2 Queue 002: canonical userland source headers

Last updated: 2026-08-31

QID: `agent2-q002`

Queue status: completed

Queue finished: **Yes**

Authorization: on 2026-08-31 the user explicitly approved execution after
reviewing this Queue's 269-file scope.

Timebox: one finite Phase and one configured `make -j16` build. This Queue does
not perform general style normalization or semantic userland changes.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 Queue requested for `/home/awe/zedBSD-2`.

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p019` | [Phase](ws001-posix/phase019-userland-file-headers/phase.md) | completed | All 269 `.c`, `.h`, and `.S` files below `userland` start with the canonical section 13 block and a separate useful file explanation, with implementation bodies preserved |

## Scope rationale

- `userland` currently contains 214 C implementations, 44 headers, and 11
  preprocessed assembly sources. The wording “source code files” applies
  consistently to all three C-family forms.
- The change is broad but mechanically bounded to leading comment regions.
  It is kept separate from functional command work and does not claim that the
  remainder of every file complies with the complete C style.
- Existing descriptive leading prose is valuable source documentation and is
  retained as the required explanation. Missing explanations are written per
  file or cohesive component, not synthesized as one meaningless generic line.
- Exact audit fixtures and body-preservation checks prevent a bulk rewrite from
  concealing semantic or attribution changes.

## Dependencies and safeguards

- `plan/coding-style.md` section 13 is the source of truth. The sample typo in
  “progmra” is prose, not required literal content; explanations describe the
  actual file.
- The committed `agent2-q001` implementation and updated coding-style document
  are the fixed baseline. This Queue may alter only source leading comments
  and must not undo their implementation or planning results; the untracked
  Agent 2 Queue books are preserved.
- No commit, staging operation, stash, reset, or cleanup is authorized.
- No `.internal/` material or external source is consulted or copied.
- An ownership/license conflict stops that path and is recorded; the canonical
  Awe Morris/Zlib block is never imposed over contrary attribution.

## Verification rules

- Use the Phase-owned exact header audit, negative fixtures, deterministic
  inventory, and body-preservation comparison.
- Build with `make -j16`; do not use aggregate `make check`.
- A QEMU cell is not required because the accepted diff is comment-only and
  the complete configured compile/link/image gate is stronger evidence for
  token preservation.
- Run `git diff --check` and report the final exact path counts.

## Completion definition

This Queue is finished after `ws001-p019` is `completed` or `uncleared`, its
actual evidence and residuals are synchronized to WS001, and no full-tree
non-header style compliance is implied.

## Execution result

Completed on 2026-08-31. The deterministic inventory contains 214 `.c`, 44
`.h`, and 11 `.S` files. Seventeen useful existing explanations were retained
and 252 file-specific explanations were added. The one pre-header UTF-8 BOM
was removed; no ownership or license conflict was found.

The exact whole-userland audit and its negative fixtures pass for all 269
files, and the recorded SHA-256 values prove every implementation body is
unchanged. All 11 assembler sources preprocess successfully. The existing
base-style audit was adjusted to find its modeline after the two required
header comments and still reports its independent 7-compliant/234-historical
full-style inventory. `make -j16` and `git diff --check` pass. Aggregate
`make check` and QEMU were not run, as fixed by this Queue's verification
rules. No commit or staging operation was performed.
