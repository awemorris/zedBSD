# ws011-p001: `net.conf` v1 format and parser

WSID: `ws011`  
Phase ID: `p001`  
Combined ID: `ws011-p001`  
Status: planned  
Parent WS: [WS011](../ws.md)

## Objective

Freeze the strict YAML-like grammar and implement a native parser, schema
validator, model, and canonical writer without changing current boot behavior.

## Work packages

- [ ] Specify indentation, scalars, mappings, lists, comments, limits, and
  source-position diagnostics.
- [ ] Implement the bounded parser without an external YAML dependency.
- [ ] Validate names, IPv4 data, prefixes, references, topology, and cycles.
- [ ] Implement deterministic canonical serialization.
- [ ] Add valid, invalid, limit, duplicate-key, and round-trip fixtures.

## Completion conditions

- Every accepted construct has a documented meaning and canonical output.
- Invalid input fails without partial output or configuration.
- Parse/write/parse produces an equivalent model.
- Malformed and limit fixtures do not crash, hang, or exceed declared limits.
- Existing boot configuration remains unchanged until `ws011-p003`.

## Acceptance

Run `NCF-T001`–`NCF-T006` from the [shared test index](../tests/README.md).

## Resume point

Decide the normative representation of empty collections, then write the
grammar before parser code.
