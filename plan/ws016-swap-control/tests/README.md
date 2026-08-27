# WS016 shared test cases

Parent: [WS016](../ws.md)

Reusable host fixtures live in this directory. Disposable QEMU evidence is
kept under `../temp/` when the owning Phase reaches runtime acceptance.

| Case ID | Owning Phase | Required observation |
| --- | --- | --- |
| SWAP-T001 | p001 | Source/local token encoding preserves IDs, boundaries, sentinel, and exact I/O routing |
| SWAP-T002 | p001 | Sparse boot IDs and runtime lowest-free IDs allocate in deterministic numeric order |
| SWAP-T003 | p001 | Duplicate inode/disk aliases, root overlap, malformed headers, and unsupported files unwind every claim |
| SWAP-T004 | p001 | Write/truncate/rename/unlink/loop and separately mounted writable aliases return `EBUSY` while active |
| SWAP-T005 | p001 | Concurrent fault/allocation/drain reaches zero target ownership before removal; injected failure retains a usable source |
| SWAP-T006 | p001 | Add/remove updates commit capacity atomically and rejects a limit below reserved commitment |
| SWAP-T007 | p002 | Versioned 32/64 UAPI layout, bounds, pointers, strings, reserved fields, and copyout are exact |
| SWAP-T008 | p002 | Root control, non-root `EPERM`, enumeration, canonical matching, and interrupted failure atomicity pass |
| SWAP-T009 | p003 | `swapon` parsing, `--`, operand order, diagnostics, continuation, and exit status pass |
| SWAP-T010 | p003 | `swapoff` parsing, `--`, operand order, diagnostics, continuation, and exit status pass |
| SWAP-T011 | p004 | amd64 QEMU add/use/drain/remove/reuse preserves page patterns and reports coherent source/aggregate stats |
| SWAP-T012 | p004 | Negative runtime cases preserve the old pool and representative q015 file/raw/mixed boot cases remain passing |

## Implemented p001 runners

- `run-swap-manager-test.sh`: SWAP-T001/T002 and BR-T45, including sparse boot
  IDs, stable token routing, PREPARED invisibility, empty-manager runtime
  activation, add/remove rollback, and source-ID reuse.
- `run-backing-claim-test.sh`: SWAP-T003/T004, including canonical FAT aliases,
  raw-volume exclusion, trusted filesystem writes, mount/teardown races,
  nested loop ownership, and early-boot execution ownership.
- `run-swap-drain-test.sh`: SWAP-T005, including successful page-in, injected
  I/O failure and retry, allocation/publication overlap, and an existing I/O
  owner.
- `run-swap-commit-resize-test.sh`: SWAP-T006, including pre-init seeding,
  expected-capacity serialization, rejected shrink, and exact-limit growth.
- `run-phase001.sh`: runs the complete SWAP-T001--T006 host gate.

The q021 p001 execution passed `run-phase001.sh`, an ASan/UBSan pass over the
manager, claim, and drain fixtures, a forced `make -B -j16`, and
`git diff --check` on 2026-08-28.

## Implemented p002 runners

- `run-system-swap-device-test.sh`: SWAP-T007/T008 UAPI layout, copyin/copyout,
  validation, effective-UID authorization, state mapping, initialized output,
  and failed-request atomicity.
- `run-swap-control-test.sh`: SWAP-T008 canonical file/raw identity, duplicate
  aliases, root overlap, signal interruption, source snapshots, and the
  post-claim missing/rebind race rejection.
- `run-boot-source-runtime-test.sh`: SWAP-T008 VFS lifetime adjunct.  It proves
  that all configured `boot0`--`boot3` mounts survive the former unused-slot
  release point, remain resolvable after a boot mount is promoted to the root,
  reject unpublished/unconfigured/non-`bootN` lookups, and cannot be destroyed
  after their system-lifetime publication.

The q021 p002 execution passed all three runners, p001 regressions, an
ASan/UBSan pass over the facade fixture, `make -j16`, and `git diff --check` on
2026-08-28.

## Implemented p003 runner

- `run-swap-command-test.sh`: SWAP-T009/T010 production-linked parsing,
  `--`, exact left-to-right requests, continuation after kernel/open failures,
  bounded selectors, diagnostics, and aggregate exit status for both commands.

The q021 p003 execution passed this runner in strict and ASan/UBSan variants,
native amd64 ELF and install-mode checks, PC/AT and PC-98 cross-ABI object
builds, `make -j16`, and `git diff --check` on 2026-08-28.

The supported build gate is `make -j16`; the aggregate `make check` target and
repository `.internal/` tests are not part of this WS.
