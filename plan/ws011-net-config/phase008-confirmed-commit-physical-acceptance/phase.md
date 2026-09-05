# WS011 Phase 008: confirmed-commit physical acceptance

Last updated: 2026-09-05

Phase ID: `p008`

Combined ID: `ws011-p008`

Status: planned; not Queue-ready pending the remote-administration transport,
target link, and safe recovery topology

Parent: [WS011](../ws.md)

Automatic prerequisite: [p007](../phase007-confirmed-commit-acceptance/phase.md)

Corrective prerequisite: [p009](../phase009-confirmed-commit-overlay-publication/phase.md)

Tests: [WS011 test index](../tests/README.md)

## Objective

Perform one consolidated real-hardware remote-administration acceptance against
the p007-accepted candidate image. Prove that an unconfirmed, reversible change
which loses the management session restores the prior reachable configuration,
then prove that a separately confirmed safe change persists through reboot.

## Why this is separate

- The current base system supplies no SSH, Telnet, or rlogin daemon. The exact
  remote-administration transport is therefore not an existing WS011 fixture.
- A physical management link, disruptive trial value, reconnect path, and
  out-of-band or local recovery path must be selected for the actual target.
- Those human/environment decisions must not be hidden inside p007's automatic
  QEMU item or answered by adding an unplanned remote-shell service.

## Queue-readiness prerequisites

- P007 passes both automatic QEMU cells on the exact candidate commit/image.
- The user identifies or approves the remote-administration transport and the
  physical target/interface used by the session.
- A local console or other out-of-band recovery route is available and tested
  before the management link is disrupted.
- The old reachable configuration, one reversible disruptive trial, and one
  safe confirmable trial are frozen without recording credentials or private
  site identifiers in the repository.
- The operator accepts the bounded interruption and reboot window.

Until all prerequisites are true, p008 remains planned and must not enter the
Queue.

## NCOM-T022: consolidated physical remote acceptance

1. Boot the p007-accepted candidate image, record its public commit/image hash,
   and verify the known old configuration and recovery route locally.
2. From the selected remote-administration session, make the frozen reversible
   management-link change and run `commit confirmed MINUTES` with the approved
   bounded timeout.
3. Prove that the originating session/path is lost, wait beyond the deadline,
   reconnect through the restored old configuration, and verify that running
   address/route/DNS state returned and `/etc/net.conf` remained unchanged.
4. On the same candidate image, start a new remote session and make the distinct
   safe change which preserves the session long enough to run ordinary
   `commit` before the deadline.
5. Verify the new canonical `/etc/net.conf`, wait beyond the former deadline to
   exclude late rollback, reboot once, and verify the committed state and remote
   reachability. Restore the lab's desired final state if the trial value was
   intentionally temporary.

## Scope and non-goals

- one candidate image, one timeout path, one confirm path, and one reboot;
- operator-supplied, redacted evidence for reachability, file stability or
  persistence, timer passage, and recovery-path availability;
- no repeated campaign, stress matrix, RF qualification, VLAN/bridge work, or
  unrelated hardware acceptance;
- no implementation change, new administration daemon, credential capture, or
  public-network exposure merely to make the observation possible.

## Completion conditions

- NCOM-T022 passes on the p007-accepted candidate and exact selected physical
  topology.
- Loss of the originating remote session is followed by timeout restoration,
  unchanged startup bytes, and successful reconnection through the old intent.
- Same-session confirmation publishes the new intent atomically, no late
  rollback occurs, and one reboot restores the committed state and reachability.
- Evidence identifies the public candidate commit/image while keeping transport
  credentials and private network details out of the repository.

## Interruption boundary

Stop before installing or exposing a remote service, changing firewall or
public-network policy, performing an unbounded disruptive test, or proceeding
without a verified local/out-of-band recovery route.

## Current result and resumption

The physical check is extracted but not Queue-ready. Q074 passed T020 but left
p007 uncleared at target atomic publication in T021; p009 must correct and
accept that path first. Resume only after p007 passes and the user selects the
remote-administration transport, target link, safe trial values, and recovery
route. No physical action is authorized here.
