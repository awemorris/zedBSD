# WS012 Phase 006: service-console integration acceptance

Last updated: 2026-08-27

WSID: `ws012`

Phase ID: `p006`

Combined ID: `ws012-p006`

Status: Planned; Queue-ready after `ws012-p002` through `ws012-p005`

Parent: [WS012](../ws.md)

Tests: [WS012 test index](../tests/README.md)

## Objective

Verify the YAML policy, ZSV1 protocol, argv interface, interactive console,
PID 1 reload, service lifecycle, concurrency, persistence, and failure
contracts together in the production image, repair in-scope integration
defects, and publish the resulting administration contract.

## Dependencies

- `ws012-p002` through `ws012-p005` are complete.
- WS002's native init/service lifecycle remains the supervised runtime under
  test; WS013 container services are not required.

## Work packages

1. Promote reusable parser, protocol, writer-failure, and concurrent-writer
   fixtures into `plan/ws012-service-console/tests/`; no `.internal/` test is
   consumed.
2. Build the supported production configuration with `make -j16` and use a
   disposable copy of the amd64 disk image for mutating acceptance.
3. Boot with `qemu-system-x86_64` and verify valid YAML boot, configured
   hostname, enabled dependency-ordered startup, disabled policy, all
   lifecycle states, LIST/SHOW detail, and absence of display-text scraping.
4. Verify separately that start/stop/restart affect runtime only and that
   enable/disable persist policy only, reload PID 1, and survive one ZSV1
   reboot
   without implicitly changing runtime state in the original session.
5. Exercise the argument-free console, help, error recovery, every public
   command, and clean EOF/exit on the guest console.
6. Exercise two concurrent persistent writers, malformed reload preserving
   the last valid policy, init unavailability after a successful write,
   protocol truncation/version/bounds failures, and atomic-write failure with
   the prior file intact.
7. Update `docs/reference/init-services.md` and the WS009 handoff to describe
   only the verified v1 YAML, ZSV1, argv, console, authorization, locking, and
   failure behavior. Remove obsolete `key=value` rc.conf claims.
8. Synchronize actual evidence and residuals into this Phase and the WS/M/Q
   books. Do not broaden the Queue to fix unrelated lifecycle or container
   issues.

## Required evidence

- A phase-local results table naming every host and QEMU case, exact command,
  outcome, and evidence path;
- production `make -j16` success and `git diff --check` success;
- QEMU console evidence for boot, table/detail status, runtime-only actions,
  policy-only actions, interactive recovery, ZSV1 halt/poweroff/reboot command
  acknowledgement, and reboot persistence;
- canonical rc.conf before/after evidence and lock/atomic-failure fixtures;
- a fatal-log scan covering panic, assertion, protocol corruption, malformed
  config acceptance, and unexpected service failure.

## Completion conditions

- all p002-p005 completion contracts pass together in the production amd64
  image and their focused host fixtures remain green;
- `/etc/rc.conf` has one valid authoritative YAML representation before and
  after concurrent/persistent operations;
- PID 1 and `/sbin/service` agree on runtime state, persistent policy, and PID
  across reload and reboot;
- no command confuses runtime actions with persistent enablement and no
  interactive-only implementation path remains;
- public documentation matches observed behavior and all discovered in-scope
  defects are repaired;
- any unrelated finding is recorded as a new Phase rather than hidden or
  silently expanded into this one.

## Reconsideration boundary

Mark this Phase `uncleared` with evidence if the production filesystem cannot
meet the accepted lock/rename durability model, PID 1 cannot expose a coherent
bounded snapshot, or a lifecycle defect requires redesign outside WS012. Do
not weaken validation or skip the failure/concurrency cases to claim success.

## Interruption / resumption

Not started. Resume only after p002-p005 are complete, beginning with a fresh
test inventory and disposable production image copy. Exercise only ZSV1, not
the removed unversioned socket grammar.
