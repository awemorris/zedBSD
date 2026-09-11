# ws011-p001: `net.conf` v1 format and parser

WSID: `ws011`  
Phase ID: `p001`  
Combined ID: `ws011-p001`  
Status: complete
Parent WS: [WS011](../ws.md)

## Objective

Freeze the strict YAML-like grammar and implement a native parser, schema
validator, model, and canonical writer without changing current boot behavior.

## Work packages

- [x] Specify indentation, scalars, mappings, lists, comments, limits, and
  source-position diagnostics.
- [x] Implement the bounded parser without an external YAML dependency.
- [x] Validate names, IPv4 data, prefixes, references, topology, and cycles.
- [x] Implement deterministic canonical serialization.
- [x] Add valid, invalid, limit, duplicate-key, and round-trip fixtures.

## Completion conditions

- Every accepted construct has a documented meaning and canonical output.
- Invalid input fails without partial output or configuration.
- Parse/write/parse produces an equivalent model.
- Malformed and limit fixtures do not crash, hang, or exceed declared limits.
- Existing boot configuration remains unchanged until `ws011-p003`.

## Acceptance

Run `NCF-T001`–`NCF-T006` from the [shared test index](../tests/README.md).

## Completion record

The normative grammar is [format-v1.md](format-v1.md); empty optional
collections are omitted and flow syntax is rejected. The native model/parser/
validator/writer is in `userland/base/net/netconf.[ch]`. The host contract test
passes with strict warnings, the production amd64 `net` binary compiles and
passes its ELF check, changed C/header files were formatted with clang-format
19, and `git diff --check` passes.
