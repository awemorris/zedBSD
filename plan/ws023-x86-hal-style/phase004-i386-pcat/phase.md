# WS023-P004: Conform the i386 PC/AT BSP

Last updated: 2026-09-03

Phase ID: `ws023-p004`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p003`

## Objective

Apply the canonical style to the i386 PC/AT boot, console/input, and PIC source
while retaining firmware, keyboard, display, and interrupt behavior.

## Scope

- `bsp-pcat/boot.c`
- `bsp-pcat/cons.c`
- `bsp-pcat/pic.c`

The PIT implementation was handled in p001.

## Procedure

1. Style boot parsing and firmware handoff without changing accepted boot
   records or their precedence.
2. Style console rendering, keyboard translation, input publication, and IRQ
   paths in semantic subsections.
3. Convert `for` declarations to leading C89 declarations and preserve table
   indices and loop bounds.
4. Style PIC setup/acknowledgement while retaining exact port-I/O order.

## Verification

- Run `git diff --check`, `hal-pcat-compile`, the input-ownership fixture,
  and a full configured PC/AT build with `make -j16`.
- Run a bounded PC/AT QEMU boot through exact `login:`.

## Completion conditions

- All three files meet the WS023 contract.
- Boot parameters, console/input behavior, and PIC ordering are unchanged.
- The focused gates and QEMU login pass.

## Execution result

The PC/AT boot, console/input, and PIC sources now meet the canonical form;
review retained slave-before-master PIC masking and the original cursor/event
state order.  Focused input tests, the full image build, and QEMU `login:`
passed; see the [q067 result ledger](../tests/q067-results.md).
