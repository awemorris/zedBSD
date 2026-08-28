# WS001 Phase 014: shell foreground job-control synchronization

Last updated: 2026-08-28

Phase ID: `ws001-p014`

Status: Completed (`q023`, 2026-08-28)

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Remove the two remaining ordering races in the shell's bounded job-control
implementation: a foreground pipeline must not read its controlling terminal
before its process group is foreground, and `fg` must foreground a stopped job
before sending `SIGCONT`. Preserve the direct foreground-external handoff fixed
by `ws012-p006` and prove that background and non-TTY execution did not change.

This is a focused synchronization Phase, not a redesign of the shell parser or
a complete POSIX jobs implementation.

## Accepted baseline and residual

`ws012-p006` fixes the single foreground external-command path with a
close-on-exec pre-exec gate: the child enters its new process group, the parent
assigns that group to the controlling terminal, and only then may the child
execute. That path and its terminal restoration are accepted inputs to this
Phase and receive regression coverage rather than a second implementation.

Two races remain:

1. The foreground pipeline path forks children which may enter
   `pipeline_child()` before the parent finishes creating the group and calls
   `tcsetpgrp()`. A first pipeline member which reads the terminal can therefore
   receive `SIGTTIN` and stop before the pipeline is foreground.
2. The `fg` builtin currently sends `SIGCONT` before `wait_foreground()` calls
   `tcsetpgrp()`. A resumed terminal reader can run in that interval, receive
   `SIGTTIN`, and stop again.

Neither residual requires a product or policy decision. The existing
single-`last_job` model, process-group ownership, command grammar, and terminal
UI remain unchanged. The producing `ws012-p006` Queue item is complete, so this
Phase is Queue-ready.

## Scope

In scope:

- a pre-execution barrier shared by every member of a foreground pipeline on a
  controlling TTY;
- one process group for the complete pipeline, assigned to the terminal before
  any member may execute terminal-reading command code;
- `fg` ordering of terminal handoff, `SIGCONT`, wait, stopped-state retention,
  and terminal restoration;
- failure cleanup for partial pipeline creation, barrier release, signal, wait,
  and terminal-handoff errors;
- regression coverage for a single external command, a terminal-reading
  foreground pipeline, Ctrl-Z followed by `fg`, a background terminal reader,
  and non-TTY execution.

Out of scope:

- multiple simultaneous remembered jobs or `%job` selection;
- terminal-mode save/restore beyond the current shell contract;
- parser, expansion, redirection, trap, or pipeline-status redesign;
- changing the kernel TTY/process-group ABI unless the accepted interfaces are
  shown defective by a focused reproducer.

## Design constraints

1. For an interactive foreground pipeline, each child establishes or joins the
   intended pipeline group and then waits on a bounded close-on-exec barrier.
   The parent confirms group membership, gives the group the controlling TTY,
   and only then releases all successfully created children.
2. A setup failure must not release children into command execution. All gate
   descriptors are closed on every path, partial children/groups are
   terminated and reaped, the shell process group is restored when necessary,
   and the original error remains reportable. Gate release must not expose the
   shell to `SIGPIPE` if a child disappears.
3. Background pipelines never receive the controlling TTY. A member which
   attempts a terminal read retains normal job-control behavior without taking
   the prompt away from the shell.
4. Non-TTY and command-substitution paths do not issue TTY foreground ioctls or
   wait on an interactive-only barrier.
5. `fg` retains the stopped job until the terminal handoff succeeds. It calls
   `tcsetpgrp()` before `SIGCONT`, then waits with stopped-state reporting and
   restores the shell group on every terminal-owned exit path. A failed
   handoff must not resume a job in the background or lose `last_job`.
6. The direct external-command gate from `ws012-p006` remains one shared
   behavior, including safe release, Ctrl-Z, child status, and prompt
   restoration. Do not introduce a parallel special case merely for the test.

## Work packages

1. Copy or create a phase-owned PTY/job-control fixture under
   `plan/ws001-posix/tests/`; do not consume `.internal/` material.
2. Add deterministic checkpoints which record the child's process group and
   terminal foreground group immediately before its first terminal read.
3. Introduce the foreground-pipeline barrier and complete cleanup/error paths
   without changing background or non-TTY process semantics.
4. Reorder `fg` to complete terminal ownership before continuation and retain a
   stopped job across any pre-continuation failure.
5. Run focused fixture cases for:
   - a direct foreground external terminal reader;
   - a foreground pipeline whose first member reads the terminal;
   - Ctrl-Z, prompt recovery, `fg`, resumed input, and normal completion;
   - a background terminal reader which cannot steal foreground ownership;
   - piped/non-TTY stdin with no terminal handoff;
   - setup/release/wait failures with no leaked child, descriptor, or terminal
     ownership.
6. Run the focused cases repeatedly enough to exercise scheduler ordering, but
   use process-group checkpoints—not probability alone—as the pass criterion.
7. Format changed shell source, run `make -j16`, and perform bounded amd64 QEMU
   acceptance against the installed `/bin/sh`. Do not use `make check` or
   `.internal/`.
8. Record actual commands, results, and any kernel residual in this Phase and
   the WS001 test index before marking the Phase complete.

## Required evidence

- The direct external helper and every foreground pipeline member observe
  `tcgetpgrp(tty) == getpgrp()` before their first terminal read.
- A Ctrl-Z case returns one usable prompt; `fg` resumes the same group only
  after it owns the TTY, accepts input, exits, and returns one usable prompt.
- A background reader never becomes foreground and the shell remains usable;
  its stopped/continued status is not lost.
- A non-TTY simple command and pipeline complete without an interactive TTY
  ioctl or synchronization deadlock.
- Injected setup failures leave no gate descriptors, unreaped children, or
  foreign foreground process group.
- The phase fixture, `make -j16`, formatting check, `git diff --check`, and
  amd64 QEMU acceptance pass with recorded output.

## Completion conditions

This Phase is complete when no terminal-reading foreground pipeline member can
execute before its group owns the terminal; `fg` foregrounds before
continuation and preserves retryable stopped state on failure; the accepted
single-external behavior remains green; background and non-TTY cases retain
their prior semantics; cleanup evidence shows no leaked resources or terminal
ownership; and the production amd64 shell passes the focused QEMU regression.

This milestone narrows `SHELL-CORE-01`, `KERN-SIG-01`, and `KERN-TTY-01`. It
does not claim complete POSIX shell or job-control conformance.

## Reconsideration boundary

Mark the Phase `uncleared` and create a kernel-focused residual if a minimal
reproducer shows that `setpgid()`, `tcsetpgrp()`, `SIGTTIN`, `SIGTSTP`,
`SIGCONT`, or `waitpid(WUNTRACED)` violates the ordering contract despite the
userspace barriers. Do not mask a kernel defect with sleeps, scheduler-yield
loops, repeated retries without state checks, or relaxed assertions.

## Resume point

No work remains in this Phase. Select a new bounded item from the WS001 ledger;
do not infer complete POSIX shell or multi-job support from this synchronization
milestone.

## Execution result

Completed in q023 without reaching the kernel reconsideration boundary.

- Interactive foreground pipelines now create one close-on-exec gate. Every
  child establishes or joins the pipeline process group before waiting; the
  parent confirms membership for all children, transfers the controlling TTY,
  and only then releases exactly one byte per child.
- Setup, release, and wait failures close every gate/pipeline descriptor, kill
  and reap only still-live wrapper children, restore shell TTY ownership when
  it had been transferred, preserve the original error, and cannot terminate
  the shell through `SIGPIPE`.
- The bounded single-job model now remembers the wrapper PID of every pipeline
  member. `fg` preserves that state until successful TTY handoff, transfers the
  TTY before `SIGCONT`, waits every remembered member with stopped reporting,
  retains retryable members on failure, and restores the shell group.
- The deterministic Phase-owned fixture passed at
  `plan/ws001-posix/temp/q023-p014-job-control.LdD3qT`. Its event trace proves
  both pipeline children wait before handoff/release and proves TTY handoff
  precedes `SIGCONT`. It also observes a real background `SIGTTIN` stop,
  verifies non-TTY execution performs no gate/TTY operation, and injects
  `tcsetpgrp`, gate-write, and parent-wait failures with subsequent recovery
  and no live fixture PID. A separate handoff failure emits no `SIGCONT`,
  retains the same stopped process group, and succeeds on the next `fg`. A
  final ten-run repetition of this expanded corpus passed 10/10, from
  `q023-p014-job-control.wNj8wc` through
  `q023-p014-job-control.CZiqw6`; ordering checkpoints, not probability,
  remain the acceptance criterion.
- The production amd64 disk image passed the current strict QEMU acceptance at
  `plan/ws001-posix/temp/q023-p014-qemu.PhJyEq`: `/bin/head -n 1 | /bin/cat`
  accepted terminal input, a direct reader recovered through Ctrl-Z and `fg`,
  a background reader recovered through `fg`, an inner non-TTY shell ran, the
  outer prompt remained usable, fatal scans were empty, and the source image
  SHA-256 remained unchanged. Reader markers were each observed exactly twice
  (TTY echo plus copied output), so a stopped reader cannot pass by returning
  the marker to the outer shell as a command. The finalized runner repeated
  successfully at `plan/ws001-posix/temp/q023-p014-qemu.JmEqpw`.
- `clang-format` 19.1.7 formatting and dry-run checks, fixture warning-free
  builds, repeated focused runs, `make -j16`, script syntax checks, and
  `git diff --check` passed. `make check` and `.internal/` were not used.
