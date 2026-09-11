# ws002-p012: native init and service lifecycle

WSID: `ws002`

Phase ID: `p012`

Status: complete

Parent WS: [WS002](../ws.md)

## Objective and design

Implement the single native `/sbin/init`, dependency-ordered service startup,
reverse stop order, supervision, `/run/init.sock`, `/sbin/service`, required
`mount -a`, and orderly halt/poweroff/reboot. Runlevels and shell-executed
service definitions are intentionally absent.

## Acceptance and result

The installed QEMU system boots under PID 1, operates services, supervises
children, and shuts down through the native lifecycle. Failure/cycle/timeout
coverage is indexed in [WS002 tests](../tests/README.md); original detailed
gates are retained in the [legacy plan](../history/phase011-019-legacy-plan.md).

## Interruption record

Not interrupted. fd 3 readiness was extended later by `ws002-p020`.

## Completion conditions

- Native PID 1 completes dependency-ordered boot and reverse-order shutdown.
- `/sbin/service` controls runtime services and persistent enablement as specified.
- Supervision, failure/cycle/timeout, mount, sync, halt, reboot, and poweroff cases pass.
