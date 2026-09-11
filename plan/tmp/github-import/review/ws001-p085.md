# ws001-p085: terminfo packages and standalone base builds

Legacy designation: Phase 8.5

WSID: `ws001`

Phase ID: `p085`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Add local `terminfo`, `terminfo-extra`, curses, `tic`, and `infocmp` packages;
align consumers such as `tput`; and convert base programs to independent
Makefiles honoring `PREFIX` and conventional installation directories.

## Design and acceptance

- `terminfo` is a data-only package with a Makefile and major terminals.
- Minor terminals are separated into `terminfo-extra`.
- With `PREFIX=/`, terminal data uses `/lib/terminfo` rather than a nonexistent
  `/share`; other prefixes use their documented hierarchy.
- Base packages build/install independently and no external base source is
  introduced.
- Terminal compiler/reader, curses, package, and QEMU cases pass.

Exact cases are indexed in [WS001 tests](../tests/README.md).

## Result and resumption

The Phase 8.5 package/build milestone completed. Semantic incompatibilities
found later remain in the WS001 ledger.

## Completion conditions

- `terminfo`, `terminfo-extra`, curses, `tic`, and `infocmp` build/install as designed.
- All affected base packages honor `PREFIX`, including `/lib/terminfo` for `/`.
- Terminal stack/tools, standalone package, top build, and Phase 8.5 QEMU tests pass.
