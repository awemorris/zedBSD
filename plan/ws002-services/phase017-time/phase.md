# ws002-p017: optional boot-time ntpdate

WSID: `ws002`

Phase ID: `p017`

Status: complete optional feature

Parent WS: [WS002](../ws.md)

## Objective and design

Provide a bounded, disabled-by-default `/sbin/ntpdate` oneshot that may set the
clock after networking and before cron. Do not add `ntpd`, `adjtime()`, or
`adjfreq()` to this Phase and do not make boot depend on network time.

## Acceptance and result

Success, timeout/degraded boot, ordering, and QEMU integration are part of the
WS002 baseline test plan. The command is a zedBSD extension rather than a POSIX
completion claim.

## Interruption record

Not interrupted. Continuous time synchronization requires a new workstream or
Phase decision.

## Completion conditions

- `/sbin/ntpdate` performs a bounded one-shot clock update when enabled.
- Success, timeout, invalid response, and degraded-boot behavior pass tests.
- The feature remains disabled by default and boot never requires network time.
