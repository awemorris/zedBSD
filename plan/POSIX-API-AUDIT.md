# POSIX kernel API audit

## Scope

This audit uses POSIX.1-2008 as incorporated into POSIX.1-2017 (The Open
Group Base Specifications Issue 7, 2018 edition).  That matches
`_POSIX_VERSION == 200809L` advertised by zedBSD.

The authoritative inventory is
[`posix-required-kernel-api.csv`](posix-required-kernel-api.csv).  Rows are
ordered first by kernel subsystem and then by interface name; implementation
and review work follows that order.

Included interfaces are mandatory System Interfaces whose behavior depends on
kernel state or a kernel-backed object: files, processes, signals, clocks,
threads and synchronization, terminals, memory mappings, multiplexing, and
sockets.  The following are deliberately outside this kernel audit:

- pure ISO C computation, string, formatting, and character-set routines;
- POSIX Shell and Utilities commands;
- XSI-only interfaces;
- functionality enclosed by an optional POSIX margin legend;
- zedBSD, BSD, or Linux extensions.

An optional facility advertised by zedBSD may be reviewed separately, but it
does not become part of this mandatory-interface table merely because zedBSD
implements it.

## Normative source

- The Open Group, POSIX.1-2017 System Interfaces index:
  <https://pubs.opengroup.org/onlinepubs/9699919799/idx/functions.html>
- The Open Group, POSIX conformance and option rules:
  <https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap02.html>
- The Open Group, `<unistd.h>` option constants:
  <https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/unistd.h.html>

The inventory can be reproduced from the official `susv4-2018` HTML archive:

```sh
python3 tools/posix-kernel-api-audit.py \
    --posix-root /path/to/susv4-2018 \
    --repo . \
    --output plan/posix-required-kernel-api.csv
```

## Status vocabulary

- `implemented`: public declaration or macro and a compiled definition exist.
- `declared-only`: a public declaration exists but no compiled definition was
  found.
- `missing`: the required public header, declaration, or definition is absent.
- `review_status=pending`: semantic conformance has not yet been reviewed.
- `review_status=reviewed`: implementation and tests were reviewed against the
  linked standard page.
- `fix_status=pending|not-needed|fixed`: disposition of review findings.

## Initial inventory result

| Status | Count |
|---|---:|
| Implemented | 233 |
| Declared only | 1 |
| Missing | 49 |
| Total | 283 |

This is a source and symbol inventory, not yet a conformance claim.  Every row
must still pass the semantic review phase.

## Final result

| Result | Count |
|---|---:|
| Implemented interfaces | 283 / 283 |
| Reviewed interfaces | 283 / 283 |
| Review findings | 11 |
| Fixed findings | 11 / 11 |
| Open findings | 0 |

All interfaces in the scoped inventory now have a public declaration or macro
and a compiled implementation.  Every row was then reviewed in CSV order; the
11 findings and their dispositions are recorded in the `review_findings` and
`fix_status` columns.  The consolidated remediation record is
[`POSIX-API-FIX-PLAN.md`](POSIX-API-FIX-PLAN.md).

The table is regenerated and the review annotations reapplied with:

```sh
python3 tools/posix-kernel-api-audit.py \
    --posix-root /path/to/susv4-2018 \
    --repo . \
    --output plan/posix-required-kernel-api.csv
python3 tools/posix-kernel-api-review.py \
    plan/posix-required-kernel-api.csv
```

This result is limited to the mandatory, kernel-facing API scope stated above.
It does not by itself certify every behavioral edge case or optional POSIX
facility.

## Execution order

1. Implement each `missing` or `declared-only` row in CSV order and record its
   source and test evidence.
2. Review all rows in CSV order against the normative page and record findings.
3. Write the consolidated correction plan in `plan/`.
4. Apply every correction, rerun architecture builds and tests, and mark each
   finding `fixed` or `not-needed`.

All four execution steps are complete.  Build and test results are summarized
in the correction plan.
