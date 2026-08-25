# ws001-p003: locale catalogs and terminal descriptions

WSID: `ws001`

Phase ID: `p003`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](../ws.md)

## Objective and scope

Add locale database/catalog foundations and the first terminal-description
support needed by POSIX utilities, with malformed-input and staged-install
coverage.

## Design and acceptance

- Keep generated locale/catalog data reproducible from local fixtures.
- Separate parsers and database lookup from command frontends.
- Validate catalog, locale, and character-map errors deterministically.
- Run Phase 3 host tests and bounded QEMU integration.

Fixtures and executable cases are indexed in
[WS001 tests](../tests/README.md).

## Result and resumption

The planned Phase 3 foundation completed. Terminfo packaging and standalone
base build work continued separately in `ws001-p085`.

## Completion conditions

- Selected locale, catalog, and terminal-description tools/data build and install.
- Valid and malformed fixture cases pass deterministically.
- Phase 3 host and QEMU integration tests pass.
