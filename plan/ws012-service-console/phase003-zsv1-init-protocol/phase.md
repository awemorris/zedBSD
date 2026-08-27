# WS012 Phase 003: ZSV1 init service-control protocol

Last updated: 2026-08-27

WSID: `ws012`

Phase ID: `p003`

Combined ID: `ws012-p003`

Status: Planned; Queue-ready after `ws012-p002`

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
- PID 1 processes control connections serially. Each LIST/SHOW response is one
  coherent snapshot taken after pending child reap and policy reload work.
- The client uses a bounded 310-second whole-request deadline, covering the
  existing maximum 300-second readiness wait without an unbounded hang.
- `/run/init.sock` remains mode `0600`. All v1 operations, including LIST and
  SHOW, are root-only; no peer-credential or unprivileged status ABI is added.
- An incompatible future protocol uses a new literal such as `ZSV2`; v1 does
  not add a negotiation handshake or binary framing.

## Work packages

1. Isolate bounded request parsing and record emission from display text.
2. Add LIST and SHOW snapshots containing state, enabled policy, and PID;
   SHOW also emits each direct `after` and `requires` dependency.
3. Convert START, STOP, RESTART, and RELOAD acknowledgements and all error
   paths to terminated ZSV1 records.
4. Add HALT, POWEROFF, and REBOOT dispatch, migrate all four shutdown-control
   command names/options to the typed client, and send the complete response
   before PID 1 starts orderly shutdown.
5. Reject malformed/extra input and ensure every accepted request receives
   exactly one terminal `END`, including action failures.
6. Add a reusable client-side ZSV1 decoder for `/sbin/service` and the
   shutdown-control commands; the decoder
   accepts only known records and never prints raw server text.
7. Add host fixtures for fragmented reads/writes, coalesced records, EOF before
   END, invalid versions/tokens/enums/PIDs, record/count/line bounds, daemon
   exit, timeout, coherent LIST/SHOW snapshots, each system action, and absence
   of state change on malformed requests.

## Completion conditions

- PID 1 emits no English display response on the control socket;
- all request and response bounds are enforced before state mutation or array
  access;
- LIST/SHOW expose the accepted lifecycle, enablement, PID, and direct
  dependency data without client text scraping;
- every success and failure response ends exactly once, and client protocol
  failures return a nonzero result without treating partial data as valid;
- halt, poweroff, reboot, and `shutdown -h/-r` use only ZSV1 and receive an
  acknowledgement before the requested system action begins;
- root-only authorization and bounded timeout behavior are covered by focused
  fixtures;
- all Phase-owned tests and `make -j16` pass, and `git diff --check` passes
  without `make check` or `.internal/` use.

## Reconsideration boundary

Stop for a new design Phase if a request must stream progress or carry an
unbounded field that cannot fit the fixed line protocol. Do not introduce
JSON, a binary serializer, or display-text parsing inside this Phase.

## Interruption / resumption

Not started. After p002 completes, resume by extracting the existing request
handler into bounded parser/emitter functions that can be exercised without
running as PID 1. Migrate shutdown-control clients in the same atomic Phase so
the removed unversioned protocol has no installed consumer.
