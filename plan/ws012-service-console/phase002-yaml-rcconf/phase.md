# WS012 Phase 002: YAML rc.conf model and persistence foundation

Last updated: 2026-08-27

WSID: `ws012`

Phase ID: `p002`

Combined ID: `ws012-p002`

Status: Complete (`q017`)

Parent: [WS012](../ws.md)

Tests: [WS012 test index](../tests/README.md)

## Objective

Replace the unused `key=value` `/etc/rc.conf` format with the accepted strict
mapping-only YAML subset, and provide one bounded in-memory model plus a
locked, atomic canonical writer for every later service-policy operation.

## Dependencies

- `ws012-p001` fixed the YAML shape and direct-replacement policy.
- The existing assignment parser remains available for `/etc/service.d/NAME`;
  service-definition syntax is not migrated by this Phase.

## Fixed v1 configuration contract

The installed configuration has this shape:

```yaml
version: 1
hostname: zedbsd

services:
  cron:
    enabled: true
  ntpdate:
    enabled: false
    settings:
      servers: ""
```

- Indentation is exactly two spaces per mapping level. Tabs are invalid.
- The document is a single mapping. Sequences, flow syntax, anchors, aliases,
  tags, document markers, multiline values, and duplicate keys are invalid.
- Scalars are `true`, `false`, bounded unsigned decimal integers, quoted
  strings without escape processing, or bounded safe plain strings. A plain
  string uses only ASCII letters, digits, `.`, `_`, `-`, `/`, or `:`; strings
  containing whitespace must be quoted.
- `version` must be the integer `1`. Known top-level keys are `version`,
  `hostname`, and `services`; unknown keys fail the whole parse.
- Each service mapping permits `enabled` and optional `settings` only.
  `enabled` is required and boolean. The initial service-setting registry
  contains `services.ntpdate.settings.servers`; unknown setting keys fail
  validation instead of being silently ignored.
- Service names retain the existing 63-byte alphanumeric/underscore/hyphen
  bound. Strings, line length, nesting depth, service count, and setting count
  receive explicit compile-time bounds. A valid parse either produces the
  complete model or no model; partial configuration is never exposed.
- The canonical writer emits `version`, `hostname`, and `services` in that
  order. Service names and settings are emitted in bytewise lexical order,
  using two-space indentation and quoted strings when plain form is unsafe.

## Persistence and locking contract

- `/etc/rc.conf.lock` is the stable companion lock. Every direct writer opens
  it with mode `0600` and takes an exclusive blocking `fcntl(F_SETLKW)` lock
  before reading the current configuration.
- A writer parses and validates the current complete file while holding the
  lock, changes its private model, writes a same-directory exclusive temporary
  file, flushes and `fsync()`s it, and atomically renames it over `rc.conf`.
- Failure before rename removes the temporary file and leaves the prior file
  byte-for-byte authoritative. The lock remains held through rename and is
  released only after the final result is known.
- Successful canonical output is root-owned mode `0644`. Readers need no lock:
  atomic rename exposes either the complete old file or the complete new file.
- All future WS012 writers use this API; locking the replaceable `rc.conf`
  inode itself is forbidden because it would not serialize writers after a
  rename.

## Work packages

1. Separate the existing service-definition assignment reader from the new
   rc.conf model API so `/etc/service.d/` remains compatible.
2. Implement the bounded YAML lexer/parser, semantic validation, model lookup,
   mutation, canonical serialization, stable locking, and atomic replacement.
3. Convert PID 1 hostname and enabled-policy loading/reload to one validated
   model snapshot. A malformed startup file is diagnosed and exposes no
   partially enabled policy; a malformed reload preserves the last valid
   in-memory policy.
4. Convert `ntpdate` from `ntpdate_servers` to
   `services.ntpdate.settings.servers`.
5. Replace the installed default rc.conf and phase-owned fixtures together;
   do not add a legacy parser, migration command, or dual-format fallback.
6. Add focused host fixtures under `plan/ws012-service-console/tests/` for
   accepted syntax, every rejected construct, bounds, canonical round-trip,
   lock contention, two-writer lost-update prevention, and injected write/
   sync/rename failure.

## Completion conditions

- The installed default and all base-system rc.conf readers use the v1 model;
- `/etc/service.d/` still uses its existing strict assignment grammar;
- accepted input round-trips to one deterministic canonical form;
- malformed, duplicate, unknown, overlong, and partially written input fails
  without publishing a partial model;
- concurrent enablement writers serialize on `/etc/rc.conf.lock` and retain
  both non-conflicting changes;
- all Phase-owned host fixtures pass, `make -j16` passes, and
  `git diff --check` passes without `make check` or `.internal/` use.

## Execution result

q017 completed this Phase on 2026-08-27. The implementation now has one
strict, bounded YAML v1 model; a separately named assignment reader retained
for `/etc/service.d/`; canonical serialization; a stable companion lock; and
same-directory failure-atomic replacement. PID 1, `service`, and `ntpdate`
all use the new model, and the installed default has no legacy fallback.

Verification evidence:

- `SVC-T001` strict model fixture: PASS;
- `SVC-T002` persistence fixture: PASS;
- `make -j16`: PASS;
- one disposable amd64 QEMU image accepted `service disable cron`, and after
  rebooting that same image cron remained disabled and was not started;
- guest `/etc/rc.conf` metadata after replacement was
  `mode=81a4 uid=0 gid=0`, equivalent to a root-owned regular file with mode
  `0644`;
- `config.mk` retained SHA-256
  `3ce199529678bade77d6f37af22bac8292df7b007f3bd70f137766da6333c1c6`;
- `git diff --check`: PASS; and
- neither `make check` nor `.internal/` was used.

The native overlay/UFS persistence proof therefore cleared the Phase
reconsideration boundary. Independent review also corrected two failure-path
details before acceptance: a reader publishes its model only after successful
close, and PID 1 reports reload success only after applying a valid snapshot.

## Reconsideration boundary

Stop and return to planning if the native filesystem cannot provide the
accepted stable-lock plus atomic-rename contract. Do not add a general YAML
library, preserve the legacy rc.conf syntax, or change `/etc/service.d/` to
make the Phase pass.

## Interruption / resumption

Complete. The next dependency-ready WS012 work begins at `ws012-p003`; no p002
implementation work remains.
