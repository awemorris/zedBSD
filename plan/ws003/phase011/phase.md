# WS003 Phase 011: common boot-parameter core and init selection

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p011`

Combined ID: `ws003-p011`

Status: Completed (`q015`, 2026-08-27)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Create one bounded, architecture-independent parser for the planned kernel
parameter grammar and use it to select PID 1 through `init=` without changing
root or swap behavior yet.

## Scope

- a kernel-owned parameter string with a 3071-byte maximum;
- exact tokenization of ASCII space-separated `name=value` items;
- indexed lookup for `boot0`--`boot3` and `swap0`--`swap3`;
- duplicate-known-key, malformed-known-key, overlength, and empty-value errors;
- bounded diagnostics for unknown keys;
- absolute-path validation for `init=`;
- `/sbin/init` as the no-parameter default;
- `init=/bin/sh` as an ordinary explicit PID 1 selection;
- removal of the implicit alternate-init fallback when the selected init
  cannot execute; and
- focused host fixtures that share the production parser.

## Non-goals

- loader or HAL handoff changes;
- interpreting boot slots, root selectors, overlay image paths, or swap
  sources in VFS;
- adding init arguments, a single-user-mode flag, or a shell fallback policy;
- parsing quoting, escaping, or Unicode.

## Design

Introduce a small common boot-parameter module rather than leaving parsing as
a static VFS helper. Initialization accepts a copied NUL-terminated string or
an empty input. The module parses once, stores slices or bounded copies in
kernel-owned memory, and provides typed accessors without reparsing the HAL
buffer from VFS and init independently.

Known singleton names are the complete initial public set from the reference
document. Duplicate known names make the parameter set invalid. Unknown names
are retained only as one bounded diagnostic event and are otherwise ignored.

`kern_init_start()` receives the selected absolute path. The execution path
must decide controlling-terminal behavior from the fact that the process is
PID 1 and the requested rescue path, not accidentally treat every alternative
supervisor as an ordinary child solely because its pathname differs from
`/sbin/init`.

## Work packages

1. Extract the current `boot.command-line` token scanning into a production
   common parser with explicit length and error contracts.
2. Add pure parser tests for every known name, sparse indices, unknown names,
   duplicates, malformed tokens, exact limits, embedded non-ASCII data, and
   missing NUL termination at the transport boundary.
3. Add `init=` selection and absolute-path validation without architecture
   conditionals.
4. Change init startup logging to print the actual selected path.
5. Make an explicit init execution failure visible and final; do not start a
   different program implicitly.
6. Preserve the current `/sbin/init` behavior when no command line is present.

## Verification

- Add BR-T42, a focused host fixture compiled against the production parser.
- Exercise `init=/sbin/init`, `init=/bin/sh`, the omitted default, relative and
  overlong paths, duplicates, and unknown parameters.
- Run applicable kernel/init focused tests.
- Run `make -j16` and `git diff --check`.
- Do not run `make check` or consume `.internal/` tests.

## Completion conditions

- one common parser represents the complete planned name set;
- every invalid known-token case fails deterministically;
- `init=` is selected without an architecture-specific branch;
- omission selects `/sbin/init`;
- explicit init failure does not silently fall back; and
- BR-T42, the build gate, and whitespace checks pass.

## Dependencies and handoff

This Phase has no dependency on the later root or swap implementation. p012
supplies real x86 loader strings. p013 and p014 consume the parsed boot/root and
swap values respectively.

## Reconsideration boundary

Stop for human review if the grammar requires quoting, arguments in `init=`, a
public binary parameter ABI, or a fallback init policy different from the
reference contract.

## Result

- Added one parse-once, kernel-owned 3072-byte parameter store and typed
  accessors for the complete initial key set.
- Enforced ASCII/NUL/3071-byte boundaries, `name=value` syntax, known-key
  uniqueness, bounded unknown-key reporting, and absolute `init=` paths below
  256 bytes.
- PID 1 now uses the selected path, defaults to `/sbin/init`, and an explicit
  execution failure enters the diagnosable idle path without trying
  `/bin/sh`. Explicit `init=/bin/sh` retains its interactive console role.
- BR-T42 passed, including boundary and no-fallback cases. The focused
  sanitizer run, affected amd64/i386 PC/AT/PC-98 kernel compile/link checks,
  and `git diff --check` also passed.
- Authoritative BR-T46 passed all 31 production-loader cells. Its normalized
  parameter markers plus default-init and explicit `init=/bin/sh` cells confirm
  the common parser and init-selection behavior on i386 PC/AT, i386 PC-98,
  amd64 BIOS, and amd64 UEFI.
