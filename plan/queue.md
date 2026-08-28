# Queue proposal: shell foreground job-control synchronization

Last updated: 2026-08-28

QID: `q022`

Queue status: proposed; execution approval pending

Queue finished: **No**

Authorization: not yet authorized. The prior automatic execution window ended
at 2026-08-28 09:00 JST; this Queue must not enter `in-progress` until the user
explicitly approves it and supplies or accepts a new timebox.

Timebox: pending user confirmation

Parent: [master plan](master.md)

Previous Queue: [q021](queue-q021.md)

## Purpose

Complete the bounded `/bin/sh` job-control synchronization residual left after
the accepted `ws012-p006` single-command gate. The Phase prevents foreground
pipeline members from reading the controlling terminal before their process
group owns it and makes `fg` transfer the terminal before sending `SIGCONT`.

## Proposed execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p014` | [WS001](ws001-posix/ws.md), [Phase](ws001-posix/phase014-shell-job-control/phase.md), [tests](ws001-posix/tests/README.md) | pending | Deterministic PTY checkpoints prove foreground pipelines and `fg` cannot race TTY ownership, while direct, background, and non-TTY behavior remains usable |

## Entry evidence and dependency order

- `ws012-p006` is complete and supplies the accepted pre-exec gate for one
  foreground external command.
- The remaining pipeline and `fg` races, cleanup rules, test checkpoints, and
  kernel reconsideration boundary are fixed in `ws001-p014`; no product-policy
  decision is currently open.
- This single-item Queue deliberately excludes `ws017-p001`: its device-mmap
  `mprotect` ceiling still needs the recorded human decision.
- `ws003-p016` is independently dependency-ready but remains outside this
  Queue so the selected WS001 correction stays finite and reviewable.

## Proposed execution

1. Create or copy a Phase-owned PTY fixture under `plan/ws001-posix/tests/`
   without using `.internal/`.
2. Add deterministic process-group/foreground-group checkpoints and implement
   the shared foreground-pipeline barrier plus `fg` handoff ordering.
3. Prove cleanup, stopped-state retention, background behavior, and non-TTY
   behavior with focused repetition and state assertions rather than sleeps.
4. Format changed shell source, run `make -j16`, perform bounded amd64 QEMU
   acceptance, and run `git diff --check`; do not run `make check`.
5. Synchronize P/W/M/Q evidence, commit with message `WIP`, and push. If push is
   rejected, retain the local commit and report the rejection.

## Stop and defer rules

- If a focused reproducer shows a kernel defect in `setpgid()`, `tcsetpgrp()`,
  `SIGTTIN`, `SIGTSTP`, `SIGCONT`, or `waitpid(WUNTRACED)`, mark the item
  `uncleared`, record the evidence, and extract a kernel-owned Phase. Do not
  hide it with sleeps or relaxed assertions.
- Do not redesign the parser, expansion, multiple-job UI, terminal-mode
  persistence, or general kernel TTY ABI in this Queue.
- Preserve the user-owned dirty `userland/noct` checkout and do not advance or
  publish its gitlink.

## Approval boundary

This book is a proposal only. Implementation begins after explicit approval of
`q022` and its timebox. Approval covers only `ws001-p014` and bounded defects
inside its Phase contract.
