# ws001-p004: parsers, editor, traversal, and archive

WSID: `ws001`

Phase ID: `p004`

Status: complete milestone; audit findings remain

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Implement the planned `bc`, `ed`, `find`, `m4`, and `pax` packages, including
standalone build/install behavior, parser error handling, and QEMU smoke tests.

## Design and acceptance

- Bound parser input, recursion, arithmetic, paths, and archive sizes.
- Preserve useful failure behavior for unimplemented grammar or options.
- Test package-specific normal and malformed fixtures.
- Verify staged install and Phase 4 QEMU execution.

The Phase 9 audit later found imported source in `bc`, `ed`, and `m4`; that
policy conflict was resolved by `ws001-p010`. Current semantic gaps remain in
the [WS001 ledger](../ws.md).

## Result and resumption

The original Phase 4 implementation milestone and its subsequent provenance
replacement are complete. Resume utility conformance only through a new Phase.

## Completion conditions

- `bc`, `ed`, `find`, `m4`, and `pax` build and install as local packages.
- Their declared normal and malformed-input fixture tests pass.
- The Phase 4 QEMU gate passes and provenance conflicts are resolved or handed
  to an explicitly named follow-up Phase.
