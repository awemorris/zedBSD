# WS012 review and test index

Parent: [WS012](../ws.md)

The p001 review cases were accepted on 2026-08-27. `SVC-T001` and `SVC-T002`
were implemented and passed in q017; the remaining groups stay as contracts
for their owning approved Queue Phases.

| Case | Status | Required design result |
| --- | --- | --- |
| `SVC-D001` | Accepted | Every argv and interactive command has one unambiguous state transition |
| `SVC-D002` | Accepted | Runtime operations and persistent enablement cannot be confused |
| `SVC-D003` | Accepted | Mapping-only YAML, immediate enable/disable, stable file locking, atomic rewrite, reload, and concurrent-session outcomes are fixed |
| `SVC-D004` | Accepted | Newline-delimited `ZSV1` records are bounded, versioned, terminated by `END`, and usable without parsing display text |
| `SVC-D005` | Accepted | Socket-backed read and mutation operations are root-only; failure is explicit |
| `SVC-D006` | Accepted | Ordinary services are complete without CPAR and no container design blocks v1 |

## Executable groups

| Group | Owning Phase | Status | Required coverage |
| --- | --- | --- | --- |
| `SVC-T001` | `ws012-p002` | Passed (`q017`) | YAML accepted/rejected grammar, semantic bounds, canonical round-trip, service-definition parser separation, all-reader migration |
| `SVC-T002` | `ws012-p002` | Passed (`q017`) | Stable lock contention, two-writer no-lost-update, temporary/sync/rename failure preserving the old file |
| `SVC-T003` | `ws012-p003` | Planned | ZSV1 request/record grammar, fragmentation, bounds, versions, END framing, coherent state snapshot, HALT/POWEROFF/REBOOT clients, timeout and authorization |
| `SVC-T004` | `ws012-p004` | Planned | Argv grammar, exit status, deterministic list/detail output, runtime-only actions, policy-only actions, persist-success/reload-failure |
| `SVC-T005` | `ws012-p005` | Planned | Prompt/help, shared commands, error recovery, EOF/exit, bounded input, concurrent console/argv writers |
| `SVC-T006` | `ws012-p006` | Planned | Production amd64 QEMU boot, service lifecycle, reload, persistence across reboot, malformed reload preservation, fatal-log scan |

## q017 evidence

- The strict model fixture passed its accepted/rejected grammar, bounds,
  canonical round-trip, partial-publication, model API, and assignment-parser
  separation cases.
- The persistence fixture passed stable-lock contention, two-writer
  no-lost-update, write/fsync/rename failure preservation, cleanup, interrupted
  write, and short-write cases.
- `make -j16` passed.
- On one disposable amd64 QEMU image, `service disable cron` rewrote the
  configuration and a reboot of that same image did not start cron.
- Guest `stat` evidence for `/etc/rc.conf` was `mode=81a4 uid=0 gid=0`, proving
  a root-owned regular file with permissions `0644` after replacement.
- The saved `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
- `git diff --check` passed. `make check` was not run and `.internal/` was not
  used.
