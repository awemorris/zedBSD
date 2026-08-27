# WS011 Phase 005: confirmed-commit design

Last updated: 2026-08-27

WSID: `ws011`

Phase ID: `p005`

Combined ID: `ws011-p005`

Status: Proposed; public transaction semantics fixed

Parent: [WS011](../ws.md)

Tests: [WS011 test index](../tests/README.md)

## Objective

Resolve the transaction, rollback, persistence, timeout, and command semantics
needed for a Junos/NETCONF-inspired `commit confirmed` facility before any
network mutation code is changed.

## Baseline

WS011 already has startup, running, and process-local candidate views plus the
historical `apply`, `save`, and `discard` commands. `apply` validates before
requests but does not claim automatic rollback after a partial backend
failure. `/etc/net.conf` is the authoritative startup configuration and its
writes are atomic. This Phase replaces that public transaction vocabulary;
the old commands remain historical implementation facts, not the target CLI.

## Scope

- `commit`, `commit confirmed MINUTES`, and `rollback` grammar;
- removal rules for interactive `apply`, `save`, and `discard`;
- complete reconcile behavior and immediate rollback on partial failure;
- networkd-owned monotonic timeout surviving the client/SSH session;
- persistence timing and safe networkd restart/reboot behavior;
- candidate generations, concurrent consoles, and one pending confirmed commit;
- DHCP/dynamic-state rollback meaning and diagnostics;
- decomposition into later implementation and verification Phases.

## Non-goals

- implementing the feature in this Phase;
- claiming Junos CLI compatibility or adding NETCONF transport;
- mixing VLAN/bridge kernel implementation from p004 into this design.

## Proposed starting semantics

```text
net(config)> commit
net(config)> commit confirmed 5
net(config)> rollback
```

Junos confirms a pending confirmed commit with a later ordinary `commit` (or
`commit check`), while IOS/IOS XE uses the separate `configure confirm` command
around its archive/replace model. zedBSD adopts the smaller Junos-style form:
`commit confirmed 5` temporarily applies the candidate, and a later ordinary
`commit` in that same interactive configuration session confirms and persists
it. `rollback` cancels it immediately. Confirmed commit is deliberately absent
from argv/non-interactive mode.

## Fixed decisions

- The initial public transaction commands are only `commit`,
  `commit confirmed MINUTES`, and `rollback`. `apply`, `save`, `discard`, a
  separate `confirm`, and non-interactive confirmed commit are removed.
- With no pending transaction, `commit` validates the process-local candidate,
  reconciles running state, and atomically writes `/etc/net.conf` only after
  the runtime operation succeeds. Failure restores the old running intent and
  leaves `/etc/net.conf` untouched.
- `commit confirmed MINUTES` requires an explicit positive timeout; there is
  no default. It is accepted only inside the interactive configuration mode.
- During a confirmed transaction `/etc/net.conf` remains the old authoritative
  configuration. The later ordinary `commit` atomically writes that session's
  candidate and only after a successful write asks networkd to disarm the
  matching timer. A failed write leaves rollback armed.
- `rollback` with a pending transaction immediately executes its rollback
  program, cancels the timer, leaves `/etc/net.conf` unchanged, and reloads the
  session candidate from that file. With no pending transaction it simply
  abandons uncommitted edits and reloads the candidate from `/etc/net.conf`.
- networkd does not own a persistent candidate database. The candidate and
  its serialization remain owned by the interactive `net` process.
- Before applying a confirmed candidate, `net` generates a complete,
  idempotent command program that re-establishes the previous network intent.
  The program is the same bounded one-command-per-line request language that
  `net` normally sends to networkd. It is created with `mktemp` under `/tmp`,
  mode 0600, and its path plus timeout are sent to networkd.
- networkd opens, bounds-checks, parses, and accepts the rollback program and
  arms the timer before acknowledging that application may begin. Keeping an
  already-open descriptor prevents path replacement after acceptance.
- `net` then sends the candidate operations one at a time. Failure, client
  exit, lost SSH/TTY, or only partial application leaves the timer armed.
- A later ordinary `commit` from the owning interactive session persists the
  candidate, explicitly disarms the matching transaction, and removes its
  rollback artifact. Timeout makes networkd execute the saved program and
  report every failed rollback step. It does not restore `/etc/net.conf`,
  because that file was never changed.
- Only one confirmed transaction is pending at a time. File locking protects
  candidate/configuration writers; a networkd generation/token prevents a
  stale client from confirming a different transaction.
- A second configuration session cannot confirm or replace the first
  session's pending transaction. It receives a clear busy error until timeout,
  rollback, or confirmation completes it.
- A networkd restart or OS reboot intentionally forgets and cancels the
  rollback timer. Pending state is volatile and no crash/reboot persistence is
  claimed. Because `/etc/net.conf` still contains the old configuration, the
  next boot naturally returns to the last committed intent.
- The rollback program describes the desired old full state rather than an
  inverse history of successfully applied commands. It must remove managed
  state absent from the old configuration as well as restore addresses,
  routes, DNS, and DHCP intent.
- DHCP rollback restores the old DHCP intent by performing a new acquisition;
  retaining or recreating the previous lease is not promised.
- No separate pending-status command, timer extension, or timer reset is in
  the initial grammar. The successful `commit confirmed` response reports that
  rollback is armed and states the requested timeout.

## Open implementation bounds

- The maximum accepted explicit timeout in minutes. The minimum is one minute
  and no default exists; the maximum remains to be chosen before code starts.
- The exact stable companion lock-file name shared by all direct
  `/etc/net.conf` writers across atomic rename.
- Existing networkd request length/count bounds must be audited and reused for
  rollback files; no second rollback-only command language is to be invented.
- If one rollback command fails, networkd continues with the remaining lines,
  records each failure, and returns an aggregate failure. The exact diagnostic
  record format belongs to the implementation Phase.
- The bounded retry and “outcome uncertain” diagnostic for a lost networkd
  confirmation reply after `/etc/net.conf` has already been atomically renamed.
  This rare cross-process boundary must not be reported as success without an
  acknowledgement.

## Work packages

- [x] Resolve grammar and startup/running/candidate transitions.
- [x] Resolve full reconcile and partial-failure rollback semantics.
- [x] Resolve timer ownership, daemon/client/reboot failure, and persistence.
- [x] Resolve concurrency and pending ownership.
- [x] Resolve DHCP and other dynamic-state expectations.
- [ ] Freeze the timeout maximum, companion lock path, and bounded diagnostic
      record before extracting implementation.
- [ ] Define design review and later executable acceptance cases.
- [ ] Split later implementation Phases and synchronize WS011/master/WS009.

## Acceptance

`NCOM-D001`--`NCOM-D006` in the [WS011 test index](../tests/README.md) have
explicit answers. No source, build, QEMU, or physical-network result is claimed.

## Actual results and evidence

The public product flow, volatile timeout ownership, configuration-write
timing, and forward rollback-program model are fixed. The remaining
prerequisite is a small implementation-bounds review, followed by extraction
of implementation and verification Phases.

References:

- RFC 6241 candidate and confirmed-commit capabilities:
  <https://datatracker.ietf.org/doc/html/rfc6241>
- Junos `commit confirmed` behavior:
  <https://www.juniper.net/documentation/us/en/software/junos/cli-evo/cli/topics/topic-map/junos-configuration-commit.html>
- Cisco IOS/IOS XE configuration rollback confirmed change:
  <https://www.cisco.com/c/en/us/td/docs/routers/ios-xe/system-management/system-management/m_cm-config-rollback-confirmed-change.html>
- Cisco IOS XR trial `commit confirmed` model:
  <https://www.cisco.com/c/en/us/td/docs/routers/xr12000/software/xr12k_r4-3/getting_started/configuration/guide/gs43xxr12k/gs43xcnov.html>

## Interruption / resumption

Resume with the three open implementation bounds. Do not Queue implementation
until the timeout maximum, common lock path, and diagnostic bounds are frozen.

## Remaining debt and handoff

All networkd/net protocol changes, timeout code, persistence changes, host
fixtures, QEMU SSH-loss simulation, and public documentation remain later
Phases.
