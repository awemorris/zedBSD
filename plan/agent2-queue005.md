# Agent 2 Queue 005: semantic paragraphs and explicit returns

Last updated: 2026-08-31

QID: `agent2-q005`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-31 the user explicitly requested that the clarified
paragraph, control-comment, and return rules be written into
`plan/coding-style.md` and applied to all of `userland`.

Timebox: one whole-userland semantic-layout pass, focused manual review of
changed return conventions, deterministic audits and fixtures, and one
configured `make -j16` build.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 Queue requested for `/home/awe/zedBSD-2`.

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p021` | [Phase](ws001-posix/phase021-ansi-c-semantic-layout/phase.md) | uncleared | Mechanical whole-tree application and verification complete; human file-specific prose review remains across all 214 implementations |

## Execution boundary

- Preserve all q001--q004 work and unrelated working-tree changes.
- Preserve observable behavior, evaluation order, diagnostics, and each
  callee's success convention.
- Do not add placeholder or syntax-restating comments merely to silence an
  audit; comments must describe the file-specific operation or decision.
- Do not commit, stage, stash, reset, use aggregate `make check`, or consume
  repository `.internal/` material.

## Verification

Extend the Phase-owned audits and fixtures for the clarified mechanically
decidable rules. Review all transformed direct call returns and representative
semantic batches, run focused tests where available, then run configured
`make -j16` and `git diff --check`.

## Queue result

The clarified layout was applied to all 214 C implementations. The pass moved
374 loop comments above their preparation assignments, added 7,727 decision
comments and 4,558 independent-return comments, inserted the required paragraph
gaps, and expanded 1,047 direct or compound call returns through typed result
variables without changing expression evaluation order. The `at` command was
manually refined to expose list, remove, input, parse, open, submit, failure,
and success paths while preserving `submit_job()`'s zero-success convention.

The transformer is idempotent. The 214-C/44-header body audit, 2,454-function
structural audit, 269-file header audit, all focused fixtures, configured
`make -j16`, and `git diff --check` pass. The first build attempt was
environmentally blocked by a full `/tmp`; the successful build used
`TMPDIR=/home/awe/agent2-q005-tmp` and regenerated the amd64 image.

`ws001-p021` remains honestly `uncleared`: deterministic comments were refined
for common file, parser, process, bounds, result, and input objects, but all
214 implementations still require a human file-specific prose review before
the Phase condition prohibiting generic or syntax-restating prose can be
claimed. The 258-row ledger continues to record that semantic hand-off.
