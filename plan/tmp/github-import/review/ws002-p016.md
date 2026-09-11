# ws002-p016: cron, crontab, at, and batch

WSID: `ws002`

Phase ID: `p016`

Status: complete baseline

Parent WS: [WS002](../ws.md)

## Objective and design

Implement one foreground cron service that owns periodic crontabs plus `at`
and `batch` queues, validates ownership and time expressions, runs job command
text through `/bin/sh`, and durably spools output while no mail provider exists.

## Acceptance and result

Periodic and one-shot jobs, credentials, durable queue/output state, recovery,
and installed-system integration reached the WS002 minimum. Shared cases are
indexed in [WS002 tests](../tests/README.md).

## Interruption record

Not interrupted. Full cron/at portability findings remain WS001 items when
they are POSIX-related.

## Completion conditions

- `cron`, `crontab`, `at`, and `batch` install and operate through the foreground service.
- Ownership, schedule parsing, credentials, durable queue/output, and recovery cases pass.
- Periodic and one-shot jobs execute successfully in the installed QEMU system.
