# WS007 Phase 003: PC-98 Xzed mouse PIC cascade

Last updated: 2026-08-31

Phase ID: `ws007-p003`

Status: Complete (`q039`)

Parent: [WS007](../ws.md)

Tests: [WS007 test index](../tests/README.md), especially `GFX-T02`

## Objective

Restore PC-98 Xzed relative mouse input by fixing the demonstrated 8259 slave
cascade lifecycle. Preserve Xzed's evdev-only input path and the independent
PC-98 bus-mouse driver; neither layer may bypass interrupt delivery.

## Reproduction and root cause

The production PC-98 image reaches Xzed and opens `/dev/input/event1`.
Injecting five QEMU `mouse_move 20 10` operations leaves the cursor hotspot at
`(320,240)`. The slave PIC correctly has IRQ13 unmasked (`IMR=df`), but the
master PIC remains `IMR=fc`, which masks its IRQ7 cascade. The pending request
is visible in both PICs and cannot reach the kernel.

Manually unmasking master IRQ7 through the QEMU monitor immediately delivers
the accumulated motion and moves the cursor to `(420,290)`, exactly
`+100,+50`. This closes Xzed, evdev framing, coordinate conversion, and the
bus-mouse sample logic as causes. The defect is
`src/hal/i386/bsp-pc98/pic.c::pic_set_irq_mask()` failing to maintain the
master cascade when a slave IRQ changes state.

## Fixed decisions

1. PC-98 master IRQ7 is the slave-cascade input, not a general device IRQ.
2. Unmasking any slave IRQ must unmask master IRQ7.
3. Masking one slave IRQ must retain the master cascade while any other slave
   IRQ is unmasked. Only masking the final slave IRQ may mask master IRQ7.
4. A direct request to mask reserved IRQ7 must not disconnect active slave
   IRQs. Initialization still starts with both PICs fully masked.
5. Preserve unrelated master IRQ bits and the exact slave mask across every
   transition. Do not infer state from a single driver's lifecycle.
6. Acceptance must traverse the production PIC, bus-mouse, evdev, and Xzed
   path; a host test which calls the mouse service directly is insufficient.

## Implementation plan

1. Give the PC-98 PIC implementation explicit master/slave mask state and one
   update rule for its reserved cascade.
2. Add a focused host fixture covering initialization, one slave, two
   concurrent slaves, final masking, direct cascade-mask safety, and unrelated
   master IRQ preservation.
3. Build the ordinary PC-98 image with `make -j16` and run existing input/HID
   and graphics ownership regressions applicable to the changed boundary.
4. Add a production qemu-pc98 regression which launches Xzed, identifies its
   cursor hotspot, injects five `mouse_move 20 10` operations, and proves the
   hotspot moves exactly from `(320,240)` to `(420,290)`. Include a button
   event if the monitor and stable Xzed screen expose an unambiguous oracle.
5. Confirm there is no manual monitor PIC unmask in the passing run and no
   panic, fault, or VFS failure.

## Completion conditions

- the focused PIC fixture proves `ff/ff` initialization, `7f/df` for only
  IRQ13 active, retained `7f/fd` after masking IRQ13 while IRQ9 remains, and
  final `ff/ff` after the last slave is masked;
- direct IRQ7 masking cannot close an active cascade and unrelated master
  masks remain intact;
- existing focused input/HID tests and `make -j16` pass;
- a fresh production qemu-pc98 Xzed run moves the cursor exactly `+100,+50`
  through `/dev/input/event1` without monitor-side PIC repair;
- `git diff --check` passes and reusable test instructions/evidence are
  recorded under WS007.

This Phase does not reopen the previously non-reproduced amd64 coordinate
report in p002 and does not add legacy `/dev/mouse` support.

## Result (2026-08-31)

The PC-98 PIC now keeps explicit master and slave masks and derives the
reserved master IRQ7 cascade from the complete slave mask. Unmasking the first
slave source opens IRQ7, masking one of several active slave sources retains
it, and masking the last slave source closes it. Direct attempts to mask IRQ7
cannot disconnect an active slave, and unrelated master bits are preserved.

The focused production-source fixture passes the required transition matrix:

```text
ff/ff -> 7f/df -> 7f/dd -> 7f/fd -> ff/ff
```

The fresh production qemu-pc98 regression booted the ordinary PC-98 image,
logged in, launched Xzed, and captured its standard cursor at `(320,240)`.
Five monitor `mouse_move 20 10` operations reached Xzed through the PC-98
bus-mouse driver and `/dev/input/event1`, moving the hotspot exactly to
`(420,290)`. The runner contains no direct PIC-port operation or manual IRQ7
repair. Its disposable disk left the production source image unchanged, and
the guest log contained no fatal, panic, or VFS failure.

Verification evidence:

| Gate | Result |
| --- | --- |
| strict focused PIC lifecycle fixture | PASS |
| focused PIC fixture with UBSan | PASS |
| WS018 input/HID ownership host gate | PASS |
| Xzed coordinate/clamp host gate | PASS |
| `make -j16` | PASS |
| `make check-disk-image` | PASS |
| production qemu-pc98 Xzed cursor gate | PASS: `(320,240) -> (420,290)` |
| immutable production image check | PASS |
| `git diff --check` | PASS |

Reusable commands and oracle details are recorded under `GFX-T02` in the
[WS007 test index](../tests/README.md). No physical checkpoint is needed for
this QEMU-reproducible regression.
