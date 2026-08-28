# Queue proposal: canonical Noct maintainer-review corrections

Last updated: 2026-08-28

QID: `q022`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-28, including
replacement of the gitlink checkout and automatic commit/push to both
`awemorris/NoctLang` and zedBSD

Timebox: no fixed limit; continue until the Queue completes or reaches a
recorded human-decision boundary

Parent: [master plan](master.md)

Previous Queue: [q021](queue-q021.md)

## Purpose

Apply the maintainer's review to the canonical NoctLang zedBSD target, publish
that reviewed upstream commit, and replace zedBSD's Noct submodule/gitlink with
one tracked Makefile that reproducibly clones and builds the accepted official
revision.

## Proposed execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws008-p004` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase004-upstream-review/phase.md), [tests](ws008-noct/tests/README.md) | complete | Canonical BeUI source/registration, zedBSD macro/CMake, Boolean JIT boundary, and Makefile-only source delivery pass the reviewed contract and prior QEMU regressions |

## Entry evidence

- `awemorris/NoctLang` main and the clean local integration checkout are at
  `3af6d723ac4cfe5ee80dcc5a5a405209c6fc4597`, titled `Add zedBSD support`.
- The current public JIT boundary is already Boolean; p004 preserves and tests
  it rather than performing another incompatible signature change.
- q019/q020 completed the target build, public evdev repair, canonical BeUI
  behavior, and amd64 RW-to-RX JIT evidence. This Queue is a bounded maintainer
  review correction, not a redesign of those facilities.
- The previously proposed shell Phase `ws001-p014` was never approved or
  started. It returns to the dependency-ready planning pool.

## Proposed execution

1. Clone a clean upstream worktree at the recorded main revision and apply the
   p004 canonical NoctLang corrections.
2. Run upstream static, zedBSD, BeUI, JIT, sanitizer, and source-layout gates.
3. Commit and push the canonical NoctLang correction; record the published
   commit ID.
4. Replace zedBSD's `userland/noct` gitlink with one tracked Makefile that
   clones the published revision into `userland/noct/NoctLang`, update package,
   toolchain, tests, and ignore rules, and prove clean/idempotent acquisition.
5. Re-run affected p001--p003 host/QEMU acceptance, `make -j16`, and
   `git diff --check`; do not run `make check` or use `.internal/`.
6. Synchronize P/W/M/Q evidence, commit `WIP`, and push zedBSD. If a push is
   rejected, retain the local commit and report the repository and rejection.

## Stop and defer rules

- Do not delete the current gitlink checkout until a clean clone of the same
  published source has been verified and the replacement Makefile is ready.
- Do not publish an upstream commit until its focused static/zedBSD/BeUI/JIT
  gates pass.
- Do not pin zedBSD to an unpublished commit or carry canonical Noct source as
  ordinary files in zedBSD.
- If preserving a public custom-HAL registration entry requires an API choice,
  or CMake cannot retain zedBSD identity without a Platform module, mark p004
  `uncleared` and request human review.

## Approval boundary

Approval must explicitly cover `ws008-p004`, its bounded corrections, deletion
of the existing `userland/noct` gitlink checkout after verification, and
commit/push to both `awemorris/NoctLang` and zedBSD. It does not authorize the
deferred shell Phase, unrelated Noct features, or a new public zedBSD UAPI.

## Execution result

Finished on 2026-08-28 with `ws008-p004` complete. Canonical NoctLang commit
`eba2043ca74b8601d68a405ecbbeca50ca8d5ac0` was pushed to official main. The
zedBSD gitlink was replaced by a single pinned Makefile, `NOCT-T030`--`T034`
host acceptance passed, and the final non-JIT, BeUI/evdev, and RW-to-RX JIT
QEMU cells all passed from `userland/noct/NoctLang`. No human-decision boundary
or uncleared item remains in q022.
