# Queue: confirmed-commit automatic acceptance

Last updated: 2026-09-05

QID: `q074`

Queue status: finished

Queue finished: **Yes**

Authorization: after reviewing the extracted proposal, the user explicitly
approved execution of q074 on 2026-09-05.

Parent: [master plan](master.md)

Previous Queue: [q073](queue-q073.md)

## Purpose

Accept the q073 confirmed-commit implementation in a deterministic amd64
PC/AT QEMU environment. Prove timeout restoration after loss of the originating
management client and prove same-session confirmation, delayed persistence,
absence of late rollback, and reboot persistence without requiring a physical
network or an unavailable in-base remote-shell daemon.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws011-p007`](ws011-net-config/phase007-confirmed-commit-acceptance/phase.md) | uncleared (`q074`) | NCOM-T020 passed; NCOM-T021 stopped at the final-request/publication boundary before DISARM, late-deadline, and reboot evidence |

## Why this was the next bounded unit

- It was first in the active order in the master plan and directly accepted the
  just-completed q073 implementation.
- P005 fixed the semantics and bounds; p006 implemented them and passed all
  host/model/integration gates. The remaining automatic uncertainty was target
  runtime behavior across a real monotonic minute and reboot.
- A dedicated NE2000 QEMU topology and monitor-driven guest console were
  established repository test patterns, so the work needed a runner and
  disposable image fixture rather than a new product interface.
- Physical remote administration was separated into p008 because the current
  base system has no SSH, Telnet, or rlogin daemon and no safe target management
  transport/topology was selected.

## Execution and timebox boundary

- One Queue item and one automatic acceptance Phase only.
- Build or derive one test-only amd64 PC/AT image with NE2000 enabled and a
  synthetic static `10.0.2.0/24` configuration. Run exactly two fresh QEMU
  cells: timeout/client-loss restoration and same-session confirmation/reboot.
- Use restricted QEMU user networking, a fixed synthetic MAC, monitor-driven
  keyboard input, bounded boot/command/cell deadlines, and disposable image
  copies. Preserve `config.mk`, the production image, and every source input by
  before/after digest.
- Retain reusable runner/config fixtures under `plan/ws011-net-config/tests/`.
  Keep verbose logs under the ignored WS011 `temp/` tree and retain only a
  bounded, secret-free summary in planning evidence.
- Rerun NCOM-T001--T012, parser/console/persistence/boot/private-protocol,
  managed-WLAN/Wi-Fi, maintained amd64/i386 target builds, documentation checks,
  and `git diff --check`.
- If either QEMU cell exposes a production defect, mark p007 `uncleared` and
  extract a corrective Phase rather than widening q074 into implementation.

## Approval result

The user explicitly approved execution on 2026-09-05. Q074 and `ws011-p007`
were processed within the boundary above.

## Completion result

- Added a reusable hybrid amd64/PC/AT NE2000 image fixture and bounded
  monitor/debug-console runner using only synthetic values and disposable
  writable copies.
- NCOM-T020 passed once: client loss left the rollback armed, real one-minute
  expiry restored `10.0.2.15/24`, and startup bytes, route, resolver, and
  gateway connectivity remained correct.
- NCOM-T021 ran once and is uncleared. The old startup view remained visible
  before ordinary commit and all ten expected check/reconcile requests were
  admitted, but atomic publication did not return inside 30 seconds. No DISARM,
  late-deadline, or reboot observation followed.
- Existing logs leave final DNS handling/response/client close, subsequent
  publication, and DISARM connection setup unresolved. Admission is logged
  before dispatch; the initial inference of completed reconcile and entry into
  `netconf_save_atomic_locked()` was too strong. No third QEMU cell was run in
  q074. The correction was extracted as `ws011-p009` in q075.
- NCOM-T001--T012, parser/console/persistence/boot/ZNV2, all four selected
  Wi-Fi regressions, maintained amd64/i386 `net`/networkd builds, both
  documentation validators, and `git diff --check` pass. Production config and
  image hashes remain unchanged.
- The bounded environment, hashes, and results are retained in the
  [q074 evidence summary](ws011-net-config/tests/q074-confirmed-commit-qemu-evidence.md).
  Physical p008 work remains unstarted and unauthorized.

Q075 retrospective closure: a separate normal cell reproduced excessive FAT
backing-file traversal during write/flush. The bounded correction and four
post-fix T021 cells complete p007; the original q074 uncleared result remains
historical. See [q075](queue-q075.md) and its corrective evidence.
