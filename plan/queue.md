# Queue: static boot defaults, independent BeUI backends, and shell TTY ordering

Last updated: 2026-08-28

QID: `q023`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-28 by directing the
agent to Phase the static boot-parameter design, place all executable Phases
in a Queue, and execute autonomously. The standing authorization to commit and
push after completed Phases remains active. For `ws008-p005`, this explicitly
includes changes, verification, commit, and publication in both canonical
`awemorris/NoctLang` and zedBSD.

Timebox: no fixed wall-clock limit; the scope is the finite three-Phase
registry below. Continue until every item is completed or recorded
`uncleared`. A human-decision boundary defers only the affected item and does
not stop dependency-independent later items.

Parent: [master plan](master.md)

Previous Queue: [q022](queue-q022.md)

## Purpose

Remove a post-WS010 generated-source/Python regression from x86 boot images,
apply the agreed independent-platform BeUI architecture upstream, and close
the shell's two remaining foreground job-control ordering races.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p016` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase016-boot-parameter-header-dependency/phase.md), [tests](ws003-bringup/tests/README.md) | completed | One maintained source default feeds every x86 loader and kernel fallback; generated/Python inputs disappear and affected four-platform and swap evidence remains usable |
| 2 | `ws008-p005` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase005-independent-beui-backends/phase.md), [tests](ws008-noct/tests/README.md) | completed | Each selected canonical BeUI platform source independently implements the sole public `noct_register_api_beui()` interface; shared dispatcher/backend and public HAL injection disappear |
| 3 | `ws001-p014` | [WS001](ws001-posix/ws.md), [Phase](ws001-posix/phase014-shell-job-control/phase.md), [tests](ws001-posix/tests/README.md) | completed | Foreground pipeline children cannot read before TTY handoff, and `fg` foregrounds before `SIGCONT`, without background/non-TTY regression |

## Entry evidence and decisions

- `ws003-p016` depends only on completed p011, p012, p015, WS010, and
  `ws016-p004`. The product decision is fixed: production image parameters are
  maintained source; test variation patches only disposable artifacts with
  Noct.
- The boot-parameter Python generator was introduced on 2026-08-27, after
  WS010's 2026-08-25 x86 Python-removal acceptance. Its removal is regression
  repair, not a new boot grammar.
- `ws008-p005` is based on canonical NoctLang tree
  `595d5797a1c68844855b8a2a7e3d41846eab64e7`. The user resolved its only known
  interface decision: remove public `noct_register_api_beui_with_hal()` and
  expose only `noct_register_api_beui()`.
- `ws001-p014` depends on completed `ws012-p006`; its userspace synchronization
  design is fixed. A proven kernel TTY/process-group defect is a residual, not
  authorization for an implicit kernel ABI redesign.
- `ws002-p021` is excluded because its probabilistic diagnostic budget is not
  yet finite. Manually blocked WS011, WS013 Runtime, WS014, WS015, WS017, WLAN,
  and VLAN/bridge work remains outside this Queue.

## Execution procedure

1. Execute each item in registry order and synchronize P/W/M/Q evidence after
   its verification contract is satisfied or its stop boundary is reached.
2. Run `make toolchain` before project-owned Noct scripts are needed. Do not
   introduce Python into a supported production path, use `make check`, or
   consume `.internal/`.
3. Use `make -j16` for repository build gates and `qemu-system-x86_64` for
   amd64 runtime verification. Use disposable image copies for writable tests.
4. After a completed Phase, commit `WIP` and push. For p005, publish the tested
   canonical NoctLang commit first, then pin and publish the zedBSD commit. If a
   push is rejected, retain the local commit, record the rejection, and
   continue where safe.
5. If a Phase reaches a human decision or its reconsideration boundary, mark
   it `uncleared`, record facts and the exact resume condition in P/W/M/Q,
   report the requested decision in chat, and continue with the next
   dependency-independent item.

## Scope and stop rules

- Do not add a replacement production boot-parameter generator, hidden
  compile-time file selector, `boot.cfg` implementation, or parameter syntax.
- Do not preserve a public BeUI HAL-injection API, add a multi-backend binary,
  or replace the removed shared BeUI source with another common implementation.
- Do not redesign the shell parser, jobs model, or kernel ABI. If a focused
  reproducer proves a kernel defect, extract and defer that residual.
- Findings outside the three Phase books are documented and returned to the
  planning pool rather than silently absorbed.

## Completion definition

q023 finishes when all three registry items are either `completed` with their
required evidence or `uncleared` with a concrete resume condition, all result
states are synchronized across P/W/M/Q, and every safe completed change is
committed and pushed under the recorded authorization.

## Execution log

- `ws003-p016` completed on 2026-08-28. `BR-T47a/b` passed at
  `plan/ws003-bringup/temp/q023-p016-audit-final.8oHUQf`; the authoritative
  four-path matrix passed 31/31 at
  `plan/ws003-bringup/temp/q023-p016-br-t46-authoritative.9rIQrm`; and the
  affected WS016 runtime-swap cells passed 3/3 at
  `plan/ws016-swap-control/temp/q023-p016-runtime-swap.UGNqkG`.
- Queue execution advanced to `ws008-p005`.
- `ws008-p005` completed on 2026-08-28. Canonical NoctLang commit
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621` was pushed and pinned in both
  zedBSD acquisition paths. `NOCT-T040/T043` passed at
  `plan/ws008-noct/temp/q023-p005-backends.P8ZsxV`; canonical generic/SDL2/
  PC-98 tests passed; and `NOCT-T044` passed at
  `plan/ws008-noct/temp/q020-p002-beui.unH7qL`. Non-JIT and JIT QEMU
  regressions also passed at `q019-p001-noct.Q5DH4P` and
  `q020-p003-jit.0mYri3`.
- Queue execution advanced to `ws001-p014`.
- A post-completion `ws003-p016` patcher audit found that instruction bytes in
  `BOOTX64.EFI` can contain the four-byte `BPR1` magic. The Phase-owned Noct
  helper now identifies a record from its fixed structural fields before full
  validation. Its self-test and an actual disposable UEFI patch at offset
  16512 both passed, and the production loader remained unchanged.
- `ws001-p014` completed on 2026-08-28. The deterministic host fixture passed
  foreground-pipeline gate ordering, TTY-before-`SIGCONT`, a real background
  `SIGTTIN` stop, non-TTY bypass, and three injected cleanup failures at
  `plan/ws001-posix/temp/q023-p014-job-control.LdD3qT`; an injected first
  `fg` handoff failure also retained the same job for a successful retry. The
  installed amd64
  `/bin/sh` passed foreground pipeline, Ctrl-Z/`fg`, background-reader/`fg`,
  non-TTY, fatal-scan, and input-integrity acceptance at
  `plan/ws001-posix/temp/q023-p014-qemu.PhJyEq`; strict marker counts distinguish
  reader output from terminal input echo.
- All three q023 items are completed; the Queue is finished.
