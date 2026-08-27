# WS012 Phase 004: non-interactive service CLI and persistent policy

Last updated: 2026-08-27

WSID: `ws012`

Phase ID: `p004`

Combined ID: `ws012-p004`

Status: Planned; Queue-ready after `ws012-p003`

Parent: [WS012](../ws.md)

Tests: [WS012 test index](../tests/README.md)

## Objective

Rebuild the script-safe `/sbin/service COMMAND [NAME]` path on the YAML and
ZSV1 foundations, with unambiguous runtime actions, immediate persistent
enable/disable, stable output, locking, and exit status.

## Dependencies

- `ws012-p002`: validated rc.conf model and atomic locked writer.
- `ws012-p003`: bounded PID 1 state/control protocol and client decoder.

## Public argv grammar

```text
service list
service show [NAME]
service status NAME
service start NAME
service stop NAME
service restart NAME
service enable NAME
service disable NAME
service reload
```

`list` and argument-free `show` are equivalent concise views. `status NAME`
is the script-compatible alias of `show NAME`. Invalid arity or an unknown
command exits 2; an operational, configuration, protocol, authorization, or
timeout failure exits 1; success exits 0.

## State and policy semantics

- `start`, `stop`, and `restart` change only the current supervised instance.
  They never change boot policy.
- `enable` and `disable` change only
  `services.NAME.enabled` under the rc.conf lock, atomically persist it, and
  then issue ZSV1 RELOAD. They never start or stop the instance.
- If persistence succeeds but PID 1 cannot acknowledge reload, the command
  exits 1 and states that persistent policy changed while runtime policy may
  remain stale. It does not blindly rewrite the old file over a possible
  concurrent update; a later `service reload` or reboot reconciles it.
- There is no candidate, `save`, `commit`, or `discard` state.
- All socket-backed commands and the configuration console require effective
  UID 0, matching the mode-0600 init socket. Authorization failures are
  diagnosed before attempted mutation.

## Display contract

The concise view is sorted bytewise by service name and contains exactly:

```text
NAME        STATUS    ENABLED   PID
networkd    running   yes       184
ntpdate     stopped   no        -
```

`show NAME`/`status NAME` displays that service's runtime state, enabled
policy, PID, type, command, restart policy, and its direct `after` and
`requires` lists. Runtime values come only from ZSV1; static metadata may be
read from the same validated `/etc/service.d/NAME` record. No transitive
dependency closure or container-specific column is added.

## Work packages

1. Share one command dispatcher and typed result model between argv and the
   later interactive frontend.
2. Implement LIST/SHOW decoding, deterministic table/detail formatting, and
   strict failure handling without forwarding raw PID 1 text.
3. Implement runtime control and reload through ZSV1.
4. Replace legacy assignment editing with locked model mutation and canonical
   YAML persistence; validate that NAME exists before writing policy.
5. Make concurrent enable/disable operations preserve unrelated changes and
   make the persist-success/reload-failure boundary explicit.
6. Add host tests for grammar/exit codes/output, each lifecycle enum, absent
   PID, unknown service, permissions, protocol failure, two-writer races,
   atomic-write injection, and init-unavailable after persistence.

## Completion conditions

- every argv form has the stated arity, state transition, exit status, and
  deterministic output;
- runtime actions do not mutate rc.conf and enable/disable do not mutate the
  current process lifecycle;
- persistent changes use `/etc/rc.conf.lock`, preserve unrelated settings,
  publish one canonical valid file, and request PID 1 reload;
- concise and detailed views use typed ZSV1 data and show all accepted states,
  enablement, PID, and direct dependencies;
- focused tests and `make -j16` pass, and `git diff --check` passes without
  `make check` or `.internal/` use.

## Reconsideration boundary

Stop for a new protocol Phase if stable argv output requires data ZSV1 does
not expose. Do not make enable imply start, make disable imply stop, or add a
candidate/save transaction to work around a persistence problem.

## Interruption / resumption

Not started. After p003 completes, resume by defining the shared typed
command-result structure and golden output before replacing the current
`send_request()` passthrough.
