# ISO C and BSD libc API audit

## Scope

The ISO inventory is the function set required by the hosted library clauses
of ISO C17.  It excludes optional Annex K bounds-checking interfaces and the
conditionally-supported complex-number and C11 thread libraries.  Function-like
APIs such as `setjmp` are included even when the standard permits a macro.

The BSD inventory uses FreeBSD 15.1 as a current reference and selects
general-purpose extensions from `<err.h>`, `<stdio.h>`, `<stdlib.h>`,
`<string.h>`, and `<strings.h>`.  POSIX interfaces reviewed in the earlier
kernel-facing audit, device-name databases, capability databases, and
OS-administration helpers are excluded.

The authoritative, ordered inventory is
[`libc-api-audit.csv`](libc-api-audit.csv).  Implementation and review work is
performed from its first row to its last row.

## Normative and reference sources

- WG14 N2176, ISO/IEC 9899:2017 ballot draft:
  <https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2176.pdf>
- FreeBSD 15.1 source branch:
  <https://cgit.freebsd.org/src/tree/?h=releng/15.1>
- FreeBSD 15.1 library manual:
  <https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+15.1-RELEASE>

The table can be regenerated after a build with:

```sh
python3 tools/libc-api-audit.py \
    --repo . \
    --output plan/libc-api-audit.csv
```

## Status vocabulary

- `implemented`: the public header exposes the interface and a libc or math
  object defines the symbol (or the interface is a required macro).
- `declared-only`: the public declaration exists but no definition is linked.
- `missing`: the public declaration or implementation is absent.
- `review_status=pending|reviewed`: whether semantic review is complete.
- `fix_status=pending|not-needed|fixed`: disposition of review findings.

## Initial inventory result

| Family | Implemented | Declared only | Missing | Total |
|---|---:|---:|---:|---:|
| ISO C17 | 159 | 0 | 246 | 405 |
| Common BSD | 0 | 0 | 61 | 61 |
| **Total** | **159** | **0** | **307** | **466** |

This initial count is a declaration-and-symbol inventory, not a conformance
claim.  Each missing row is implemented first; every row is then reviewed
against its linked specification or manual page.

## Execution order

1. Implement each missing row in CSV order and regenerate the table.
2. Review all 466 rows in CSV order and record findings.
3. Consolidate findings in `plan/LIBC-API-FIX-PLAN.md`.
4. Apply all corrections and perform header, host, architecture, and QEMU
   regression checks.

## Final result

| Family | Implemented | Reviewed | Open findings | Total |
|---|---:|---:|---:|---:|
| ISO C17 | 405 | 405 | 0 | 405 |
| Common BSD | 61 | 61 | 0 | 61 |
| **Total** | **466** | **466** | **0** | **466** |

The initial result above is retained as the audit baseline.  The final CSV is
the authoritative row-by-row record after implementation, review, correction,
and integration testing.  The correction plan and completed disposition are in
`LIBC-API-FIX-PLAN.md`.
