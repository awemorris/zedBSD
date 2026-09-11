# WS001 Phase 013: bounded link and unlink correction

Last updated: 2026-08-25

Phase ID: `ws001-p013`

Status: complete

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective and scope

Prove the small pathname utilities against actual host filesystem semantics:
operand counts, `--`, same-inode hard linking, existing/missing operands,
directory unlink rejection, diagnostics/status, and native builds. Cross-device,
permission, link-count exhaustion, injected I/O failure, and guest runtime are
retained for a later filesystem-failure matrix.

## Work packages

- [x] Add the missing `link --` option terminator.
- [x] Add a focused temporary-filesystem test for both utilities.
- [x] Format, run the host test, and build both native amd64 commands.
- [x] Update the compliance ledger with remaining proof debt.

## Completion conditions

The focused host test and native builds pass and the ledger remains
conservative about untested filesystem/guest failure behavior.

## Evidence and result

The focused test passes for hard-link identity, removal, option delimiters,
operand counts, existing/missing targets, and directory rejection. The amd64
native build passes. Cross-device, injected I/O, permission-matrix, and guest
runtime behavior remain explicitly outside this bounded Phase.
