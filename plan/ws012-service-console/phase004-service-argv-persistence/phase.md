# WS012 Phase 004: non-interactive service CLI and persistent policy

Last updated: 2026-08-28

WSID: `ws012`

Phase ID: `p004`

Combined ID: `ws012-p004`

Status: Completed (`q018`, 2026-08-28)

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

## Completion record (`q018`)

- `/sbin/service` now enters a shared context-driven dispatcher used by the
  argv frontend and available to p005. It enforces the fixed grammar and exit
  statuses 0/1/2, validates effective UID 0 before any backend operation, and
  decodes typed ZSV1 results without forwarding server display text.
- LIST and argument-free SHOW share a bytewise-sorted deterministic table.
  SHOW NAME and STATUS NAME share a deterministic detail view containing all
  six lifecycle states, `-` for an absent PID, validated type/command/arguments
  and restart metadata, and sorted direct dependencies.
- START, STOP, RESTART, and RELOAD affect only runtime state. ENABLE and
  DISABLE validate the service definition and PID 1 SHOW record before taking
  the stable rc.conf lock, atomically publish canonical YAML, and then request
  RELOAD. A post-persistence transport, typed-error, or wrong-token failure
  reports that policy changed and runtime policy may be stale; it never rolls
  back over a possible concurrent update.
- Review exposed a response-boundary defect: forged or invalid callback counts
  and unterminated result tokens could reach fixed-array or string operations.
  The dispatcher now validates service/dependency counts and token termination
  immediately after every callback, before copy, sort, or comparison.
- The production-backed service-command fixture passed strict C17, ASan/UBSan,
  exact grammar/output/failure cases, and 20/20 repeated forked two-writer
  runs. The p002 strict-model and persistence fixtures were rerun and passed.
  The service production target and the repository-wide `make -j16` gate
  passed.
- Formatting and `git diff --check` passed. The saved `config.mk` SHA-256
  remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `make check` was not run and `.internal/` was not used.

## Reconsideration boundary

Stop for a new protocol Phase if stable argv output requires data ZSV1 does
not expose. Do not make enable imply start, make disable imply stop, or add a
candidate/save transaction to work around a persistence problem.

## Interruption / resumption

Completed without interruption in q018. Continue with `ws012-p005`, reusing
the verified dispatcher and result formatting behind the bounded interactive
console rather than creating a second command implementation.
