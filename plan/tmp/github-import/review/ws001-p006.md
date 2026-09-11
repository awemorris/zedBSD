# ws001-p006: development utilities

WSID: `ws001`

Phase ID: `p006`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Implement the selected POSIX development utilities and their local parsers,
reports, and staged package interfaces.

## Design and acceptance

- Keep production code zedBSD-local and independently buildable.
- Test deterministic output, malformed source/input, and unsupported options.
- Exercise Phase 6 fixtures and host scripts, then QEMU integration.
- Record toolchain/provider limitations instead of silently skipping them.

Exact cases are indexed in [WS001 tests](../tests/README.md).

## Result and resumption

The Phase implementation gates completed. Full standards review remains
governed by the component rows and future bounded Phases.

## Completion conditions

- Every selected development utility builds and installs from local base source.
- Deterministic output, malformed-input, and unsupported-option cases pass.
- Phase 6 host, build, and QEMU gates pass with limitations recorded in WS001.
