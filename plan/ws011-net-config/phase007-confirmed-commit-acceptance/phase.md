# WS011 Phase 007: confirmed-commit acceptance

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p007`

Combined ID: `ws011-p007`

Status: planned; follows p006

Parent: [WS011](../ws.md)

Implementation: [confirmed commit](../phase006-confirmed-commit-implementation/phase.md)

Tests: [WS011 test index](../tests/README.md)

## Objective

Prove the complete confirmed-commit behavior automatically and then with one
consolidated real-hardware remote-administration check.

## Automatic acceptance

- A bounded QEMU client changes an address or route through `commit confirmed
  1`, deliberately loses the management path, and observes the old intent
  restored after timeout while `/etc/net.conf` remains byte-identical.
- A second cell performs `commit confirmed 1` followed by ordinary `commit`
  in the same session, proves the new configuration is published atomically,
  and proves no later rollback occurs.
- Explicit rollback, client death, concurrent-session busy handling, stale
  token rejection, networkd restart semantics, partial rollback continuation,
  and fresh DHCP acquisition pass focused fixtures.
- Existing net.conf parser/persistence/boot tests and wired/WLAN regressions
  pass on the same candidate image.

## Physical acceptance

Use one candidate image and one remote session. Temporarily apply a reversible
network change with `commit confirmed`, verify that loss or deliberate
non-confirmation returns the machine to its prior reachable state, then perform
one separately confirmed change and verify that it persists. This is one
combined check, not an iterative boot or repeatability campaign.

## Completion conditions

Every automatic cell passes, the single real-hardware result is recorded, and
no secret or unrelated interface configuration appears in retained evidence.
VLAN and bridge remain excluded under manual hold `MB-010`.
