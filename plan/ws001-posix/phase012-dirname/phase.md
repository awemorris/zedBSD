# WS001 Phase 012: bounded dirname correction

Last updated: 2026-08-25

Phase ID: `ws001-p012`

Status: complete implementation milestone; conformance review remains open

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Apply the bounded proof pattern from `ws001-p011` to `dirname`: correct lexical
edge cases and output failure handling, then retain any unproved conformance
areas in the WS ledger.

## Scope

- empty, no-slash, root, double-slash, and all-slash operands;
- repeated and trailing slash removal;
- long operands without fixed pathname buffers;
- `--`, operand-count failures, and stdout failure.

Locale diagnostics, allocation-failure injection, and native guest runtime
coverage are outside this Phase.

## Work packages

- [x] Extract the Phase from the tier-1 ledger.
- [x] Implement the pathname-string reduction without filesystem access.
- [x] Add `--` and reliable buffered-output failure detection.
- [x] Add focused host tests, including a dynamically sized long operand.
- [x] Format changed C and pass the focused host test.
- [x] Build and validate the native amd64 executable.
- [x] Update the compliance ledger and Phase result.

## Completion conditions

The focused host test passes, the amd64 native executable builds and validates,
and untested locale/allocation/native-runtime behavior remains explicitly
visible rather than being claimed conforming.

## Evidence and result

`sh plan/ws001-posix/tests/dirname-test.sh` reports
`WS001 dirname: PASS`. `make -j16 build/amd64/bin/dirname` builds the native
binary and passes the amd64 user-ELF validator. The final `make -j16` image
build also succeeds, and changed userland C is formatted with clang-format 19.

The utility remains `implemented-unreviewed`: localized diagnostics,
allocation-failure injection, and direct guest cases for stdout/allocation
failure remain for a cross-utility I/O/diagnostic audit.

## Resume point

Select another tier-1 candidate such as `link`, `unlink`, or `cksum`; do not
expand this closed Phase into a general pathname project.
