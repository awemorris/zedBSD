# Queue: configuration, input producers, and platform prerequisites

Last updated: 2026-08-25

QID: `q003`

Queue status: finished

Queue result: post-closure architecture decision resolved

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q002](queue-q002.md)

## Purpose and status

This Queue selected five dependency-ready resume points after `q002`. It closed
with four completed items and one carried-forward architecture decision.

## Execution registry

| Priority | Item | WS / Phase | Status | Local completion result |
| --- | --- | --- | --- | --- |
| 1 | `/etc/net.conf` persistence and boot migration | `ws011-p003` | completed | Atomic save, authoritative boot, rc.conf removal, exact static/DHCP request tests, and default QEMU boot pass |
| 2 | Existing keyboard/mouse producer bridge | `ws006-p003` | completed | Existing producers publish evdev records; QEMU registers event0/event1 and reaches login with legacy paths retained |
| 3 | ACPI MCFG and dynamic APIC-vector prerequisites | `ws004-p003` | uncleared | At Queue closure the MSI ownership model was undecided; the post-closure decision resolved it |
| 4 | Init/service public guide | `ws009-p003` | completed | Current configuration, readiness, restart, control, limitations, and shutdown behavior are published and link-valid |
| 5 | POSIX `link`/`unlink` bounded proof | `ws001-p013` | completed | Operand, hard-link identity, failure, format, and native build checks pass |

## Post-closure decision

The project selected the unified logical-IRQ API recorded in
[`ws004-p003`](ws004-hardware/phase003-ecam-msi/phase.md). This resolved the
only human-decision blocker and made that Phase eligible for `q004`.

## Closure

- [x] Every item is completed or uncleared.
- [x] Required focused, build, and QEMU evidence is recorded.
- [x] Remaining limitations and resume conditions are explicit.
- [x] Queue finished is **Yes**.
