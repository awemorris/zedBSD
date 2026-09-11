# WS007 Phase 002: X11 mouse-coordinate investigation

Last updated: 2026-08-25

Phase ID: `ws007-p002`

Status: carried forward (reported defect not reproduced)

Parent: [WS007](../ws.md)

Tests: [WS007 test index](../tests/README.md)

## Objective

Run a bounded reproduction matrix for the report that amd64 Xzed's internal
cursor coordinates do not follow mouse movement. Repair a confirmed safe local
defect, or carry the report with exact missing conditions.

## Bounded matrix

| Dimension | Executed value | Observation |
| --- | --- | --- |
| Emulator | QEMU 10.0.11, `pc`, 512 MiB, 4 CPUs | Production amd64 image booted and Xzed session launched |
| Display/session | PC graphics device; `startx`; requested 800x600 | zwm/zshell/zterm reached stable desktop |
| Input device | QEMU PS/2 relative mouse, `/dev/mouse` | `info mice` reported `QEMU PS/2 Mouse` |
| Relative forward | five `mouse_move 20 10` events | Cursor moved exactly +100,+50 from its observed center |
| Relative reverse | five `mouse_move -20 -10` events | Cursor returned to its observed center |
| Internal bounds | host test with normal and `INT32_MIN/MAX` deltas | State clamps to `(0,0)` and `(width-1,height-1)` without signed overflow |
| Repetition | two fresh Xzed launches; repeated small deltas | Same tracking result; no divergence reproduced |
| Absolute input | unavailable | Current kernel has no evdev/absolute pointer producer; gated by WS006 IN-01/02/04 |

A single monitor delta outside the PS/2 packet range sets PS/2 overflow bits;
the current driver intentionally discards that hardware packet. Large monitor
deltas were therefore not used as evidence of an Xzed coordinate defect.

## Safe change

The inline coordinate update was extracted to a small shared helper using
64-bit intermediate arithmetic. This removes theoretical signed overflow and
provides a focused host test for delta accumulation and both bounds. The
configured system builds after the change.

## Result and carry-forward reason

The originally reported internal-coordinate mismatch was not reproduced in the
bounded relative-input matrix. Per the Queue rule, this Phase result is
`uncleared`, not `completed`. Absolute input and evdev consumer behavior cannot be tested until
WS006 supplies `/dev/input/eventN` and Xzed migration.

## Resume condition

Resume when one of these exists: a reproducible gesture/device/resolution from
the original environment, an absolute pointer producer, or `ws006-p004` Xzed
evdev migration. Record raw event deltas and queried X coordinates together so
transport loss can be separated from Xzed state.
