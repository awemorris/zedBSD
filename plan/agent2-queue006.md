# Agent 2 Queue 006: symmetric control blocks

Last updated: 2026-08-31

QID: `agent2-q006`

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-31 the user explicitly requested that block-entry
indentation, symmetric `if`/`else` braces, braced multi-line loop bodies, braced
`for`-containing-`if` bodies, and empty block-entry lines be fixed throughout
`userland` and added to `plan/coding-style.md`.

Timebox: one deterministic whole-userland control-block pass, focused fixtures,
whole-tree audits, and one configured `make -j16` build.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 Queue requested for `/home/awe/zedBSD-2`.

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p021` | [Phase](ws001-posix/phase021-ansi-c-semantic-layout/phase.md) | completed | Enforce the clarified control-block symmetry, loop bracing, entry indentation, and entry-spacing rules across all 214 implementations |

## Execution boundary

- Preserve all q001--q005 work and unrelated working-tree changes.
- Preserve `else` association, loop extent, `break`/`continue` targets,
  evaluation order, diagnostics, and observable behavior.
- Use statement-aware lexical parsing; do not brace control flow through blind
  text substitution.
- Do not commit, stage, stash, reset, use aggregate `make check`, or consume
  repository `.internal/` material.

## Verification

Add positive and negative fixtures for all clarified rules, require the
transformer to be idempotent, run the 214-C/44-header body audit, the
2,454-function structural audit, the 269-file header audit, configured
`make -j16`, and `git diff --check`.

## Execution result

The deterministic statement-aware pass changed 159 implementations while
adding 296 required `if`-body braces, 240 loop-body braces, removing 602 empty
block-entry lines, and correcting 334 overindented decision statements. A
second generalized indentation pass corrected another 211 immediate
statements in `if`, `else`, `for`, and `while` blocks. Both passes preserve
statement extent and are now idempotent.

The ANSI compiler cross-check exposed 31 declaration-after-statement cases
that the older lexical audit had missed. Those declarations now reside in
their function-leading groups, with runtime initialization retained at the
original execution points. A strict rebuild of every configured userland
object passes with `-Wdeclaration-after-statement`; the ordinary configured
`make -j16` also passes.

The positive and negative body fixtures cover asymmetric branches, a `for`
whose body is an `if`, a physically multi-line loop body, excess entry
indentation, and an empty block entry. The 214-C/44-header body audit reports
zero residuals, the structural audit reports 2,454 functions and zero
diagnostics, and the canonical header audit passes all 269 files. On 2026-08-31
the user completed the remaining manual inspection and explicitly accepted the
result. Phase 21 and this Queue are complete.
