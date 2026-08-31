# WS001 Phase 018: bounded tee conformance

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p018`

Combined ID: `ws001-p018`

Status: Complete implementation milestone (`agent2-q001`, 2026-08-31)

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Close WS001 utility row 120's bounded `tee` defects: implement `-i`, remove
the fixed 32-file ceiling, and make every input, output, signal, and close
result observable without abandoning unaffected outputs.

## Scope and fixed decisions

- Implement `-a`, `-i`, combined/repeated options, `--`, and any number of
  output operands up to process/resource limits.
- `-i` ignores `SIGINT` for the command as required; default signal behavior
  otherwise remains intact.
- Dynamically allocate descriptor/output-name state with checked overflow and
  allocation failure.
- Retry interruptible reads/writes where appropriate, complete partial writes,
  diagnose each failed output once, close it, and continue sending later input
  to stdout and every remaining output.
- An initial open failure does not prevent processing of other outputs, but it
  contributes to the final nonzero status.  Close and stdout failures are
  likewise reflected truthfully.

## Verification

The Phase-owned fixture covers default and append behavior, no operands, 40
outputs, duplicate paths, open failures mixed with successes, a `/dev/full`
secondary output while another continues, a recoverable `/dev/full` stdout,
input errors, `-i` SIGINT handling, option termination, and binary/NUL data.
Deterministic partial-write, `EINTR`, allocation, descriptor-exhaustion, close
failure, and default-SIGINT injection remain cross-cutting work.

## Completion conditions

- utility row 120 has no known implementation incompatibility within this
  declared surface;
- all unaffected outputs receive the complete available input despite another
  output's failure;
- the production source passes the host fixture and p015 style gate;
- the standalone package and native amd64 binary build; and
- `make -j16` and `git diff --check` pass.

## Execution result

`tee` now parses combined/repeated `-a` and `-i`, dynamically owns every file
descriptor, uses unbuffered robust writes, diagnoses a failed output once,
retires it, and continues all other file operands. Open, read, write, and close
failures contribute to the final status. Default signal disposition is
unchanged; `-i` installs `SIG_IGN` for `SIGINT`.

```text
TEE-T001 options, outputs, failures, binary data, and SIGINT: PASS
BASE-STYLE-T001 objective source audit: PASS
native amd64 tee ELF check: PASS
make -j16: PASS
AGENT2-Q001 amd64 utility smoke: PASS
```

The utility row remains `implemented-unreviewed` pending the retained
deterministic fault-injection and locale evidence.
