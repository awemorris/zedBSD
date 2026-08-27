# Queue: WS012 service administration completion

Last updated: 2026-08-28

QID: `q018`

Queue status: in-progress

Queue finished: **No**

Authorization: explicitly approved by the user on 2026-08-27 as part of the
automatic WS-priority Queue execution run, including automatic continuation
and the Phase-boundary WIP commit/push policy below

Timebox: continuous execution through 2026-08-28 09:00 JST; stop earlier only
when every Queue item has been processed or no dependency-ready work remains

Parent: [master plan](master.md)

Previous Queue: [q017](queue-q017.md)

## Purpose

Complete the remaining WS012 service-administration stack in dependency order.
Build the bounded ZSV1 PID 1 protocol first, rebuild the script-safe service
CLI on that typed protocol and q017's YAML persistence foundation, add the
argument-free interactive console through the same dispatcher, and finish
with production amd64 QEMU integration and public documentation.

The p003 protocol includes typed service operations and fixed `ZSV1 HALT`,
`ZSV1 POWEROFF`, and `ZSV1 REBOOT` operations. `/sbin/halt`,
`/sbin/poweroff`, `/sbin/reboot`, and `/sbin/shutdown` migrate together; the
unversioned socket grammar is removed without a compatibility path.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws012-p003` | [WS012](ws012-service-console/ws.md), [Phase](ws012-service-console/phase003-zsv1-init-protocol/phase.md), [tests](ws012-service-console/tests/README.md) | completed | PID 1 and every installed service/shutdown client use bounded, terminated ZSV1 service and system-action records with no display-text parsing or unversioned fallback |
| 2 | `ws012-p004` | [WS012](ws012-service-console/ws.md), [Phase](ws012-service-console/phase004-service-argv-persistence/phase.md), [tests](ws012-service-console/tests/README.md) | completed | The argv CLI has fixed grammar/output/exit status, separates runtime control from locked persistent policy, and uses typed ZSV1 state |
| 3 | `ws012-p005` | [WS012](ws012-service-console/ws.md), [Phase](ws012-service-console/phase005-interactive-console/phase.md), [tests](ws012-service-console/tests/README.md) | completed | Argument-free service enters the bounded native console and reuses the p004 dispatcher without candidate/save state |
| 4 | `ws012-p006` | [WS012](ws012-service-console/ws.md), [Phase](ws012-service-console/phase006-integration-acceptance/phase.md), [tests](ws012-service-console/tests/README.md) | in-progress | Focused fixtures, production amd64 QEMU behavior, persistence/failure/concurrency cases, fatal-log scan, and public documentation prove the complete WS012 contract |

## Entry evidence and dependency order

- q017 completed `ws012-p002`: strict YAML parsing, stable locking, atomic
  persistence, all-reader migration, the production build, and a guest
  persistence/reboot proof all pass.
- All p003 product decisions are fixed. `/run/init.sock` remains the root-only
  endpoint, fd 3 remains an independent readiness channel, foreground
  daemon/respawn services remain direct PID 1 children, and ZSV1 carries the
  three system actions.
- p004 depends on p003, p005 depends on p004, and integrated p006 depends on
  p002-p005. An item starts only after every earlier dependency completes.
- No known human product decision remains inside this finite Queue.

## Ordered execution

1. Execute [p003](ws012-service-console/phase003-zsv1-init-protocol/phase.md):
   isolate the bounded parser/emitter and typed client decoder, migrate service
   and shutdown clients atomically, add focused protocol/action fixtures, and
   verify its complete P-book contract.
2. Execute [p004](ws012-service-console/phase004-service-argv-persistence/phase.md):
   implement the shared typed dispatcher, deterministic views, runtime-only
   control, policy-only locked mutations, and explicit failure/exit semantics.
3. Execute [p005](ws012-service-console/phase005-interactive-console/phase.md):
   wrap that dispatcher with the bounded `service>` console, usable help,
   recovery, EOF/exit handling, and concurrent-writer coverage.
4. Execute [p006](ws012-service-console/phase006-integration-acceptance/phase.md):
   rerun every focused group, build with `make -j16`, exercise a disposable
   amd64 image with `qemu-system-x86_64`, repair in-scope integration defects,
   scan fatal diagnostics, and update the public init/service reference.
5. After each Queue item is processed, synchronize its actual result into its
   P book, WS012, the master, the test index, and this Queue before starting a
   dependent item.
6. When all items are completed or honestly uncleared, archive q018 and select
   the next dependency-ready WS according to the approved automatic loop.

## `ws012-p003` result

`ws012-p003` completed on 2026-08-28. The production-shared ZSV1 protocol,
client, server, and shutdown-argv fixtures passed, including ASan/UBSan runs
for the protocol, client, and server. `make -j16` passed. One disposable amd64
QEMU boot verified the root-owned mode-`0600` socket, typed state and lifecycle
operations, synchronous reload, typed error recovery, acknowledged halt, and
a clean fatal-log scan. See
[the q018 p003 QEMU evidence](ws012-service-console/tests/q018-p003-qemu-evidence.md).

The saved `config.mk` SHA-256 remained
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`, and
`git diff --check` passed. `make check` was not run and `.internal/` was not
used. The following p004-p005 items are also complete; q018 remains in progress
with `ws012-p006` as the current item.

## `ws012-p004` result

`ws012-p004` completed on 2026-08-28. The shared context/dispatcher implements
the fixed argv grammar and 0/1/2 statuses, root preflight, deterministic typed
LIST/SHOW for all six states and absent PIDs, static metadata plus sorted direct
dependencies, exact runtime actions, and strictly separate persistent policy.
ENABLE/DISABLE perform definition and SHOW preflight, locked atomic canonical
YAML persistence, then RELOAD; a failure after persistence reports changed /
stale policy and never rolls back over concurrent work.

The production service-command fixture passed strict C17 and ASan/UBSan and
20/20 repeated forked two-writer runs. The p002 model and persistence fixtures
were rerun and passed. Review also found and fixed missing callback-response
count/token bounds before fixed-array/string use. The service production target,
repository-wide `make -j16`, formatting, and `git diff --check` passed. The saved `config.mk` hash
remained `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
`make check` was not run and `.internal/` was not used. The following p005 item
is also complete; q018 remains in progress with `ws012-p006` as the current
item.

## `ws012-p005` result

`ws012-p005` completed on 2026-08-28. Argument-free service now enters the
exact root-only banner/prompt loop around the p004 dispatcher. The reader
accepts 511 bytes, completely consumes and rejects 512 or more, tokenizes at
most 16 space/tab fields without shell interpretation, exposes help/? without
save/commit state, and cleanly handles EOF/exit/quit. Local, dispatcher,
malformed-input, and backend failures recover to a fresh prompt and request;
there is no global candidate or lock outside the existing per-update lock.

The production console fixture passed strict C17, ASan/UBSan, and 20/20
repeated runs; the p004 regression passed. Review found and fixed ignored
help/diagnostic stream-write failures. Repository-wide `make -j16`, formatting,
and `git diff --check` passed. The saved `config.mk` hash remained
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
`make check` was not run and `.internal/` was not used. q018 remains in progress
with `ws012-p006` as the current item.

## Phase checkpoint and push policy

After each processed Phase and its planning-book synchronization:

1. run `git add -A`;
2. run `git commit -m WIP`; and
3. run `git push`.

The user explicitly authorized automatic push. If the execution environment
or approval reviewer rejects a push, record that fact and continue with the
local WIP commit; push rejection alone does not stop this Queue. A failed test,
unfinished Phase, or hidden uncertainty must not be mislabeled as completed to
create a checkpoint.

## Stop, defer, and continuation rules

- Routine defects inside a selected Phase's fixed P-book contract remain in
  scope and are repaired until its verification conditions pass.
- If a Phase reaches its reconsideration boundary or needs a new human product
  decision, record the exact question and evidence in P/W/M, mark that item
  `uncleared`, leave dependent items unexecuted with the dependency reason,
  report the question in the conversation, and continue any independent
  authorized work.
- Do not replace ZSV1 with JSON, binary framing, display-text scraping, an
  unversioned compatibility path, or an unbounded field.
- Do not add candidate/save service policy, container-specific protocol data,
  support for daemonizing/forking services, runlevels, or a general command
  language.
- Use `make -j16`; do not run `make check` or consume `.internal/`. Use only
  disposable image copies for mutating QEMU acceptance and preserve the user's
  saved build configuration.
- Physical-machine validation and unrelated service, network, container, or
  kernel redesign are outside q018.

## Approval boundary

q018 authorizes only `ws012-p003` through `ws012-p006` in the stated dependency
order. Detailed implementation and verification requirements remain in the
linked P books rather than being duplicated here.
