# WS011 Phase 006: confirmed-commit implementation

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p006`

Combined ID: `ws011-p006`

Status: Ready; proposed as the sole pending item in q073

Parent: [WS011](../ws.md)

Design: [confirmed-commit contract](../phase005-confirmed-commit-design/phase.md)

Tests: [WS011 test index](../tests/README.md)

Queue proposal: [q073](../../queue.md)

## Objective

Replace the historical interactive `apply`/`save`/`discard` transaction with
`commit`, `commit confirmed MINUTES`, and `rollback`, while keeping the
candidate exclusively in the originating `net` process and keeping
`/etc/net.conf` entirely outside networkd.

## Baseline

- Interactive `net` loads `/etc/net.conf` into process-local `startup` and
  `candidate` structures. Its current `apply` sends a sequence of ordinary
  networkd requests, `save` independently calls `netconf_save_atomic()`, and
  `discard` copies startup back to candidate.
- `netconf_save_atomic()` validates, writes, syncs, and renames a same-directory
  temporary file, but the p005 `/run/net.conf.lock` writer serialization is not
  implemented yet.
- Private protocol `ZNV2` currently supports one bounded request and one
  correlated response per connection. It has no arm, disarm, token, rollback,
  or transaction-result fields.
- Networkd is a single-process poll loop with a monotonic WLAN deadline and
  root-only wired mutations. It currently owns no confirmed transaction and
  has no `/etc/net.conf` access.

## Scope

- process-local candidate/token ownership and the three final interactive
  commands in `/sbin/net`;
- complete old-intent rollback-program generation and complete candidate
  reconcile, including removal of managed state absent from the target;
- private ZNV2 arm, matching-token disarm, and explicit rollback operations;
- secure rollback-program open/validation, one volatile networkd transaction,
  monotonic timeout integration, all-line execution, and bounded diagnostics;
- serialized atomic `/etc/net.conf` publication and uncertain-outcome handling;
- NCOM-T001--T012 focused fixtures, existing networking regressions, supported
  target builds, and directly affected public documentation.

## Non-goals

- p007 QEMU or physical remote-administration acceptance;
- VLAN/bridge work held by `MB-010`;
- a public protocol, persistent daemon candidate, cross-reboot timer, or
  networkd access to `/etc/net.conf`;
- argv confirmed commit, separate `confirm`, pending-status, timer extension,
  or timer reset.

## Dependencies

- p005 is the normative design and bounds source.
- q072 completed the higher-priority documentation dependency.
- p001--p003 supply the netconf model, interactive console, atomic writer, boot
  path, and existing fixtures. WS005/q071 supplies the managed-WLAN regression
  boundary that this Phase must preserve.

## Expected implementation and evidence paths

- `userland/base/net/main.c`, `netconf.c`, and `netconf.h` for console state,
  reconcile/rollback generation, writer locking, persistence, and diagnostics;
- `userland/base/net/protocol.h` and `protocol.c` for the private bounded wire
  additions shared by `net` and networkd;
- `userland/base/networkd/main.c` plus private helper files if separation is
  needed for transaction state, secure program parsing, timer wakeups, and
  rollback execution;
- `userland/base/net/Makefile` and `userland/base/networkd/Makefile` only when a
  private helper is split out;
- `plan/ws011-net-config/tests/` for NCOM focused fixtures and runners;
- the WS011 P/W/M/Q books, test index, and directly affected reference/how-to
  documentation for final evidence synchronization.

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

## Ordered work packages

- [ ] NCOM-I01: Extract a deterministic complete-intent reconcile and canonical
      rollback-program builder from the current forward-only `apply_candidate()`
      path; cover static, DHCP, routes, DNS, enable/disable, and absent-state
      retirement without adding VLAN/bridge semantics.
- [ ] NCOM-I02: Extend ZNV2 with exact bounded arm, disarm, and explicit rollback
      request/response fields. Keep wired mutations root-only, reject duplicate
      or unknown fields, and preserve all existing opcodes and framing.
- [ ] NCOM-I03: Add networkd's single volatile transaction owner: secure
      pre-arm file open and validation, opaque generation token, monotonic poll
      deadline, already-open descriptor execution, all-step diagnostics, busy
      and stale-token handling, restart cleanup, and no `net.conf` path.
- [ ] NCOM-I04: Replace interactive `apply`/`save`/`discard` parsing and help with
      `commit`, `commit confirmed MINUTES`, and `rollback`. Retain the confirmed
      candidate/token only in the originating process and preserve safe exit or
      lost-session behavior.
- [ ] NCOM-I05: Add the permanent root-mode-0600 `/run/net.conf.lock` writer
      boundary, runtime-before-persistence ordering, post-rename disarm retries,
      and explicit nonzero `outcome uncertain` result when acknowledgement is
      not obtained.
- [ ] NCOM-I06: Implement and pass NCOM-T001--T012, then rerun the existing
      parser, console, persistence, boot, private protocol, wired, and WLAN
      focused gates plus supported `net`/networkd target builds.
- [ ] NCOM-I07: Update the public network-console documentation and synchronize
      actual commands, evidence, remaining p007 work, and resume state into the
      P/W/M/Q books. Run the Markdown link check and `git diff --check`.

## Acceptance-case allocation

- NCOM-T001 covers final grammar/help and rejects every removed or excluded
  command form.
- NCOM-T002 covers normal commit validation, complete reconcile, rollback after
  partial application, and runtime-before-atomic-persistence ordering.
- NCOM-T003 covers successful pre-arm validation, the mode-0600 `/tmp` file,
  already-open descriptor ownership, and path replacement after acceptance.
- NCOM-T004 covers temporary application, same-session token ownership, later
  ordinary commit, atomic publication, disarm ordering, and bounded retries.
- NCOM-T005--T007 cover timeout, explicit rollback, client/TTY loss, unchanged
  startup bytes, and candidate reload behavior.
- NCOM-T008--T009 cover one-pending ownership, concurrent writer locking, stale
  tokens, daemon restart, and reboot-intent semantics.
- NCOM-T010 covers malformed, truncated, symlinked, over-count, per-line, and
  total-size rollback programs before arming.
- NCOM-T011 covers all-step partial rollback reporting and fresh DHCP-intent
  reacquisition rather than lease restoration.
- NCOM-T012 covers acknowledgement loss, existing parser/console/persistence/
  boot behavior, direct-ifconfig recovery, wired requests, and managed WLAN.

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

## Current result and resumption

Planning extraction is complete; no source or test implementation has started.
After explicit q073 approval, first mark the Queue and this Phase in progress,
rerun the existing WS011 focused baseline, then begin NCOM-I01. P007 remains the
sole acceptance handoff after every p006 completion condition has evidence.
