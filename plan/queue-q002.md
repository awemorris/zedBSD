# Queue: software-only follow-up set

Last updated: 2026-08-25

QID: `q002`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q001](queue-q001.md)

## 1. Purpose

This document selects the next bounded, executable work from the resume points
left by `q001`. It is an execution index: detailed design, work packages,
tests, results, and interruption state belong in the owning WS and Phase
documents.

The physical Dell Latitude inventory and the X11 mouse defect are intentionally
not reopened in this Queue. Both retain their existing carry-forward conditions.
`q002` concentrates on work that can proceed in the current software/QEMU
environment.

## 2. Status model

Every item uses exactly one state:

| State | Meaning |
| --- | --- |
| pending | No Phase extraction or implementation has begun in this Queue |
| in-progress | Phase extraction, implementation, or verification is active |
| completed | The local completion conditions have passing evidence |
| uncleared | The item was attempted but cannot be cleared; evidence, reason, and a concrete resume condition are recorded |

`Queue finished` becomes **Yes** when every item is completed or uncleared,
the affected Phase/WS/master documents agree, and no item remains pending
or in-progress. A finished Queue may therefore contain explicit carry-forward
work.

## 3. Execution registry

| Priority | Queue item | Owning WS / Phase | Authoritative documents | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | PCIe ECAM/MSI prerequisites for xHCI | `ws004-p002` | [Master](master.md), [WS004](ws004-hardware/ws.md), [Phase](ws004-hardware/phase002-pcie-xhci-prerequisites/phase.md), [foundation audit](ws004-hardware/phase001-foundation-audit/audit.md), [tests](ws004-hardware/tests/README.md) | completed | Freeze the QEMU-testable ECAM, BAR, interrupt, DMA, and teardown contract needed by xHCI; implement and verify only missing common prerequisites that are independent of Latitude-only evidence |
| 2 | Interactive `net` console | `ws011-p002` | [WS011](ws011-net-config/ws.md), [Phase](ws011-net-config/phase002-console/phase.md), [tests](ws011-net-config/tests/README.md) | completed | Argument-free `net` enters the three-mode console; interactive and argv operations share validation, errors cannot mutate state, and exit/EOF/unsaved-change behavior is bounded and tested |
| 3 | Kernel input core and event device | `ws006-p002` | [WS006](ws006-input/ws.md), [Phase](ws006-input/phase002-input-core/phase.md), [tests](ws006-input/tests/README.md), [public reference](../docs/reference/evdev.md) | completed | Registration, event fan-out, per-reader buffering, overflow/resync, nonblocking read, poll, detach, and permissions pass focused tests without yet migrating producers or consumers |
| 4 | Build-from-source guide | `ws009-p002` | [WS009](ws009-documentation/ws.md), [Phase](ws009-documentation/phase002-build-guide/phase.md), [guide](../docs/howto/build-from-source.md), [tests](ws009-documentation/tests/README.md) | completed | A clean documented procedure covers prerequisites, `make toolchain`, `make -j16`, x86 image outputs, and supported QEMU launch paths, and its commands are reproduced and link-validated |
| 5 | POSIX `dirname` bounded proof/correction | `ws001-p012` | [WS001 ledger](ws001-posix/ws.md), [Phase](ws001-posix/phase012-dirname/phase.md), [tests](ws001-posix/tests/README.md) | completed | `dirname` double-slash, all-slash, trailing-slash, empty/long operand, locale-independent lexical behavior, usage, and broken-stdout cases are corrected or precisely handed off with focused host and native-build evidence |

## 4. Order and dependency rules

The priority order expresses project value, not a requirement to serialize all
work.

1. `ws004-p002` is limited to software/QEMU facts. It must not invent Latitude
   ECAM, IOMMU, interrupt-remapping, or controller facts while `ws003-p001`
   remains carried forward.
2. `ws011-p002` consumes the model frozen in `ws011-p001`. Persistent boot
   migration remains `ws011-p003` and is outside this Queue.
3. `ws006-p002` consumes the ABI frozen in `ws006-p001`. Existing console
   producer bridging, Xzed migration, legacy-console removal, and USB HID stay
   in later Phases.
4. `ws009-p002` may document only commands reproduced against the current
   tree. It must distinguish supported procedures from planned hardware paths.
5. `ws001-p012` follows the small, bounded `basename` Phase pattern and must
   not expand into a general utility sweep.

## 5. Execution and verification policy

- Do not commit changes.
- Do not use `make check`; it is not a project acceptance target.
- Use focused tests copied under the owning `plan/wsXXX-*/tests/` directory.
  Do not consume `.internal/` as a test source during execution.
- Use `make -j16` for the repository build gate when the Phase changes buildable
  code.
- Use `qemu-system-x86_64` for required amd64 runtime evidence. A Phase may use
  host tests and compile-only ABI tests where its local completion conditions
  do not require a guest runtime.
- Do not weaken tests or claim target-hardware behavior from QEMU evidence.
- If a finding requires redesign outside the selected Phase, stop that item,
  record the evidence and resume condition, and mark it uncleared rather
  than forcing implementation.

## 6. Per-item update record

Update this table whenever an item changes state. Evidence should live in the
created Phase document and be linked here rather than duplicated.

| Queue item | Status | Last verified result | Blocker or carry-forward reason | Next action |
| --- | --- | --- | --- | --- |
| 1 | completed | Extended config/capability and bridge-tree fixtures, DMA/rescan regressions, native kernel/image, and QEMU boot pass | Actual MCFG/MSI and Latitude facts remain outside this Phase | Extract MCFG/vector work or QEMU-only xHCI INTx scope |
| 2 | completed | NCLI host suite, native `net`, full image, and QEMU login pass | Atomic save/boot migration remains `ws011-p003` | Resume at `ws011-p003` |
| 3 | completed | LP64/ILP32 ABI, multiple-reader/overflow queue, native kernel/image, and QEMU boot pass | No real producer is connected by IN-01 | Extract `ws006-p003` for IN-02 and guest event-node evidence |
| 4 | completed | Toolchain, help/target audit, full image, amd64 QEMU marker, and 283 links pass | PC-98 still requires its custom QEMU | Resume DOC-40 or producer-linked documentation |
| 5 | completed | Focused host suite, native dirname ELF, full image, and QEMU boot pass | Locale/allocation/direct guest failure proof remains | Select another bounded tier-1 utility |

## 7. Explicitly deferred from q002

- `ws003-p001` remains uncleared until the physical Latitude 5320 or a
  trustworthy inventory is available.
- `ws007-p002` remains uncleared until the original defect can be
  reproduced or WS006 has supplied the evdev/Xzed path.
- `ws011-p003` persistence and boot migration begins only after the interactive
  console contract is complete.
- `ws006` producer bridging, console consumption, consumer migration, legacy
  removal, and USB HID are later Phases.
- WS005 physical networking, WS008 Noct/BeUI, GPU, Vulkan, GLES, and Wayland are
  not selected for this Queue.

## 8. Queue closure checklist

- [x] Every registry item is completed or uncleared.
- [x] Every extracted Phase has an authoritative `phase.md` and test references.
- [x] Focused evidence is recorded without using `.internal/` or `make check`.
- [x] Carry-forward items name their evidence, blocker, dependency, and resume point.
- [x] Affected WS registries and resume points are current.
- [x] `plan/master.md` reflects the resulting WS states.
- [x] `Queue status` is `finished` and `Queue finished` is **Yes**.

## 9. Closure

`q002` closed with all five selected items completed. The common integration
gate was `make -j16`, followed by an explicit 25-second
`qemu-system-x86_64` run of `build/amd64/hdd-image.img`; debug output reached
`init: system running` and `login:`. No `.internal/` test, `make check`, commit,
or target-hardware claim was used.
