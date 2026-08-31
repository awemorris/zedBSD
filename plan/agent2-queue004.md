# Agent 2 Queue 004: ANSI C and semantic userland layout

Last updated: 2026-08-31

QID: `agent2-q004`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-31 the user explicitly requested merging the
clarified rules into `plan/coding-style.md` and applying them again.

Timebox: one deterministic whole-userland pass for the newly clarified rules,
their audits and fixtures, and one configured `make -j16` build.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 Queue requested for `/home/awe/zedBSD-2`.

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p021` | [Phase](ws001-posix/phase021-ansi-c-semantic-layout/phase.md) | uncleared | Mechanical ANSI C/layout rules pass; semantic paragraph and decision-comment review remains across all 214 implementations |

## Execution boundary

- Preserve the q001--q003 work and all unrelated working-tree changes.
- Change organization and presentation only; no POSIX feature implementation,
  ABI change, diagnostic change, or generated artifact is in scope.
- Permit configured reserved-name compiler extensions, including `__inline`,
  without treating ordinary C99 declaration syntax as permitted.
- Do not commit, stage, stash, reset, use aggregate `make check`, or consume
  repository `.internal/` material.

## Verification

The Phase-owned audits and fixtures must cover all 214 C and 44 header files;
the exact header audit covers all 269 C-family files. Refactoring tools must be
idempotent. The final gates are the focused audits, configured `make -j16`, and
`git diff --check`.

## Queue result

The deterministic whole-userland pass removed every declaration-form `for`
initializer, every declaration below the function-leading declaration group,
and every actual standalone scope-only compound statement. It normalized 455
declaration/statement gaps and installed the modeline-first header in all 269
C-family files. The transformer is idempotent, both mechanical audits and
their fixtures pass, and the configured build passes.

`ws001-p021` is honestly `uncleared` because semantic prose is not mechanically
complete. A strict adjacency inventory still finds 7,779 `if` decision sites
without an immediately preceding purpose comment, and assignment/call
paragraphs require the same human semantic review. Generic comment insertion
would not satisfy the rule that prose state what the paragraph does.
