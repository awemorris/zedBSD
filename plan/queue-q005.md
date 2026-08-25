# Queue: physical-key input broker decision

Last updated: 2026-08-25

QID: `q005`

Queue status: finished

Queue result: partial port follow-ups recorded

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q004](queue-q004.md)

## Execution registry

| Priority | Item | WS / Phase | Sources | Status | Required local result |
| --- | --- | --- | --- | --- | --- |
| 1 | Physical-key input broker and guest evidence | `ws006-p004` | [WS006](ws006-input/ws.md), [Phase](ws006-input/phase004-console-broker/phase.md), [tests](ws006-input/tests/README.md) | completed | PC/AT software milestone: fixed string API, focused/build checks, and production QEMU evdev/console coexistence pass |

## Decision and result

The human decision rejects a public HAL keycode/HID enum. A key event contains
`char symbol[16]` and one press/release/repeat flag. The production API, PC/AT
producers, kernel broker, evdev path, and tty translation implement that form.
Focused tests and all configured x86 builds pass. QEMU reads `KEY_C` plus
`SYN_REPORT` from the production event node while the console receives the same
key. PC-98/X68000 physical-detail completion remains a port follow-up.

## Closure checklist

- [x] Human judgment fixed the event representation.
- [x] Production API and consumers implement it.
- [x] Focused/build/QEMU evidence passes.
- [x] Partial port follow-ups are explicit.
