# WS001 Phase 017: bounded cmp conformance

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p017`

Combined ID: `ws001-p017`

Status: Complete implementation milestone (`agent2-q001`, 2026-08-31)

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Close WS001 utility row 20's bounded `cmp` defects: add the required option
surface and historical skip extension, compare independent streams correctly
across arbitrary short reads, and preserve the specified
equal/different/error exit classes.

## Scope and fixed decisions

- Implement `-l` and `-s`, `--`, two file operands, and the optional `skip1`
  and `skip2` operands with checked numeric conversion.
- `-l` reports every differing byte in the required numbering/radix form;
  default mode reports the first difference; `-s` emits nothing.
- A `-` operand denotes standard input, but using standard input for both
  operands is rejected rather than compared through one shared stream.
- Maintain independent buffered-reader state so short reads, `EINTR`, and
  unequal chunk boundaries do not become false differences.
- Treat output and close failures as command errors.  Exit 0 means equal, 1
  means different, and values greater than 1 mean an operational error.

## Verification

The Phase-owned host fixture covers empty/equal/different inputs, first and
later differences, EOF asymmetry, `-l`, `-s`, both skips, decimal/octal and
invalid/overflow skips, FIFOs producing deliberately unequal short reads,
open failure, `--`, and both-stdin rejection. It compares applicable
output/status cases against the normative contract rather than importing
another implementation's test suite. Deterministic `EINTR`, read/close fault
injection, broken stdout, and locale diagnostics remain cross-cutting work.

## Completion conditions

- utility row 20 no longer has a known implementation incompatibility within
  this declared surface;
- the production source passes the complete host fixture and p015 style gate;
- the standalone package and native amd64 binary build;
- the WS001 ledger retains any locale-diagnostic or runtime evidence still
  unproved rather than promoting the row prematurely; and
- `make -j16` and `git diff --check` pass.

## Execution result

`cmp` now keeps independent 4096-byte reader state for each operand, retries
interrupted reads, implements mutually exclusive `-l`/`-s`, preserves exact
0/1/2 exit classes, reports the Issue 8 POSIX-locale formats, and accepts the
planned historical decimal/octal skip operands as an extension. The guest
smoke executes the installed native binary.

```text
CMP-T001 options, offsets, status, EOF, and short reads: PASS
BASE-STYLE-T001 objective source audit: PASS
native amd64 cmp ELF check: PASS
make -j16: PASS
AGENT2-Q001 amd64 utility smoke: PASS
```

The utility row remains `implemented-unreviewed` until the retained
cross-cutting fault and locale evidence is supplied.
