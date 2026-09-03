# WS023-P005: Conform the i386 PC-98 BSP

Last updated: 2026-09-03

Phase ID: `ws023-p005`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p004`

## Objective

Apply the canonical style to the PC-98 BSP, including the large console and
display paths, without changing the native IPL, text/graphics, bus-mouse, or
PIC-cascade behavior.

## Scope

- `bsp-pc98/boot.c`, `cons.c`, `display.c/h`, `dma.h`, and `pic.c`
- `bsp-pc98/keyboard-map.h` and `jisx0208.c`

The PIT implementation was handled in p001.

## Procedure

1. Style boot, console/input, display, and PIC paths as independently reviewed
   semantic sections.
2. Replace prohibited cleanup `goto` paths with small local helpers or
   explicit returns while preserving release order.
3. Hoist declarations and remove declared-`for` syntax without changing
   translation tables or hardware loops.
4. Add canonical envelopes to mapping/table files but do not mechanically
   rewrite their data payload.

## Verification

- Run `git diff --check`, `hal-pc98-compile`, the PC-98 PIC-cascade fixture,
  the input-ownership fixture, and a full configured PC-98 build using
  `make -j16`.
- Run maintained qemu-pc98 boot to exact `login:` and the retained cursor
  movement regression when its runner is available.

## Completion conditions

- Every i386 C/header file now meets the WS023 contract.
- IPL, console/display, evdev mouse, and PIC-cascade behavior are unchanged.
- Focused tests, full build, and maintained PC-98 runtime pass.

## Execution result

Every PC-98 BSP C/header file in scope now meets the canonical form while the
mapping payload remains unchanged.  PIC cascade, input ownership, Xzed cursor
movement, full build, and maintained PC-98 `login:` gates passed; see the
[q067 result ledger](../tests/q067-results.md).
