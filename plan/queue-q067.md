# Queue: i386/amd64 HAL coding-style conformance

Last updated: 2026-09-03

QID: `q067`

Queue status: finished

Queue finished: **Yes**

Authorization: the user explicitly approved q067 execution on 2026-09-03 and
confirmed that the previously staged i386 edits were committed and must also
be refactored.

Proposed timebox: none. Execute the finite Phases below in dependency order.

Parent: [master plan](master.md)

Previous Queue: [q066](queue-q066.md)

## Purpose

Restore the visibly compressed i386 HAL source first, then apply the same
canonical C style to the complete amd64 HAL. Preserve every hardware and ABI
behavior, and finish with one combined four-platform build/runtime campaign.

## Read-only intake

- i386: 24 C files plus 19 headers, 6,801 lines. Eight files contain the
  principal mechanically compressed implementation.
- amd64: 25 C files plus 20 headers, 7,661 lines. Only two of 348 production
  function definitions already have the complete required signature layout.
- Baseline commit `b4be6eb` contains the previously staged edits to five
  in-scope i386 files: `multiboot.h`, `page.c`, `percpu.c`, `pic.h`, and
  `smp.c`. P001 refactors and reviews them too; it does not treat that partial
  expansion as an exemption from the canonical style.
- Before planning, the staged baseline passed fresh PC/AT, PC-98, and amd64
  configured builds with isolated build roots.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws023-p001` | [Restore compressed i386 HAL core source](ws023-x86-hal-style/phase001-i386-compressed-core/phase.md) | completed | Preserve committed work and style the compressed APIC/SMP/page/PIT group |
| 2 | `ws023-p002` | [Conform the i386 core and interrupt sources](ws023-x86-hal-style/phase002-i386-core-interrupts/phase.md) | completed | Style remaining i386 low-level/interrupt/library code |
| 3 | `ws023-p003` | [Conform the i386 VM and task sources](ws023-x86-hal-style/phase003-i386-vm-task/phase.md) | completed | Style i386 MMU/task code with ABI and ownership preserved |
| 4 | `ws023-p004` | [Conform the i386 PC/AT BSP](ws023-x86-hal-style/phase004-i386-pcat/phase.md) | completed | Style PC/AT boot/console/PIC and retain QEMU login |
| 5 | `ws023-p005` | [Conform the i386 PC-98 BSP](ws023-x86-hal-style/phase005-i386-pc98/phase.md) | completed | Style PC-98 boot/console/display/PIC and retain login/input |
| 6 | `ws023-p006` | [Conform amd64 leaf and small core modules](ws023-x86-hal-style/phase006-amd64-leaf-core/phase.md) | completed | Establish amd64 conventions and style low-risk modules |
| 7 | `ws023-p007` | [Conform amd64 interrupt, page, SMP, and task code](ws023-x86-hal-style/phase007-amd64-interrupt-task/phase.md) | completed | Style core execution paths and retain BIOS/UEFI SMP |
| 8 | `ws023-p008` | [Conform amd64 boot, APIC, and time sources](ws023-x86-hal-style/phase008-amd64-platform-time/phase.md) | completed | Style order-sensitive firmware/controller/time code |
| 9 | `ws023-p009` | [Conform the amd64 address-space implementation](ws023-x86-hal-style/phase009-amd64-vmspace/phase.md) | completed | Style the large VM module independently |
| 10 | `ws023-p010` | [Conform the amd64 console and input implementation](ws023-x86-hal-style/phase010-amd64-console/phase.md) | completed | Style every production/test branch of the large console module |
| 11 | `ws023-p011` | [Complete the x86 HAL style and regression audit](ws023-x86-hal-style/phase011-x86-final-audit/phase.md) | completed | Account for all 88 files and pass the four-platform final matrix |

## Fixed execution rules

- `plan/coding-style.md` is authoritative. Formatting tools may assist but
  cannot synthesize semantic comments or decide behavior.
- Preserve evaluation order, symbol linkage, object/layout ABI, volatile and
  port-I/O order, barriers, IRQ/lock state, timer semantics, ownership, and
  observable results.
- No public HAL API or ABI change is authorized. The unused-parameter helper is
  private and duplicated in the two architecture-local `defs.h` files.
- Keep ABI-required compiler extensions and clearer static-table
  initializations only as recorded exceptions; do not broaden their use.
- A suspected functional defect is recorded outside q067. Do not repair it
  under style scope.
- Use `make -j16`; do not run aggregate `make check`.
- After each completed Phase, update M/W/P/Q evidence, commit the scoped
  checkpoint as `WIP`, merge current `origin/main` before publishing, and
  push. If publishing is rejected, retain the local commit and continue only
  when the next Phase does not depend on remote publication.

## Verification order

1. Snapshot the inherited staged diff and run the i386 compressed-core Phase.
2. Complete i386 core, VM/task, PC/AT, and PC-98 in isolated review batches,
   compiling each batch and running runtime gates at the BSP boundaries.
3. Complete amd64 leaf/core, interrupt/task, platform/time, VM, and console
   batches, running focused tests and firmware-mode runtime gates where their
   behavior is touched.
4. Re-audit all 88 files and run focused regressions.
5. Build all four CI configurations in fresh isolated build roots.
6. Run i386 PC/AT, maintained PC-98, amd64 BIOS SMP, and amd64 UEFI SMP bounded
   QEMU acceptance.

## Stop conditions

Mark only the affected Phase `uncleared` and stop or continue to an independent
item as permitted if:

- preserving a `goto` cleanup path requires a functional ownership redesign;
- canonical ordering would change linkage, initialization, or compiled layout;
- a public HAL/API/ABI change appears necessary;
- a focused or runtime regression cannot be resolved without changing behavior;
- the inherited staged edits contain intent that cannot be determined from
  source/history.

## Completion definition

Q067 finishes when all eleven items are either completed or honestly
uncleared, the final file ledger accounts for all 88 C/header files, no
in-scope user edit is lost, and M/W/P/Q contain the exact test and publication
results.

## Execution result

- All eleven Phases completed.  Every one of the 88 maintained x86 HAL C/header
  files is accounted for and passes the mechanical audit.
- Independent semantic and object-symbol review found no public HAL API, ABI,
  layout, linkage, or intended behavior change.  Review-discovered evaluation
  and I/O-order drift was corrected before closure.
- Focused and sanitizer fixtures, strict architecture compile gates, all four
  configured image builds, and the four required QEMU runtime cells passed.
- Existing concurrent assembly/editor changes were outside the C/header scope
  and were neither modified nor staged by q067.
- Exact evidence and retained baseline risks are recorded in the
  [q067 result ledger](ws023-x86-hal-style/tests/q067-results.md).
- The eleven Phase checkpoints `800808e` through `7f4b9bc` were published to
  `origin/main` after the required origin merge check completed as a no-op.
