# Queue proposal: confirmed-commit automatic acceptance

Last updated: 2026-09-05

QID: `q074`

Queue status: proposed; awaiting explicit execution approval

Queue finished: **No**

Authorization: the user requested that the next work be extracted. This
authorizes the planning proposal only; QEMU execution remains `pending` until
the user explicitly approves q074.

Parent: [master plan](master.md)

Previous Queue: [q073](queue-q073.md)

## Purpose

Accept the q073 confirmed-commit implementation in a deterministic amd64
PC/AT QEMU environment. Prove timeout restoration after loss of the originating
management client and prove same-session confirmation, delayed persistence,
absence of late rollback, and reboot persistence without requiring a physical
network or an unavailable in-base remote-shell daemon.

## Proposed execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws011-p007`](ws011-net-config/phase007-confirmed-commit-acceptance/phase.md) | pending | Add and run the two-cell NCOM-T020/T021 QEMU acceptance plus the q073 focused regression/build boundary; depends on completed ws011-p006 |

## Why this is the next bounded unit

- It is first in the active order in the master plan and directly accepts the
  just-completed q073 implementation.
- P005 fixed the semantics and bounds; p006 implements them and passes all
  host/model/integration gates. The remaining automatic uncertainty is target
  runtime behavior across a real monotonic minute and reboot.
- A dedicated NE2000 QEMU topology and monitor-driven guest console are already
  established repository test patterns, so this work needs a runner and
  disposable image fixture rather than a new product interface.
- Physical remote administration is separated into p008 because the current
  base system has no SSH, Telnet, or rlogin daemon and no safe target management
  transport/topology has been selected. Q074 contains no unresolved physical
  decision.

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
  Keep run images and verbose logs under the ignored WS011 `temp/` tree; retain
  only bounded, secret-free summaries in planning evidence.
- Rerun NCOM-T001--T012, the parser/console/persistence/boot/private-protocol
  gates, managed-WLAN/Wi-Fi focused regressions, maintained amd64/i386
  `net`/networkd builds, documentation link checks, and `git diff --check`.
- Do not run aggregate `make check`, change public grammar or protocol, begin
  VLAN/bridge p004, add a remote-shell service, or perform a physical test.
- If either QEMU cell exposes a production defect, mark p007 `uncleared` and
  extract a corrective Phase instead of silently widening this acceptance
  Queue into implementation.

## Important uncertainty

The automatic runner and its exact guest keystroke/image-staging mechanics do
not exist yet. They are bounded fixture work inside p007. The physical
remote-administration mechanism is a separate unresolved dependency owned by
p008 and cannot block or be claimed by q074.

## Approval boundary

Present this q074 proposal before starting QEMU or adding its runner. On
explicit approval, set q074 and `ws011-p007` to `in-progress` and execute the
P-book packages in order. Without that approval, this Queue remains a proposal.
