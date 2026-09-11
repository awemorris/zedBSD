# ws002-p013: logger and zedBSD syslogd

WSID: `ws002`

Phase ID: `p013`

Status: complete baseline

Parent WS: [WS002](../ws.md)

## Objective and design

Implement POSIX `logger`, local datagram logging at `/run/log`, foreground
`syslogd`, `/var/log/messages`, and the boot kernel-message snapshot at
`/run/dmesg.boot`. Do not create `/var/log/syslog` or `/var/log/dmesg` in the
initial policy.

## Acceptance and result

Logging startup, messages, kernel snapshot, persistence, and service lifecycle
were included in integrated QEMU acceptance. Shared cases and remaining fault
paths are indexed in [WS002 tests](../tests/README.md).

## Interruption record

Not interrupted. Broader syslog policy and XSI libc interfaces remain separate
ledger work where applicable.

## Completion conditions

- `logger` and foreground `syslogd` exchange messages through `/run/log`.
- `/var/log/messages` and `/run/dmesg.boot` follow the documented persistence policy.
- Startup, malformed message, unavailable output, restart, and QEMU integration cases pass.
