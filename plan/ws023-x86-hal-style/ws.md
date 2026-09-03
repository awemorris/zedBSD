# WS023: i386/amd64 HAL coding-style conformance

Last updated: 2026-09-03

WSID: `ws023`

Status: complete (`q067`)

Parent: [master plan](../master.md)

## Objective

Apply [the project C coding style](../coding-style.md) to every maintained C
source and header below `src/hal/i386/` and `src/hal/amd64/`. Restore the
i386 implementation from mechanically compressed source to reviewable,
debugger-friendly C, and bring the amd64 HAL to the same source contract.

This is a behavior-preserving source-quality workstream. It must not use
formatting as authority to change a HAL API or ABI, structure layout, firmware
handoff, register access, interrupt state, lock ordering, timing, ownership, or
error semantics.

## Audited baseline

- i386 contains 24 C files and 19 headers, 6,801 lines in total. The principal
  compressed files are `acpi.c`, `mps.c`, `ioapic.c`, `lapic.c`,
  `percpu.c`, `smp.c`, and the two BSP PIT implementations.
- amd64 contains 25 C files and 20 headers, 7,661 lines in total. Only 2 of 348
  function definitions already use the required definition layout; 26
  functions have a collapsed one-line body.
- The inventory found widespread missing modelines, canonical headers,
  forward declarations, public/static function comments, leading declaration
  groups, semantic paragraph comments, and explicit return paths.
- Five i386 files were already staged before this WS was created:
  `multiboot.h`, `page.c`, `percpu.c`, `pic.h`, and `smp.c`. The user committed
  them as `b4be6eb` before authorizing q067. They remain fully in scope and are
  reviewed as inherited edits rather than treated as already conforming.

## Fixed boundaries

- Scope is all maintained `.c` and `.h` files below the two architecture
  directories. Assembly sources are outside this C-style pass.
- Preserve exact short-circuit evaluation, I/O and MMIO access order, barriers,
  volatile semantics, interrupt enable state, lock ordering, allocation
  ownership, and failure values.
- Reordering file sections may not alter initialization order, emitted object
  layout, symbol linkage, conditional compilation, or test-only variants.
- Every normal static function receives a forward declaration. Public
  definitions precede static definitions unless conditional compilation makes
  that impossible; any exception is recorded in the Phase.
- Ordinary source uses ANSI C declaration placement. Existing compiler
  extensions needed for ABI/layout or clear static tables are retained rather
  than mechanically translated. Each retained exception is recorded; it is
  not precedent for unrelated new syntax.
- `UNUSED_PARAMETER` is a private source helper, duplicated in each
  architecture's private `defs.h`. It is not a new public HAL interface.
- Large tables such as the PC-98 JIS mapping retain their data values and
  indexing representation. Their surrounding file envelope/declarations are
  styled without payload churn.
- No generated prose comments are accepted. Comments describe actual intent,
  hardware constraints, ownership, or result semantics.
- A discovered behavior defect is recorded for a separate Phase. It is not
  repaired covertly in this style-only WS.

## Phase registry

| Phase | Status | Required result |
| --- | --- | --- |
| [ws023-p001](phase001-i386-compressed-core/phase.md) | Complete (`q067`) | Preserve the inherited committed edits and restore the compressed i386 APIC/SMP/page/time sources to the canonical form |
| [ws023-p002](phase002-i386-core-interrupts/phase.md) | Complete (`q067`) | Style the remaining i386 low-level, interrupt, and library sources and headers |
| [ws023-p003](phase003-i386-vm-task/phase.md) | Complete (`q067`) | Style i386 address-space and task/context code without changing MMU or context semantics |
| [ws023-p004](phase004-i386-pcat/phase.md) | Complete (`q067`) | Style the i386 PC/AT boot, console, and PIC implementation |
| [ws023-p005](phase005-i386-pc98/phase.md) | Complete (`q067`) | Style the i386 PC-98 boot, console, display, PIC, and table envelopes |
| [ws023-p006](phase006-amd64-leaf-core/phase.md) | Complete (`q067`) | Establish the amd64 file envelope/private helper and style the small leaf/core modules |
| [ws023-p007](phase007-amd64-interrupt-task/phase.md) | Complete (`q067`) | Style amd64 interrupt, page, SMP, library, and task code |
| [ws023-p008](phase008-amd64-platform-time/phase.md) | Complete (`q067`) | Style amd64 firmware handoff, APIC, clock, and timecounter code |
| [ws023-p009](phase009-amd64-vmspace/phase.md) | Complete (`q067`) | Style the large amd64 address-space implementation independently |
| [ws023-p010](phase010-amd64-console/phase.md) | Complete (`q067`) | Style the large amd64 console/input implementation and all compiled variants |
| [ws023-p011](phase011-x86-final-audit/phase.md) | Complete (`q067`) | Prove complete file coverage and run the combined x86 build/runtime regression |

## Completion conditions

- Every one of the 88 audited C/header files has the exact modeline, canonical
  copyright block, and an accurate file explanation.
- All function layout, declaration placement, section ordering, function
  comments, semantic paragraphs, loop/switch comments, debugger-friendly
  decisions, body symmetry, and return comments meet
  `plan/coding-style.md`, with only explicitly recorded ABI/table extensions.
- No prohibited `goto`, declaration in a `for` initializer, compressed
  multi-statement source, or direct meaningful-call return remains.
- The inherited `b4be6eb` i386 changes are accounted for in the review evidence
  and preserved in the final result.
- Focused HAL fixtures pass, all four configured x86 images build with
  `make -j16`, and i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI reach
  their bounded QEMU acceptance markers.
- `git diff --check` passes and the final diff contains no public HAL/API/ABI
  or behavior change.

## Completion result

Q067 completed all eleven Phases.  The audit accounts for all 88 C/header
files, strict and focused regressions pass, all four configured images build,
and the PC/AT, PC-98, amd64 BIOS SMP, and amd64 UEFI SMP runtime cells reach
their acceptance markers.  See the
[q067 result ledger](tests/q067-results.md) for exact evidence, retained
extensions, and pre-existing risks that were intentionally not repaired by
this style-only Workstream.
