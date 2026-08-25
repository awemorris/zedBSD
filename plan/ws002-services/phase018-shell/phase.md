# ws002-p018: POSIX.1-2024 `/bin/sh`

WSID: `ws002`

Phase ID: `p018`

Status: partial with recorded compatibility handoffs

Parent WS: [WS002](../ws.md)

## Objective and design

Remove zedBSD administration builtins and direct power ioctls, separate
interactive libedit behavior, and move `/bin/sh` toward Issue 8 grammar,
expansion, redirection, execution, trap/job-control, strict-mode, and selected
widely used extension behavior.

## Acceptance and result

The shell reached the minimum needed by login, cron, scripts, and integrated
QEMU operation. It is not promoted to full POSIX compliance: remaining grammar,
expansion, job-control, and error-semantic gaps stay in
[WS001](../../ws001-posix/ws.md). Host shell tests and installed QEMU scenarios
are indexed in [WS002 tests](../tests/README.md).

## Interruption and resume point

The Phase stopped at a useful partial result under its partial-success rule.
Resume standards work by extracting a new WS001 Phase from the shell ledger,
not by changing this historical result to complete.

## Completion conditions

- Removed zedBSD administration builtins and startup behavior do not remain in `/bin/sh`.
- The declared grammar, expansion, redirection, execution, strict/extension,
  interactive, signal, and job-control test set passes.
- Login, cron, and non-interactive scripts work in QEMU.
- Any omitted POSIX requirement is recorded in WS001; otherwise this Phase remains partial.
