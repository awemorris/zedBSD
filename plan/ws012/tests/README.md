# WS012 review and test index

Parent: [WS012](../ws.md)

The p001 review cases were accepted on 2026-08-27. `SVC-T001` and `SVC-T002`
passed in q017, and `SVC-T003` through `SVC-T006` passed in q018. The contracts
and evidence below remain the regression baseline for future changes.

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
| `SVC-T003` | `ws012-p003` | Passed (`q018`, 2026-08-28) | ZSV1 request/record grammar, partial/fragmented I/O, client `SHUT_WR` and server EOF-before-dispatch gating, bounded stalled-input rejection, bounds/versions/END framing, `MSG_NOSIGNAL` disconnect safety, one 310-second whole-request deadline, synchronous RELOAD results, coherent LIST/SHOW snapshots, dependency token/count validation, root-only authorization, and HALT/POWEROFF/REBOOT clients whose actions occur only after complete `OK`+`END` transmission; v1 POWEROFF-to-HALT backend mapping is explicit |
| `SVC-T004` | `ws012-p004` | Passed (`q018`, 2026-08-28) | Argv grammar, exit status, deterministic list/detail output, runtime-only actions, policy-only actions, persist-success/reload-failure |
| `SVC-T005` | `ws012-p005` | Passed (`q018`, 2026-08-28) | Prompt/help, shared commands, error recovery, EOF/exit, bounded input, concurrent console/argv writers |
| `SVC-T006` | `ws012-p006` | Passed (`q018`, 2026-08-28) | Production amd64 QEMU boot, service lifecycle, reload, persistence across reboot, malformed reload preservation, fatal-log scan |

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

## q018 p003 evidence

- The production-shared protocol, client, server, and shutdown-argv fixtures
  passed; ASan/UBSan also passed for the protocol, client, and server fixtures.
- `make -j16` passed. A disposable amd64 QEMU boot verified a root-owned
  mode-`0600` `/run/init.sock`, typed list/show state, lifecycle operations,
  synchronous reload, typed error recovery, and acknowledged halt. The guest
  fatal-log scan was clean; details are in
  [the q018 p003 QEMU record](q018-p003-qemu-evidence.md).
- The saved `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `git diff --check` passed. `make check` was not run and `.internal/` was not
  used.

## q018 p004 evidence

- The production dispatcher fixture passed strict C17 and ASan/UBSan runs. It
  covered grammar and exit 0/1/2, root preflight, all six sorted lifecycle
  states, absent PID, list/show and show/status aliases, metadata/arguments,
  sorted dependencies, exact runtime tokens, typed/transport/wrong-token
  failures, and runtime-versus-policy separation.
- Real rc.conf and assignment APIs proved definition-plus-SHOW preflight,
  canonical atomic enable/disable persistence, unrelated-field preservation,
  RELOAD-only policy reconciliation, no write for missing/invalid definitions,
  and changed/stale diagnostics without rollback after three reload-failure
  classes. Forked two-writer acceptance passed 20/20 repeated runs.
- Response count/token bounds are now checked at the dispatcher callback
  boundary before fixed-array and string operations. The p002 model and
  persistence fixtures were rerun and passed.
- The service production target, repository-wide `make -j16`, formatting, and
  `git diff --check` passed. The saved `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `make check` was not run and `.internal/` was not used.

## q018 p005 evidence

- The production console fixture passed strict C17 and ASan/UBSan plus 20/20
  repeated runs. It verified the exact banner/prompt/help; root-before-banner;
  EOF/exit/quit; blank space/tab input; 511-byte acceptance and complete
  consumption/rejection at 512 bytes and above; the 16-field limit; no shell
  quoting; and recovery after local, dispatcher, malformed, control-character,
  and backend failures.
- One and repeated sessions exercised every public operation through a fresh
  p004 backend request, including immediate policy persistence. The same
  dispatcher and per-update stable lock preserve the p004 console/argv
  concurrency result without adding candidate or console-global state.
- Review found and fixed unchecked help/diagnostic stream writes. The p004
  dispatcher regression, repository-wide `make -j16`, formatting, and
  `git diff --check` passed. The saved `config.mk` SHA-256 remained
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`.
  `make check` was not run and `.internal/` was not used.

## q018 p006 evidence

- The consolidated host runner passed the model, persistence, protocol,
  client, server, shutdown-argv, command-dispatcher, and console fixtures under
  both strict C17 and ASan/UBSan. The exact 16-cell matrix is in
  [q018-p006-results.tsv](q018-p006-results.tsv).
- The production QEMU runner passed argv/list/show, runtime-only operations,
  the complete interactive command set and recovery, policy-only operations,
  malformed reload preservation, reboot persistence, explicit start, and the
  ZSV1 reboot/halt/poweroff final-action boundaries. Guest and QEMU/HMP fatal
  scans were empty. Details are in the
  [q018 p006 integration record](q018-p006-qemu-evidence.md).
- Integration fixed deterministic mode `0644` for registered image data and a
  `/bin/sh` foreground handoff race which previously stopped argument-free
  `service` with `SIGTTIN`. Foreground-pipeline and `fg` ordering are recorded
  separately in `ws001-p014`.
- `make -j16`, formatting, DOC-T00, and `git diff --check` passed. The original
  image and saved `config.mk` hashes remained unchanged. `make check` was not
  run and `.internal/` was not used.
