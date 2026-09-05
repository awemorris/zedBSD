# WS011 Phase 007: confirmed-commit automatic acceptance

Last updated: 2026-09-05

WSID: `ws011`

Phase ID: `p007`

Combined ID: `ws011-p007`

Status: Ready; proposed as the sole pending item in q074

Parent: [WS011](../ws.md)

Implementation: [confirmed commit](../phase006-confirmed-commit-implementation/phase.md)

Tests: [WS011 test index](../tests/README.md)

Queue proposal: [q074](../../queue.md)

Physical follow-up: [p008](../phase008-confirmed-commit-physical-acceptance/phase.md)

## Objective

Prove the complete confirmed-commit behavior in bounded amd64 PC/AT QEMU cells:
automatic rollback after the originating client is lost, and same-session
confirmation with delayed atomic publication, no late rollback, and persistence
through reboot.

## Baseline

- Q073 completed p006 and NCOM-T001--T012. The host/model/integration evidence
  covers exact grammar, complete reconcile, secure rollback-program ownership,
  monotonic scheduling, explicit rollback, session loss, writer contention,
  stale tokens, restart cleanup, malformed bounds, partial failure, and lost
  disarm acknowledgement.
- Maintained amd64 and i386 PC/AT `net`/networkd target builds and the existing
  parser, console, persistence, boot, ZNV2, managed-WLAN, Wi-Fi command, store,
  and child gates pass at commit `327cf63`.
- The repository has bounded monitor-driven amd64 QEMU test patterns and an ISA
  NE2000 plus restricted user-network baseline, but no WS011 confirmed-commit
  target-runtime runner yet.
- The base system has no SSH, Telnet, or rlogin daemon. A real-hardware remote
  session therefore remains a separate transport/topology decision in p008.

## Scope

- one reusable, secret-free QEMU runner and any test-only config/image fixture
  required to install a synthetic static `/etc/net.conf`;
- one fresh timeout/client-loss cell and one fresh confirm/reboot cell;
- exact guest observations for running address, startup-file digest, command
  result, timer passage, and rebooted state;
- q073 focused regressions, maintained target builds, document links, and
  source/input integrity checks on the final candidate;
- bounded P/W/M/Q/test-index synchronization after execution.

## Non-goals

- physical or remote-shell acceptance, which is p008;
- production source correction, new public commands, protocol changes, or a
  shorter test-only confirmed timeout;
- VLAN/bridge work held by `MB-010`;
- an argv confirmed commit, separate `confirm`, pending-status, timer extension,
  cross-reboot timer, or persistent networkd candidate;
- repeated QEMU, boot, or physical campaigns beyond the two required cells.

## Fixed QEMU topology and safety boundary

- Use `qemu-system-x86_64`, the maintained amd64 PC/AT path, ISA `ne2k_isa` at
  I/O `0x300` IRQ 10, a fixed synthetic MAC, and restricted QEMU user networking
  on `10.0.2.0/24` with gateway `10.0.2.2` and DNS `10.0.2.3`.
- The startup model uses synthetic `ne0` address `10.0.2.15/24`. Candidate
  values remain in the same synthetic lab subnet and contain no site address,
  hostname, credential, or hardware identifier.
- Drive the guest through QEMU monitor keystrokes and observe through the debug
  console. This is an independent observer/control channel, not a claim that
  zedBSD already supplies a remote shell.
- Build a dedicated test-only image or patch only a disposable copy. Never edit
  `config.mk`, the production image, or a previously accepted artifact in place.
- Record source-image/config digests before and after, enforce boot, command,
  one-minute timer, reboot, and whole-cell deadlines, and remove successful run
  images. Verbose disposable evidence belongs below the ignored WS011 `temp/`
  tree.

## Acceptance cells

### NCOM-T020: timeout and client loss

1. Boot the synthetic startup configuration and record the byte digest of
   `/etc/net.conf`, `ne0` address, default route, and resolver state.
2. Enter interactive configuration, change `ne0` to a distinct synthetic
   address, and run `commit confirmed 1`.
3. Observe the temporary running address while the startup-file digest remains
   unchanged, then exit the originating `net` process without ordinary commit.
4. Wait beyond the monotonic one-minute deadline without restarting networkd.
5. Observe the old address/route/resolver intent restored, the original
   `/etc/net.conf` digest unchanged, and ordinary guest networking usable.

### NCOM-T021: confirmation, late-timer absence, and reboot

1. Boot a fresh copy with the same old startup intent and record its digest.
2. In one living interactive session, change `ne0` from `10.0.2.15/24` to a
   second usable synthetic address, run `commit confirmed 1`, verify that the
   startup digest is still old, and then run ordinary `commit`.
3. Observe the new file digest and canonical address, wait beyond the former
   deadline, and prove that no late rollback changes running state or the file.
4. Reboot the same disposable cell and prove `net boot` restores the new
   committed address with the same new file digest and usable networking.

## Ordered work packages

- [ ] NCOM-A01: Freeze the test-only amd64/NE2000 configuration, synthetic
      startup/candidate values, monitor command vocabulary, markers, and all
      controller deadlines without modifying production inputs.
- [ ] NCOM-A02: Add a reusable two-cell runner which records metadata/results,
      drives the real installed `net` and networkd, checks source/config
      integrity, and retains no secret or successful writable image.
- [ ] NCOM-A03: Run NCOM-T020 once from a fresh image and retain bounded evidence
      for pre-arm state, temporary apply, lost client, timer expiry, complete
      rollback, unchanged startup bytes, and connectivity recovery.
- [ ] NCOM-A04: Run NCOM-T021 once from another fresh image and retain bounded
      evidence for delayed publication, matching-session confirmation, no late
      rollback, and reboot persistence.
- [ ] NCOM-A05: Rerun NCOM-T001--T012, existing WS011 and ZNV2 gates, the four
      q073 managed-WLAN/Wi-Fi runners, and maintained amd64/i386
      `net`/networkd target builds. Do not run aggregate `make check`.
- [ ] NCOM-A06: Run both documentation validators and `git diff --check`, then
      synchronize actual commands, hashes, results, and the p008 resume boundary
      into the P/W/M/Q/test books.

## Completion conditions

- Both fresh QEMU cells pass within their fixed deadlines against installed
  production binaries and a real one-minute confirmed timer.
- T020 proves temporary runtime mutation, originating-client loss, automatic
  complete rollback, byte-identical old `/etc/net.conf`, and recovered network
  usability.
- T021 proves unchanged persistence before confirmation, atomic new persistence
  before disarm, no rollback after the former deadline, and the new intent after
  reboot.
- All selected focused regressions and maintained amd64/i386 target builds pass;
  production inputs remain digest-identical and evidence contains no secret or
  unrelated interface configuration.
- Any production defect makes p007 `uncleared` and receives a separately queued
  corrective Phase. P007 does not absorb behavior changes merely to pass.

## Interruption boundary

Stop for review before changing production semantics, adding a remote-shell
service, shortening the public minimum timeout, modifying a production image in
place, beginning p008 physical work, or performing a third QEMU acceptance run.

## Current result and resumption

The p006 dependency and existing focused gates are complete. P007 is fully
extracted as the sole proposed q074 item; no runner has been added and no QEMU
cell has started. Obtain explicit q074 execution approval before NCOM-A01.
