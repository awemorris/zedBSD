# Queue: YAML `/etc/rc.conf` foundation

Last updated: 2026-08-27

QID: `q017`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-27 as part of the
automatic WS-priority Queue execution run

Timebox: continuous execution through 2026-08-28 09:00 JST; this finite Queue
contains one dependency-ready Phase and may finish earlier

Parent: [master plan](master.md)

Previous Queue: [q016](queue-q016.md)

## Purpose

Complete `ws012-p002`, the only currently dependency-ready WS012 Phase. Replace
the legacy `/etc/rc.conf` assignment format with the accepted bounded,
mapping-only YAML v1 model and one locked, atomic persistence API shared by
PID 1, `service`, and `ntpdate`.

The later service-console Phases are not in this finite Queue. The user has
since accepted `/run/init.sock` for ZSV1 service and HALT/POWEROFF/REBOOT
actions without the old protocol or a new signal path. After p002 closes,
p003-p006 are eligible for the next WS012 Queue in dependency order.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws012-p002` | [WS012](ws012-service-console/ws.md), [Phase](ws012-service-console/phase002-yaml-rcconf/phase.md), [tests](ws012-service-console/tests/README.md) | completed | All rc.conf readers use the strict YAML v1 model; stable locking and atomic replacement pass focused failure/concurrency fixtures and the production build |

## Entry evidence and dependencies

- `ws012-p001` accepted the exact YAML shape, strict subset, direct replacement
  policy, stable companion lock, and atomic same-directory rename contract.
- The installed file has only hostname, service enablement, and ntpdate server
  consumers; `/etc/service.d/` remains on its separate assignment grammar.
- Native `fcntl(F_SETLKW)`, `fsync`, ownership/mode operations, and same-mount
  rename exist; q017 guest acceptance proved the required overlay/UFS behavior.
- The p003 shutdown protocol decision is resolved but remains outside this
  Queue's implementation boundary.

## Ordered execution

1. Inventory and separate the existing assignment parser from every rc.conf
   consumer without changing `/etc/service.d/` syntax.
2. Implement bounded YAML parsing, semantic validation, canonical ordering and
   serialization, model lookup/mutation, stable lock acquisition, exclusive
   temporary-file write, fsync, and atomic replacement.
3. Convert PID 1 hostname and enabled-policy snapshots, `service` persistence,
   and `ntpdate` servers to the model; install only canonical YAML rc.conf.
4. Add Phase-owned accepted/rejected grammar, bounds, round-trip, failure-
   injection, lock-contention, and no-lost-update host fixtures.
5. Run focused fixtures, `make -j16`, a bounded QEMU persistence/reload proof
   if required by the Phase, config-preservation verification, and
   `git diff --check`.
6. Synchronize the Phase, WS012, master, and Queue, then archive q017.
7. Group p003-p006 into the next WS012 Queue after p002 closes.

## Stop, defer, and continuation rules

- Parser, persistence, reader migration, fixture, or ordinary build defects
  within the fixed p002 contract remain in scope and are repaired directly.
- If the filesystem cannot provide the accepted stable-lock plus atomic-rename
  contract, record exact evidence, mark p002 uncleared, report the required
  human decision, and continue to the next independent WS.
- Do not broaden p002 with the separately authorized p003 service protocol.
- Do not use `make check`, `.internal/`, a physical test, or a repository
  commit. Preserve unrelated working-tree changes and `config.mk`.

## Approval boundary

This Queue authorizes only `ws012-p002`. The accepted p003-p006 work requires
the next finite Queue record.

## Result

`ws012-p002` completed without a human-decision blocker. The legacy assignment
format was replaced by the strict mapping-only YAML v1 model for rc.conf while
the `/etc/service.d/` assignment reader remained separate. PID 1, `service`,
and `ntpdate` now share the validated model; writers serialize on the stable
companion lock and publish canonical data through same-directory fsync and
atomic rename.

The strict-model and persistence host fixtures passed, including grammar,
bounds, deterministic round-trip, lock contention, two-writer no-lost-update,
and injected write/fsync/rename failures. `make -j16` passed. On one disposable
amd64 QEMU image, `service disable cron` persisted across reboot and cron was
not started afterward. Guest `stat` evidence for `/etc/rc.conf` was
`mode=81a4 uid=0 gid=0`, proving the required root-owned `0644` replacement.

The saved `config.mk` SHA-256 remained
`3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`, and
`git diff --check` passed. No `make check`, `.internal/`, physical test, or
repository commit was used.

The first guest attempt also exposed an unrelated build-dependency defect:
an earlier custom boot-parameter input could leave a newer generated header
authoritative for a later default build. That residual is isolated as
[`ws003-p016`](ws003-bringup/phase016-boot-parameter-header-dependency/phase.md)
and does not weaken the successful p002 rerun with regenerated defaults.
