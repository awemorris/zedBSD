# ws002-p014: getty, login sessions, and respawn

WSID: `ws002`

Phase ID: `p014`

Status: complete baseline

Parent WS: [WS002](../ws.md)

## Objective and design

Provide supervised `getty`, `login`, authentication/session setup, controlling
terminal ownership, logout, bounded respawn, and crash-loop protection without
making init dependent on shell internals.

## Acceptance and result

Installed-image QEMU acceptance reached login, entered and exited an interactive
shell, and exercised respawn. The shared case index is
[WS002 tests](../tests/README.md).

## Interruption record

Not interrupted. Authentication-policy expansion is not implied by the
completed console-session baseline.

## Completion conditions

- Supervised `getty` reaches `login` with correct terminal/session ownership.
- Successful login, failed authentication, logout, respawn, and crash-loop cases pass.
- The installed QEMU system returns to a usable login prompt after session exit.
