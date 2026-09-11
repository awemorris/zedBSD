# WS012 Phase 003: ZSV1 init service-control protocol

Last updated: 2026-08-28

WSID: `ws012`

Phase ID: `p003`

Combined ID: `ws012-p003`

Status: Completed (`q018`, 2026-08-28)

Parent: [WS012](../ws.md)

Tests: [WS012 test index](../tests/README.md)

## Objective

Replace human-readable PID 1 socket responses with the accepted bounded
`ZSV1` service-control protocol, making PID 1 the machine-readable authority
for runtime lifecycle state, persistent enablement, PID, and direct
dependencies.

## Dependencies

- `ws012-p002` supplies the validated enabled-policy snapshot and reload
  behavior.
- The existing service table and lifecycle enums remain the implementation
  baseline. Foreground managed services remain direct PID 1 children.
- `/run/init.sock` is the accepted root-only service-control endpoint.
- fd 3 remains an independent service-to-init readiness path and is not used
  by the control protocol.

## Recorded transport and compatibility decision

The current tree also accepts unversioned halt, poweroff, and reboot requests
on `/run/init.sock`. On 2026-08-27 the user accepted the socket for service
and system-action management but rejected compatibility preservation:

- ZSV1 fully replaces the unversioned grammar;
- `ZSV1 HALT`, `ZSV1 POWEROFF`, and `ZSV1 REBOOT` are accepted fixed commands;
- `/sbin/halt`, `/sbin/poweroff`, `/sbin/reboot`, and `/sbin/shutdown` migrate
  together to the bounded ZSV1 client and require a terminated response; and
- no new signal-based shutdown-control path is implemented in this Phase.

The initial service supervisor continues to require foreground direct-child
processes for daemon/respawn types. Supporting daemonizing/forking services is
not required for p003-p006.

The current system-device UAPI has only `ZEDBSD_SYSTEM_HALT` and
`ZEDBSD_SYSTEM_REBOOT`. ZSV1 still preserves `HALT`, `POWEROFF`, and `REBOOT`
as three distinct requests, but the v1 `POWEROFF` request deliberately maps to
the existing `ZEDBSD_SYSTEM_HALT` backend, as the unversioned command does
today. Physical power-off is not guaranteed by p003. Adding a distinct kernel
and platform power-off operation is a candidate for a later Phase and does not
block the ZSV1 migration.

## Protocol contract

One connection carries exactly one newline-terminated request and one response
ending in `ZSV1 END`:

```text
ZSV1 LIST
ZSV1 SHOW sshd
ZSV1 START sshd
ZSV1 STOP sshd
ZSV1 RESTART sshd
ZSV1 RELOAD
ZSV1 HALT
ZSV1 POWEROFF
ZSV1 REBOOT
```

Any unversioned request is rejected as an unknown version without changing
system state.

Successful response records are:

```text
ZSV1 SERVICE sshd running 1 231
ZSV1 AFTER networkd
ZSV1 REQUIRES networkd
ZSV1 OK stopped
ZSV1 END
```

Failures are machine-readable and still terminated:

```text
ZSV1 ERROR 2 unknown-service
ZSV1 END
```

- The decimal error field is a nonzero errno-style value and the reason is a
  bounded lowercase hyphenated token, not display prose.
- A request is at most 256 bytes including newline. A response line is at most
  512 bytes, the number of `SERVICE` records is bounded by `SERVICE_MAX`, and
  direct dependency records are bounded by the validated service definition.
- Service names, lifecycle enums, booleans (`0`/`1`), decimal PIDs, and reason
  tokens contain no whitespace and require no quoting. Unknown versions,
  commands, records, states, missing `END`, extra fields, overlong lines, and
  trailing requests are protocol errors.
- A conforming client sends the complete request and then calls
  `shutdown(fd, SHUT_WR)`. PID 1 reads through EOF and validates that the input
  contains exactly one complete request before dispatching it. It does not
  mutate state merely because the first newline has arrived. This half-close
  contract makes delayed trailing input rejectable without a timing race.
- PID 1 applies a bounded receive deadline while collecting the request. A
  client that omits the half-close, stalls mid-record, or exceeds the request
  bound is closed without dispatch or state mutation.
- PID 1 processes control connections serially. Each LIST/SHOW response is one
  coherent snapshot taken after pending child reap and policy reload work.
- The client uses one monotonic 310-second whole-request deadline across all
  connect, partial-send, and fragmented-response work, covering the existing
  maximum 300-second readiness wait without an unbounded hang. Individual I/O
  calls do not restart that deadline.
- Request and response writers handle partial writes and `EINTR` and suppress
  `SIGPIPE`, using `send(..., MSG_NOSIGNAL)` where available. A peer disconnect
  is an ordinary protocol/transport failure and must never terminate PID 1.
- RELOAD is executed synchronously against the validated policy snapshot. Its
  `OK` or `ERROR` record reports that attempt's actual result rather than only
  acknowledging a deferred reload flag.
- HALT, POWEROFF, and REBOOT are scheduled only after every byte of their
  `OK` and single terminal `END` records has been sent successfully. A partial
  response or disconnected client does not schedule the system action.
- `/run/init.sock` remains mode `0600`. All v1 operations, including LIST and
  SHOW, are root-only; no peer-credential or unprivileged status ABI is added.
- An incompatible future protocol uses a new literal such as `ZSV2`; v1 does
  not add a negotiation handshake or binary framing.

## Work packages

1. Isolate bounded request parsing and record emission from display text,
   including incremental I/O helpers whose production implementation is used
   directly by the host fixtures.
2. Add LIST and SHOW snapshots containing state, enabled policy, and PID;
   SHOW also emits each direct `after` and `requires` dependency. Validate each
   dependency token as a service name and enforce an explicit count bound
   before registering or emitting the definition; empty, malformed, or excess
   dependency tokens make the definition invalid.
3. Convert START, STOP, RESTART, and RELOAD acknowledgements and all error
   paths to terminated ZSV1 records. Perform RELOAD synchronously so its reply
   represents the actual reload result.
4. Add HALT, POWEROFF, and REBOOT dispatch, migrate all four shutdown-control
   command names/options to the typed client, retain the accepted v1
   POWEROFF-to-HALT backend mapping, and schedule an action only after the
   complete `OK` plus `END` response has been sent.
5. Make every client half-close its request direction, make PID 1 read through
   EOF under a bounded receive deadline, and reject malformed, incomplete,
   overlong, or extra input before dispatch. Ensure every accepted request
   receives exactly one terminal `END`, including action failures.
6. Add a reusable client-side ZSV1 decoder for `/sbin/service` and the
   shutdown-control commands; the decoder accepts only known records, never
   prints raw server text, enforces one monotonic 310-second whole-request
   deadline, and treats partial writes, disconnects, and missing `END` as
   failures without `SIGPIPE` termination.
7. Add host fixtures for fragmented reads/writes, client half-close and server
   EOF gating, coalesced records, EOF before END, invalid
   versions/tokens/enums/PIDs, record/count/line bounds, stalled server input,
   daemon exit, the whole-request deadline, coherent LIST/SHOW snapshots,
   dependency validation, each system action and backend mapping, and absence
   of state change on malformed input or failed action acknowledgement.

## Completion conditions

- PID 1 emits no English display response on the control socket;
- all request and response bounds are enforced before state mutation or array
  access;
- LIST/SHOW expose the accepted lifecycle, enablement, PID, and direct
  dependency data without client text scraping, and malformed or excess
  dependency records cannot enter or escape the service model;
- every success and failure response ends exactly once, and client protocol
  failures return a nonzero result without treating partial data as valid;
- PID 1 dispatches only after a bounded read reaches the client's `SHUT_WR`
  EOF, a stalled or trailing request causes no mutation, and broken peers
  cannot deliver `SIGPIPE` to PID 1;
- RELOAD reports its synchronous result, and each system action is scheduled
  only after successful complete transmission of `OK` and `END`;
- halt, poweroff, reboot, and `shutdown -h/-r` use only ZSV1 and receive an
  acknowledgement before the requested system action begins; POWEROFF uses
  the documented v1 HALT backend without claiming physical power-off;
- the client enforces one 310-second whole-request deadline across fragmented
  I/O rather than restarting a timeout for each read or write;
- root-only authorization and bounded timeout behavior are covered by focused
  fixtures;
- all Phase-owned tests and `make -j16` pass, and `git diff --check` passes
  without `make check` or `.internal/` use.

## Completion record (`q018`)

- PID 1 and the installed service and shutdown commands now share the bounded
  ZSV1 parser, emitter, server, and client implementation. LIST, SHOW,
  START, STOP, RESTART, synchronous RELOAD, HALT, POWEROFF, and REBOOT use
  terminated typed records; the unversioned fallback was removed.
- The production-backed protocol, client, server, and shutdown-argv fixtures
  passed their framing, partial-I/O, EOF-gating, deadline, disconnect,
  dependency-bound, action-acknowledgement, and error cases. ASan/UBSan runs
  of the protocol, client, and server fixtures also passed.
- `make -j16` passed. One disposable amd64 QEMU image reached PID 1 and a root
  shell; `/run/init.sock` was root-owned mode `0600`; typed list/show,
  stop/start/restart, synchronous reload, unknown-service recovery, and the
  acknowledged `/sbin/halt` path behaved as specified. The fatal-log scan was
  clean. See [the q018 production QEMU evidence](../tests/q018-p003-qemu-evidence.md).
- The saved `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `git diff --check` passed; `make check` was not run and `.internal/` was not
  used.

## Reconsideration boundary

Stop for a new design Phase if a request must stream progress or carry an
unbounded field that cannot fit the fixed line protocol. Do not introduce
JSON, a binary serializer, or display-text parsing inside this Phase.

## Interruption / resumption

Completed without interruption in q018. Continue with `ws012-p004`, which may
now build the stable argv CLI and persistent-policy operations on the verified
typed protocol.
