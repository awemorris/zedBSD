# Queue proposal: confirmed-commit implementation

Last updated: 2026-09-05

QID: `q073`

Queue status: proposed; awaiting explicit execution approval

Queue finished: **No**

Authorization: the user asked for the next work to be extracted. That
authorizes this planning proposal, not implementation. The item remains
`pending` until the user explicitly approves q073 execution.

Parent: [master plan](master.md)

Previous Queue: [q072](queue-q072.md)

## Purpose

Implement the already frozen interactive confirmed-commit contract as one
bounded software Phase. Keep the candidate and `/etc/net.conf` writer in the
originating `net` process; networkd owns only the volatile deadline, opaque
token, already-open rollback program, and bounded rollback result.

## Proposed execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws011-p006`](ws011-net-config/phase006-confirmed-commit-implementation/phase.md) | pending | Replace interactive `apply`/`save`/`discard` with `commit`, `commit confirmed MINUTES`, and `rollback`; p005 design and q072 documentation dependency are complete |

## Why this is the next bounded unit

- It is first in the active order in the master plan.
- `ws011-p005` froze ownership, grammar, time, size, locking, persistence, and
  acknowledgement bounds; no unresolved design decision remains.
- q072 completed the only higher-priority dependency.
- VLAN/bridge p004 remains outside scope under `MB-010`.
- p007 includes QEMU and user-operated physical acceptance, so it remains a
  separate follow-up after this implementation reaches its focused gates.

## Execution and timebox boundary

- One Queue item and one implementation Phase only.
- Change the private `net`/networkd protocol, interactive transaction owner,
  rollback-program machinery, focused fixtures, build integration, and the
  directly affected public network-console documentation.
- Preserve the existing argv interface, direct `/sbin/ifconfig` recovery,
  wired boot, and completed managed-WLAN behavior.
- Do not implement VLAN/bridge, a persistent daemon candidate, a cross-reboot
  timer, an argv confirmed-commit command, a separate `confirm`, pending-status,
  or timer-extension/reset commands.
- Stop after NCOM-T001--T012, focused existing regressions, supported target
  builds, and `git diff --check`. Do not begin p007 QEMU or physical acceptance
  and do not run aggregate `make check`.

## Approval boundary

Before source changes, present this q073 proposal to the user. On explicit
approval, change the Queue to active, set `ws011-p006` to `in-progress`, and
execute its P-book work packages in order. Without that approval, q073 remains
a proposal and no implementation work is authorized.
