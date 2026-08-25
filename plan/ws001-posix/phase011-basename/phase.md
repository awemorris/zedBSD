# WS001 Phase 011: bounded basename correction

Last updated: 2026-08-25

Phase ID: `ws001-p011`

Status: complete implementation milestone; conformance review remains open

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Use one small P2 utility to restart iterative POSIX work: correct the known
lexical/output defects in `basename`, add focused evidence, and retain a
conservative ledger state where native runtime proof is still absent.

## Scope

- empty and all-slash operands, including the chosen `//` behavior;
- trailing slashes and final pathname component extraction;
- optional suffix rules, especially suffix-equals-result;
- the option terminator, operand-count errors, and stdout failure.

Internationalized diagnostics, injected allocation failure, and a native
zedBSD runtime fixture are outside this bounded Phase.

## Work packages

- [x] Re-rank the open compliance ledger by dependency and boundedness.
- [x] Select `basename` as a low-dependency proof/correction Phase.
- [x] Preserve an empty input instead of converting it to `.`.
- [x] Accept `--` and detect buffered stdout failure with `fflush()`.
- [x] Add focused host cases for the scoped semantics and failures.
- [x] Build and validate the native amd64 executable.
- [x] Narrow, but do not erase, the remaining ledger handoff.

## Evidence

`sh plan/ws001-posix/tests/basename-test.sh` reports
`WS001 basename: PASS`. `make -j16 build/amd64/bin/basename` builds the native
binary and passes the Noct user-ELF validator. Changed userland C is formatted
with clang-format 19.

## Result and remaining debt

The Phase is complete as a bounded implementation milestone. The utility stays
`implemented-unreviewed`: native zedBSD runtime evidence, diagnostic locale,
allocation-failure injection, and a deliberate broken-output test inside the
zedBSD libc environment remain. Those missing cases no longer justify
reopening this Phase; a future cross-utility I/O/diagnostic audit can close
them.

## Resume point

Select the next tier-1 proof candidate from the WS ledger, preferably
`dirname`, `link`/`unlink`, or `cksum`, rather than expanding this Phase into a
general pathname or stdio project.
