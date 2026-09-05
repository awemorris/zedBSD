# WS011 Phase 006: confirmed-commit implementation

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p006`

Combined ID: `ws011-p006`

Status: planned; Queue-ready after the higher-priority WS009 documentation
follow-up

Parent: [WS011](../ws.md)

Design: [confirmed-commit contract](../phase005-confirmed-commit-design/phase.md)

Tests: [WS011 test index](../tests/README.md)

## Objective

Replace the historical interactive `apply`/`save`/`discard` transaction with
`commit`, `commit confirmed MINUTES`, and `rollback`, while keeping the
candidate exclusively in the originating `net` process and keeping
`/etc/net.conf` entirely outside networkd.

## Implementation contract

- A normal `commit` validates the in-memory candidate, reconciles running
  network state, and atomically writes `/etc/net.conf` after runtime success.
- `commit confirmed MINUTES` serializes the old complete running intent into a
  mode-0600 `mktemp` file under `/tmp`, asks networkd to open, validate, and arm
  it, and only then applies the candidate operations. It does not write a new
  configuration file anywhere.
- Networkd retains the already-open rollback descriptor, deadline, and opaque
  token. It never opens or modifies `/etc/net.conf`.
- The originating `net` process retains the prospective configuration and
  token in memory. A later ordinary `commit` revalidates and reconciles the
  complete running state to that candidate, atomically publishes it, then
  disarms the matching networkd transaction. Failed reconciliation or write
  leaves rollback armed.
- Loss of the originating session makes confirmation impossible. A later
  authorized administrator may request immediate `rollback`, but cannot adopt
  or confirm the lost candidate. Otherwise the timer expires and networkd
  executes the rollback program.
- Timeout and explicit rollback execute every valid rollback line even if one
  operation fails, return bounded per-step diagnostics, and leave the old
  `/etc/net.conf` unchanged.
- A networkd restart or OS reboot forgets the volatile timer. The unchanged old
  `/etc/net.conf` remains the next boot intent.
- The 1--1440-minute, `/run/net.conf.lock`, 64-operation, 4096/32768-byte, and
  bounded acknowledgement-retry rules from p005 are normative.

## Compatibility removal

Remove `apply`, `save`, and `discard` from interactive help and parsing. Do not
add argv/non-interactive confirmed commit, a separate `confirm`, pending-status,
timer-extension, or timer-reset command.

## Completion conditions

- Focused model tests prove normal commit, confirmed arm/apply/confirm,
  explicit rollback, timeout, partial apply, partial rollback, lost client,
  concurrent sessions, stale token, daemon restart, DHCP-intent reacquisition,
  atomic persistence, and acknowledgement loss.
- Malformed, oversized, replaced, truncated, symlinked, or over-count rollback
  programs fail before arming, and the already-open descriptor defeats pathname
  replacement after acceptance.
- `/etc/net.conf` changes only on a successful normal or confirming commit;
  networkd has no configuration-file access path.
- Existing static/DHCP boot, direct `ifconfig`, wired networking, and completed
  WLAN behavior remain passing.

## Interruption boundary

Stop for review before adding a new public protocol family, persistent daemon
candidate, cross-reboot timer, or any networkd write access to `/etc/net.conf`.
