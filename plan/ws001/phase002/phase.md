# ws001-p002: low-dependency commands and shell builtins

WSID: `ws001`

Phase ID: `p002`

Status: complete milestone; compliance review remains iterative

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Implement the planned low-dependency utilities and shell builtins without
waiting for large kernel or service providers. Split host-testable parsing and
algorithms from runtime behavior that required zedBSD/QEMU.

## Design and acceptance

- Use independent base-package Makefiles and the established staged install.
- Test option/operand errors and malformed inputs as well as normal output.
- Verify shell builtin state changes in the shell process rather than a child.
- Run the Phase 2 host groups and bounded Phase 2 QEMU scenario.

Exact executable cases are indexed in [WS001 tests](../tests/README.md).

## Result and resumption

The Phase implementation gates completed. Full Issue 8 review was not implied;
open semantics remain component rows in [WS001](../ws.md).

## Completion conditions

- All utilities and builtins selected for this Phase build and install locally.
- Normal, invalid-option, malformed-input, and state-changing builtin cases pass.
- The bounded Phase 2 QEMU scenario and top-level build pass.
