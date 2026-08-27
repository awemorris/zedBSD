# Queue: runtime swap control

Last updated: 2026-08-28

QID: `q021`

Queue status: active

Queue finished: **No**

Authorization: selected automatically under the user's approved WS-priority
Queue loop on 2026-08-28; Phase-boundary commit and push are authorized for the
zedBSD repository. If push is rejected by the environment, retain the local
commit and continue.

Timebox: the current continuous execution cycle; stop when every Queue item has
been processed or no dependency-ready item remains

Parent: [master plan](master.md)

Previous Queue: [q020](queue-q020.md)

## Purpose

Implement the fixed WS016 runtime-swap design as one dependency-ordered Queue:
first replace the immutable boot aggregate with a safe four-source runtime
manager, then expose it through `/dev/system`, add the native administration
commands, and prove the complete path in disposable amd64 QEMU images.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws016-p001` | [WS016](ws016-swap-control/ws.md), [Phase](ws016-swap-control/phase001-runtime-swap-manager/phase.md), [tests](ws016-swap-control/tests/README.md) | complete | Stable source-encoded slots, dynamic add/drain/remove, backing claims, and live commit accounting pass focused host tests and regressions |
| 2 | `ws016-p002` | [WS016](ws016-swap-control/ws.md), [Phase](ws016-swap-control/phase002-swap-uapi/phase.md), [tests](ws016-swap-control/tests/README.md) | complete | Versioned privileged `/dev/system` control and source enumeration pass ABI, permission, and failure-atomicity tests |
| 3 | `ws016-p003` | [WS016](ws016-swap-control/ws.md), [Phase](ws016-swap-control/phase003-swap-commands/phase.md), [tests](ws016-swap-control/tests/README.md) | in-progress | `/sbin/swapon` and `/sbin/swapoff` implement the fixed multi-operand CLI and are installed in configured images |
| 4 | `ws016-p004` | [WS016](ws016-swap-control/ws.md), [Phase](ws016-swap-control/phase004-runtime-swap-acceptance/phase.md), [tests](ws016-swap-control/tests/README.md) | pending; dependency-gated by p001--p003 | Disposable amd64 QEMU images prove runtime add, page-out/in, safe drain/remove, failure preservation, and boot-swap regression |

## Entry evidence and dependency order

- `ws003-p014` completed signed `ZEDSWAP1`/`ZEDSWAP2` file/raw sources,
  direct I/O, boot aggregation, integrity checks, and multi-source behavior.
- `ws003-p015` completed the four-platform boot-parameter matrix and supplies
  the boot-swap regression baseline reused by p004.
- The WS016 runtime model, stable token encoding, backing claim, drain rules,
  versioned UAPI, command contract, and reconsideration boundaries are fixed.
- p002 starts only after p001 completes; p003 starts only after p002 completes;
  p004 starts only after p001--p003 complete. An uncleared predecessor leaves
  every dependent Phase unexecuted.

## Ordered execution

1. Execute [ws016-p001](ws016-swap-control/phase001-runtime-swap-manager/phase.md)
   and verify the source manager, backing claims, drain, and commit accounting.
2. On p001 completion, execute
   [ws016-p002](ws016-swap-control/phase002-swap-uapi/phase.md) and verify the
   versioned `/dev/system` ABI and failure-atomic control boundary.
3. On p002 completion, execute
   [ws016-p003](ws016-swap-control/phase003-swap-commands/phase.md) and verify
   the two installed native `/sbin` commands.
4. On p001--p003 completion, execute
   [ws016-p004](ws016-swap-control/phase004-runtime-swap-acceptance/phase.md)
   and run the complete disposable-image amd64 QEMU acceptance.
5. After each processed Phase, synchronize P/W/M/Q/test evidence, run
   `git add -A`, `git commit -m WIP`, and `git push`. If push is rejected by
   the environment, retain the local commit and continue.

## Stop, defer, and continuation rules

- Execute only the fixed surfaces in the linked WS016 Phase books. Do not add
  formatting, priorities, `fstab`, UFS extent discovery, or a syscall/libc API.
- If a Phase reaches its reconsideration boundary, record it as `uncleared`
  with the facts learned and concrete resume condition, then continue with any
  remaining dependency-ready Queue item. Do not execute a dependent Phase
  whose predecessor is not complete.
- Use `make -j16`, never `make check` or `.internal/`. Runtime acceptance uses
  `qemu-system-x86_64` and disposable copies of the amd64 disk image.
- Preserve all existing boot-selected swap, VM, storage, and image-build
  behavior unless the owning Phase explicitly changes it.

## Approval boundary

The user's automatic Queue-loop authorization covers only `ws016-p001` through
`ws016-p004` in dependency order and the bounded repairs allowed by their Phase
books. A reconsideration boundary is recorded as `uncleared`; it does not
authorize a policy redesign or execution of dependency-gated successors.
