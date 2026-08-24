# Phase 9 POSIX.1-2024 utility compatibility audit

Audit date: 2026-08-24

The findings in this report are embedded in the live project tracker at
[`docs/posix-compliance-master.md`](posix-compliance-master.md).  Future work
updates the master while this file remains the evidence record for the first
audit pass.

## Result

Phase 9's first audit pass reviewed every one of the 111
`implemented-unreviewed` rows in `tests/posix-2024-utilities.csv` against its
official POSIX.1-2024 utility page, the current source, and the cited tests.
No row passed the Phase 9 review checklist, so this audit does not promote any
row to `reviewed`.

This is a hand-off report, not a claim that every unlisted detail is correct.
It records confirmed incompatibilities and missing proof so that later work can
be implemented locally.  External implementations shall not be imported into
`userland/base` to close these items.

## Method and labels

The official pages were read from the Open Group's Issue 8 publication at
`https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/`.
For each pending row the audit extracted the synopsis and the sections for
options, operands, I/O, environment, asynchronous events, exit status, errors,
and change history.  It then inspected the implementation and the test named by
the matrix row.

The tables use these labels:

- **P0 policy conflict**: imported external implementation is present in
  `userland/base`; it cannot be accepted under the project policy.
- **P1 known incompatibility**: source inspection demonstrates a required
  interface or semantic area is absent or materially incorrect.
- **P2 incomplete proof**: useful implementation exists, but the current tests
  and audit do not establish the complete Issue 8 contract.

P2 does not mean conforming.  It remains a release blocker until positive,
usage/error, boundary, short-I/O, `EINTR`, broken-output, allocation-failure,
filesystem-error, locale/environment, and runtime cases applicable to the
utility have been exercised.

## Cross-cutting findings

### External implementation policy

Three Phase 4 package trees conflict with the no-external-implementation
policy:

- `userland/base/bc` contains the Gavin D. Howard `bc` implementation and its
  upstream `LICENSE.md`;
- `userland/base/ed` contains OpenBSD `ed` sources; and
- `userland/base/m4` contains OpenBSD `m4` sources and generated parser code.

They must not be treated as acceptable base implementations.  Removal and a
local reimplementation are separate follow-up work; this audit deliberately
does not delete them or silently replace them.

#### Phase 10 resolution update (2026-08-24)

The three historical P0 findings above have been resolved: the imported trees,
generated parser sources, and obsolete m4 host compatibility layer were
removed, and independent local `bc`, `ed`, and `m4` replacements now pass the
Phase 10 provenance, host, package/install, top-level amd64 build, and QEMU
gates.  Their POSIX contracts are not complete, so the live master reclassifies
all three as P1 known incompatibilities and lists their exact remaining work.
The P0 rows later in this file are intentionally retained as the evidence from
the first audit pass rather than rewritten as if the conflict had never
existed.

### Test evidence is not sufficient

`tests/test-userland-commands-host.sh` is 110 lines and builds only a subset of
the utilities that cite it.  Its pending-utility checks are mostly one normal
example for `tr`, `cut`, `sort`, `uniq`, `comm`, `fold`, `nl`, `expand`,
`unexpand`, `grep`, `sed`, `awk`, `xargs`, `iconv`, `diff`, and `date`; only
`cut`, `iconv`, and `grep` receive a small negative check.  It contains no
utility-specific matrix markers for these pending rows and no injected short
I/O, `EINTR`, `ENOMEM`, broken stdout, or filesystem-failure coverage.

The shell rows cite `tests/sh-lexer-host-test.c`, but that file tests tokenizing
and unterminated quoting only.  It does not invoke `alias`, `cd`, `command`,
`env`, `getopts`, `printf`, `pwd`, `read`, `test`, `type`, `umask`, `unalias`,
or `wait`, and it cannot prove mutations of a running shell.  The separate
`sh-posix-builtins-host-test.c` currently covers only `hash` and `ulimit`.

The specialized Phase 4--8 tests provide valuable normal and malformed-input
coverage, but they do not cover every option, standard format, locale rule,
diagnostic/exit-status path, resource limit, interruption, or runtime scenario
required by Phase 9.  Exact per-utility test evidence must therefore be added
before changing any of the following rows.

### High-priority semantic gaps

- `awk` is only a 73-line record splitter.  It recognizes a few textual forms
  of `print` and `$N`, but lacks `-F`, `-v`, `-f`, patterns, variables,
  expressions, assignments, control flow, arrays, functions, ERE behavior,
  `BEGIN`/`END`, and redirection.  This is a new local implementation project,
  not an incremental conformance fix.
- `sh` has a useful simple-command lexer/executor, but not the complete POSIX
  shell grammar and execution environment.  Unsupported parameter expansions
  are explicit in `expand.c`; compound commands, full redirection/here-document
  behavior, functions, expansion edge cases, traps/jobs, and special-builtin
  error semantics need a grammar-level audit and implementation.
- `diff`, `sed`, `patch`, `pr`, `od`, `sort`, `xargs`, and several file
  utilities are small subsets rather than complete implementations.
- The SCCS suite shares a checked local history format, but it is not yet shown
  interoperable with the classic SCCS weave/control format and implements only
  a subset of the specified options.  The ten SCCS rows must remain one pending
  feature.

## Per-utility findings

Utility names link to the official Issue 8 pages.  Entries are kept in matrix
order.

| # | Utility | Finding | Hand-off |
|---:|---|---|---|
| 1 | [admin](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/admin.html) | P1 known incompatibility | Implements only `-i`, `-n`, `-r`, and `-y`; required user/flag/MR/descriptive-text administration, `-h`, `-z`, optional-option-argument rules, and classic SCCS interoperability are absent. |
| 2 | [alias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/alias.html) | P2 incomplete proof | Basic set/list/query exists; quoting of displayed values, name/error cases, alias-substitution timing, and persistence in a running shell are not tested. |
| 3 | [ar](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ar.html) | P2 incomplete proof | Archive mutation and a SysV/GNU symbol index exist; complete operation/modifier interactions, `-C`, position/name edge cases, malformed archives, metadata, interruption, and output failures remain unproved. |
| 6 | [awk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/awk.html) | P1 known incompatibility | Only trivial `print`/`$N` processing exists.  Implement the POSIX language locally, including options, grammar, EREs, variables, records/fields, arrays, functions, control flow, I/O, and diagnostics. |
| 7 | [basename](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/basename.html) | P2 incomplete proof | Basic suffix removal exists; `//`, root-only paths, trailing slashes, suffix-equals-result, empty results, locale, usage, and broken stdout need exact tests. |
| 9 | [bc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/bc.html) | P0 policy conflict | The current package is an imported upstream implementation.  Remove it from base and replace it with a project-local arbitrary-precision parser/VM before conformance review. |
| 13 | [cat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cat.html) | P2 incomplete proof | `-u` and copying exist; repeated stdin, partial reads/writes, `EINTR`, same-file/error paths, close errors, and broken stdout are not proved. |
| 14 | [cd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cd.html) | P1 known incompatibility | Missing `-L`, `-P`, and `-e`, `CDPATH`, logical `..`, `PWD`/`OLDPWD` updates, correct unset-`HOME` behavior, and shell-environment tests. |
| 15 | [cflow](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cflow.html) | P1 known incompatibility | Uses a token heuristic rather than a conforming C preprocessing/declaration analysis; macro/include options are accepted without providing full preprocessing semantics. |
| 16 | [chgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chgrp.html) | P1 known incompatibility | Numeric GIDs only; group names, `-h`, recursive `-R` with `-H`/`-L`/`-P`, symlink rules, and traversal/error behavior are absent. |
| 17 | [chmod](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chmod.html) | P2 incomplete proof | Numeric and substantial symbolic modes plus `-R` exist; omitted-who/umask semantics, symlink/traversal policy, special bits, race/error cases, and exhaustive grammar tests remain. |
| 18 | [chown](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chown.html) | P1 known incompatibility | Numeric UID/GID only; owner/group names, omitted components, `-h`, recursive link modes, and traversal/error semantics are absent. |
| 19 | [cksum](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cksum.html) | P2 incomplete proof | CRC path is plausible, but standard vectors at length boundaries, multiple files/stdin naming, read interruption, output/close failure, and accumulated exit status are not fully tested. |
| 20 | [cmp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cmp.html) | P1 known incompatibility | Missing `-l` and `-s`; independent short reads can be compared incorrectly, and `EINTR`, offsets, diagnostics, same-stdin, and close/error paths are incomplete. |
| 21 | [comm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/comm.html) | P1 known incompatibility | Column suppression exists, but comparison uses byte ordering rather than `LC_COLLATE`; sorted-input assumptions, long lines, read/write errors, and locale behavior are unproved. |
| 22 | [command](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/command.html) | P1 known incompatibility | Only ordinary dispatch and `-v` are recognized; `-p`, `-V`, lookup/reporting rules, special-builtin behavior, and 126/127 statuses are incomplete. |
| 23 | [compress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/compress.html) | P1 known incompatibility | Classic `.Z` LZW is implemented, but Issue 8 algorithm-selection interfaces and complete overwrite, metadata, signal, full-disk, corrupted-stream, and replacement semantics remain. |
| 24 | [cp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cp.html) | P1 known incompatibility | Regular-file copying only; required options, directories, recursion, symlinks, special files, metadata preservation, interactive/force behavior, alias detection, and robust I/O are absent. |
| 26 | [csplit](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/csplit.html) | P1 known incompatibility | Supports one numeric split only; regex operands, repeats, `-f`/`-n`/`-s`/`-k`, multiple sections, cleanup rules, line zero/errors, and output failure handling are absent. |
| 28 | [cut](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cut.html) | P1 known incompatibility | `-b` and `-c` are byte-equivalent and `-n`/multibyte semantics are missing; field delimiter/suppression and list grammar need correction and boundary/I/O tests. |
| 29 | [cxref](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cxref.html) | P1 known incompatibility | Token-based references are not a complete C translation-unit analysis; preprocessing options, declarations/scopes, output formats, width, and diagnostics need full implementation. |
| 30 | [date](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/date.html) | P1 known incompatibility | Always uses UTC, implements only a small format subset, and cannot set time; `-u`, operands, locale/timezone behavior, conversions, invalid dates, and permissions are incomplete. |
| 31 | [dd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dd.html) | P2 incomplete proof | A substantial operand/conversion path exists, but record accounting, `conv=` combinations, skip/seek/truncation, partial records, signals, `EINTR`, full-disk, and exact diagnostics need a dedicated matrix. |
| 32 | [delta](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/delta.html) | P1 known incompatibility | Only `-y` is parsed; required options, MR/comment rules, p-file selection, SID/permission cases, weave interoperability, signals, and transactional recovery are incomplete. |
| 33 | [df](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/df.html) | P1 known incompatibility | Only `-k` exists; `-P`/`-t`, correct filesystem/device identity, block accounting, operand resolution, default operand safety, locale formatting, and errors are incomplete. |
| 34 | [diff](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/diff.html) | P1 known incompatibility | Performs line-at-the-same-index comparison rather than a difference algorithm; output forms, context/unified hunks, whitespace/recursive options, binary/errors, and exit status 2 semantics are absent. |
| 35 | [dirname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dirname.html) | P2 incomplete proof | Basic lexical reduction exists; double slash, all-slash, trailing-slash, empty/long operand, locale, usage, and broken stdout cases need proof. |
| 36 | [du](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/du.html) | P1 known incompatibility | Only `-a` exists; `-s`/`-k`/`-x` and `-H`/`-L`, hard-link deduplication, mount/symlink/cycle rules, overflow, permissions, and traversal errors are absent. |
| 37 | [echo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/echo.html) | P2 incomplete proof | Simple joining/newline exists; implementation-defined `-n`/backslash cases must be documented, and NUL/locale/output-error behavior needs running-shell tests. |
| 38 | [ed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ed.html) | P0 policy conflict | The current package is imported OpenBSD `ed`.  Replace it with a project-local line editor before reviewing the extensive address, BRE, global, undo, file, signal, and recovery contract. |
| 39 | [env](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/env.html) | P1 known incompatibility | Implemented as a shell builtin that only prints the environment and rejects arguments; `-i`, assignments, utility execution, PATH lookup, and 126/127 statuses are absent. |
| 41 | [expand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/expand.html) | P1 known incompatibility | Accepts only one tab width, not a tab list; multibyte/column handling, file-boundary state, malformed options, read/write errors, and locale semantics are incomplete. |
| 43 | [false](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/false.html) | P2 incomplete proof | Status behavior is trivial, but operand handling in the real shell, redirection failure, traps, and special-builtin execution context are not cited or tested. |
| 46 | [file](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/file.html) | P1 known incompatibility | Hard-coded recognition and `-L` only; required magic-file options and processing, MIME mode, default database, locale descriptions, special files, and error behavior are absent. |
| 47 | [find](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/find.html) | P2 incomplete proof | A useful expression parser/traversal exists; precedence/action edge cases, `-exec ... +`, `-ok` locale prompt, link loops, races, permissions, mount boundaries, and interruption need full review. |
| 48 | [fold](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fold.html) | P1 known incompatibility | Byte-oriented width handling is incomplete for screen columns and multibyte characters; `-s`, backspace/tab/carriage return, long lines, and I/O errors need implementation/tests. |
| 49 | [fuser](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fuser.html) | P2 incomplete proof | Kernel query works in QEMU; permissions, all reference kinds, mount/block-device `-c` semantics, `-f`, `-u` identity, races, multiple operands, and exact output/status remain. |
| 51 | [get](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/get.html) | P1 known incompatibility | Parses only `-e`, `-k`, `-p`, `-s`, and `-r`; the remaining selection/listing/cutoff/include/exclude behavior, keyword rules, permissions, and classic histories are incomplete. |
| 53 | [getopts](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/getopts.html) | P2 incomplete proof | Cluster scanning and silent errors exist; option arguments across argv, `OPTIND` resets, `OPTARG` unsetting, invalid variable/optstring, explicit args, and running-shell state need tests. |
| 55 | [grep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/grep.html) | P1 known incompatibility | Only a subset of required options is accepted; multiple `-e`/`-f`, complete BRE/ERE/fixed semantics, binary/NUL, locale matching, long lines, output errors, and status 2 need work. |
| 57 | [head](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/head.html) | P2 incomplete proof | `-n` and `-c` exist; legacy syntax, multiple-file headers, zero/large counts, non-seekable short reads, repeated stdin, read/write errors, and locale diagnostics need tests. |
| 58 | [iconv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/iconv.html) | P1 known incompatibility | Supports UTF-8 validation/copy only; conversions, `-c`, `-s`, `-l`, encoding aliases/state, incomplete sequences, locale defaults, and streaming boundary behavior are absent. |
| 59 | [id](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/id.html) | P1 known incompatibility | Numeric current-process `-u`/`-g`/`-G` only; user operands, names, real/effective selection, group names, combinations, and lookup errors are absent. |
| 60 | [ipcrm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcrm.html) | P2 incomplete proof | ID/key removal works in QEMU; complete option combinations, invalid/stale IDs, ownership/permission, races, multiple objects, diagnostics, and partial-failure status remain. |
| 61 | [ipcs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcs.html) | P2 incomplete proof | Object enumeration works; every selection/detail option, field units/headings, users/groups/times, removed/racing objects, permission errors, and locale formatting need review. |
| 63 | [join](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/join.html) | P1 known incompatibility | Only `-1` and `-2` exist; `-a`, `-v`, `-e`, `-o`, `-t`, repeated/unpairable fields, locale collation, long records, and read/write errors are absent. |
| 64 | [kill](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/kill.html) | P1 known incompatibility | Basic numeric/name signals and `-l` exist; required `-s` form, complete symbolic set/listing, process-group operands, range/parse rules, permission errors, and shell-builtin context need work. |
| 66 | [link](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/link.html) | P2 incomplete proof | Direct `link()` wrapper exists; exact operand count, directories, existing targets, cross-filesystem, permissions, diagnostics, and status are not cited by tests. |
| 67 | [ln](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ln.html) | P1 known incompatibility | Only two-operand `-s` exists; `-f`, `-L`/`-P`, directory/multiple operands, target-directory basename rules, overwrite/diagnostic behavior, and alias cases are absent. |
| 68 | [locale](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/locale.html) | P1 known incompatibility | Shared artifact metadata exists, but supported-name discovery is restricted, and `-a`/`-m` plus `-c`/`-k` output, quoting, category/keyword coverage, environment precedence, and locale errors need completion. |
| 69 | [localedef](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/localedef.html) | P1 known incompatibility | Checked artifact writing exists, but charmap/source grammar, symbolic characters, all category keywords, collation rules, `copy`, ellipses, diagnostics, `-u`, portability, and atomic installation are incomplete. |
| 71 | [logname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/logname.html) | P2 incomplete proof | `getlogin()` path exists; no-login/session cases, stray operands, locale diagnostics, output failure, and QEMU login-session behavior are not tested. |
| 73 | [ls](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ls.html) | P1 known incompatibility | Several common options exist, but required Issue 8 option/output combinations, locale collation/character display, owner/group/time formats, symlink operands, recursion cycles, and errors are incomplete. |
| 74 | [m4](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/m4.html) | P0 policy conflict | The current package is imported OpenBSD `m4`.  Replace it locally before reviewing standard builtins, quoting/diversions, expression grammar, locale, nesting limits, signals, and output failures. |
| 78 | [mesg](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mesg.html) | P2 incomplete proof | Basic tty mode query/change exists; no-controlling-terminal, `y`/`n` parsing, unrelated permission-bit preservation, diagnostic/status, and QEMU tty tests remain. |
| 79 | [mkdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkdir.html) | P2 incomplete proof | `-p` and `-m` exist; full symbolic mode grammar/umask, intermediate modes, existing paths, slash/symlink/race cases, partial failure, and diagnostics need tests. |
| 80 | [mkfifo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkfifo.html) | P2 incomplete proof | `-m` exists but numeric parsing alone is insufficient; symbolic modes/umask, multiple operands, existing paths, permissions, cleanup/error accumulation, and filesystem support need proof. |
| 83 | [mv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mv.html) | P1 known incompatibility | Rename-only subset with simple destination directories; `-f`/`-i`, cross-filesystem copy/remove, directories, symlinks/specials, metadata, self/subtree checks, and failure recovery are absent. |
| 84 | [newgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/newgrp.html) | P2 incomplete proof | Membership/password and login mode paths exist; real set-ID/session behavior, supplementary groups, environment reset, shell replacement, audit/security failures, and interactive QEMU cases remain. |
| 87 | [nl](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nl.html) | P1 known incompatibility | Only a few body/start/increment options exist; header/footer styles, delimiters, width/format/separator, regex numbering, blank grouping, section resets, and locale/I/O behavior are absent. |
| 88 | [nm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nm.html) | P1 known incompatibility | Checked ELF/archive parsing exists, but several accepted options are ignored or incomplete; standard output formats, radix/sort/undefined/dynamic symbols, archive labels, malformed objects, and non-ELF policy need review. |
| 89 | [nohup](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nohup.html) | P2 incomplete proof | Signal ignoring and execution exist; tty-dependent `nohup.out` redirection, mode/ownership, stderr duplication, HOME fallback, open failures, messages, and 126/127 statuses need tests. |
| 90 | [od](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/od.html) | P1 known incompatibility | Fixed hexadecimal dump only; `-A`, `-j`, `-N`, `-t`, `-v`, legacy operands, typed formats, duplicate suppression, endianness, offsets, and read/output failures are absent. |
| 91 | [paste](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/paste.html) | P2 incomplete proof | Parallel/serial and a delimiter string exist; escaped delimiters, empty delimiter/list cycling, unequal/empty files, repeated stdin, long lines, descriptor limits, and I/O errors need proof. |
| 92 | [patch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/patch.html) | P1 known incompatibility | Minimal single-file unified-patch handling only; standard options, context/normal/ed formats, reversed/fuzzed hunks, pathname selection, rejects/backups, timestamps, safety, atomicity, and signals are absent. |
| 93 | [pathchk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pathchk.html) | P1 known incompatibility | `-p` exists but Issue 8 `-P`, empty/leading-hyphen checks, component/path limits for the relevant directory, searchability/non-directory cases, locale, and status aggregation need work. |
| 94 | [pax](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pax.html) | P1 known incompatibility | Useful ustar/pax read/write/copy exists, but most standard options and formats, pattern selection, ownership/modes/times/links/specials, append/update semantics, substitutions, volume/error recovery, and security cases remain. |
| 95 | [pr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pr.html) | P1 known incompatibility | Only a simple header/numbering path exists; columns/merge, page ranges, widths/lengths/margins, separators, form feeds, tabs, double spacing, locale/time, and error handling are absent. |
| 96 | [printf](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/printf.html) | P2 incomplete proof | Many conversions and escapes exist; format reuse, missing/extra arguments, numeric character constants, precision/width, locale, overflow/domain errors, `%b` corner cases, NUL, and output failure need running-shell tests. |
| 97 | [prs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/prs.html) | P1 known incompatibility | Only `-d` and `-r` are parsed and the data-spec set is partial; cutoff/all-delta selection, every keyword/escape, locale/time, malformed/classic histories, and diagnostics remain. |
| 98 | [ps](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ps.html) | P1 known incompatibility | A kernel snapshot and common selection/output fields exist; complete POSIX/XSI option semantics, default selection, tty/session/group/user lists, field widths/headings, time/state values, races, and permissions remain. |
| 99 | [pwd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pwd.html) | P1 known incompatibility | Always calls `getcwd()` and accepts no options; `-L`/`-P`, valid logical `PWD`, removed/unsearchable directories, shell state, and output errors are absent. |
| 100 | [read](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/read.html) | P1 known incompatibility | Reads one bounded line into one variable/`REPLY`; `-r`, `-d`, multiple variables, `IFS` splitting, backslash/continuation, NUL/EOF, long input, and current-shell semantics are absent. |
| 104 | [rm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rm.html) | P1 known incompatibility | `-f` and recursive removal exist, but `-i`, `-d`, `-v`, write-protected prompting, root/dot protection, symlink traversal, deep/racing trees, locale prompts, and error aggregation are incomplete. |
| 105 | [rmdel](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdel.html) | P1 known incompatibility | Basic `-r` SID removal exists; full leaf/branch/release constraints, ownership, pending edits, MR/history preservation, classic weave, locking interruption, and diagnostics remain. |
| 106 | [rmdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdir.html) | P2 incomplete proof | `-p` exists; slash/dot/root handling, parent stopping and diagnostics, symlinks/nonempty paths, permissions/races, multiple operands, and partial-failure status need tests. |
| 107 | [sact](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sact.html) | P2 incomplete proof | Shared p-file display exists; multiple/no pending edits, malformed/classic files, operand naming, permissions, output failure, diagnostics, and locale behavior need review. |
| 108 | [sccs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sccs.html) | P1 known incompatibility | Basic argument-safe dispatch exists, but standard options, directory/project-prefix rewriting, command-specific flags, bulk operands, exit propagation, and full subcommand set/format compatibility are incomplete. |
| 109 | [sed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sed.html) | P1 known incompatibility | Supports `-n`, `p`, `d`, and a narrow substitution syntax only; addresses, script files/`-e`/`-f`, BRE commands, hold/pattern spaces, branches, multiline behavior, writes, and errors are absent. |
| 110 | [sh](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sh.html) | P1 known incompatibility | Not a complete POSIX shell language or environment.  Implement and test full grammar, expansions, assignments, redirections/here-documents, compound commands/functions, execution/search, traps/jobs, special builtins, and error/status rules. |
| 111 | [sleep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sleep.html) | P2 incomplete proof | One numeric duration and `nanosleep()` exist; accepted syntax/range, fractional precision, overflow, signal interruption/remainder policy, locale decimal point, stray operands, and diagnostics need tests. |
| 112 | [sort](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sort.html) | P1 known incompatibility | In-memory bytewise ordering with only a small option subset; keys, locale collation, numeric/month/dictionary/fold/nonprinting modes, merge/check/output, stable ties, large data, and I/O errors are absent. |
| 113 | [split](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/split.html) | P1 known incompatibility | Basic `-l`/`-b` exists; `-a`, suffix exhaustion, size suffix grammar, zero/huge values, exact line/byte boundaries, file cleanup, existing outputs, short writes, and signals are incomplete. |
| 114 | [strings](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/strings.html) | P1 known incompatibility | Printable-byte scan with `-n` only; `-a`, `-t`, object-file sections, offset radices, locale/multibyte printability, long sequences, malformed objects, and I/O errors are absent. |
| 116 | [stty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/stty.html) | P1 known incompatibility | Only a few flags and `raw` are handled; `-g`, speeds, control characters, rows/columns, complete modes, parse/application atomicity, non-tty errors, and exact restorable output are absent. |
| 117 | [tabs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tabs.html) | P2 incomplete proof | Major predefined forms, explicit lists, `-T`, and terminfo output exist; exact historical layouts, `+m`, terminal width/margins, tty errors, malformed data, output interruption, and runtime terminal tests remain. |
| 118 | [tail](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tail.html) | P1 known incompatibility | Buffers input and supports only `-n`; `-c`, `-f`, `-r`, origin/sign forms, legacy syntax, growing/truncated files, pipes, large inputs, overflow, and robust I/O are absent. |
| 120 | [tee](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tee.html) | P1 known incompatibility | `-a` exists, but `-i` is absent, outputs are capped at 32, and short writes, `EINTR`, broken outputs, signal behavior, descriptor/open failures, and continuation/status rules are incomplete. |
| 121 | [test](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/test.html) | P1 known incompatibility | Implements only small unary/binary arities; compound negation/parentheses/AND/OR, all primaries, precedence by argument count, integer errors, symlinks, permissions, and `[` form are incomplete. |
| 122 | [time](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/time.html) | P1 known incompatibility | No `-p`, reports only elapsed time, omits user/system CPU, mishandles normalized time subtraction, and lacks signal/exec status, locale format, and redirection tests. |
| 124 | [touch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/touch.html) | P1 known incompatibility | Sets both times to now and always permits create; `-a`/`-m`/`-c`, `-r`, `-t`, `-d`, parsing/ranges/timezones, permissions, symlink policy, and partial failures are absent. |
| 125 | [tput](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tput.html) | P1 known incompatibility | Shared checked terminfo lookup/expansion exists, but only the local capability vocabulary is supported; standard operand forms, `clear`/`init`/`reset`, booleans/numbers/statuses, parameter language, tty/output errors, and broad database compatibility remain. |
| 126 | [tr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tr.html) | P1 known incompatibility | Byte ranges with `-d`/`-s` only; `-c`/`-C`, character classes, equivalence classes, repetitions, escaping, multibyte/locale semantics, empty sets, and robust I/O are absent or unsafe. |
| 127 | [true](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/true.html) | P2 incomplete proof | Status behavior is trivial, but real-shell operand/redirection/trap/special-builtin context is not cited or exercised. |
| 129 | [tty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tty.html) | P2 incomplete proof | Basic terminal-name/status behavior exists; non-tty stdin, inaccessible names, exact diagnostics, stray options/operands, output failure, and QEMU console/pty cases need proof. |
| 130 | [type](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/type.html) | P1 known incompatibility | Basic builtin/PATH reporting exists, but aliases/functions/keywords, multiple names, command hashing, not-found diagnostics/status, quoting, and running-shell resolution order are incomplete. |
| 132 | [umask](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/umask.html) | P1 known incompatibility | Numeric query/set only; `-S`, symbolic masks, omitted-who semantics, invalid input without mutation, and persistence/child inheritance in a running shell are absent. |
| 133 | [unalias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unalias.html) | P2 incomplete proof | Named removal and `-a` exist; option termination, multiple-name partial failure, exact status/diagnostic, and alias expansion/cache effects in a running shell need tests. |
| 134 | [uname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uname.html) | P2 incomplete proof | Required option letters are present; combined/default ordering, field values, unknown options, locale diagnostics, kernel failures, and broken stdout need exact tests. |
| 135 | [uncompress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uncompress.html) | P2 incomplete proof | Streaming `.Z` decoding exists; all name/stdin/stdout/overwrite/metadata cases, malformed code transitions, truncation, short I/O, signals, full disk, and atomic replacement remain. |
| 136 | [unexpand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unexpand.html) | P1 known incompatibility | `-a` and one integer width exist, but tab lists, leading-only rules across boundaries, backspace/multibyte columns, file boundaries, malformed lists, and I/O errors are incomplete. |
| 137 | [unget](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unget.html) | P2 incomplete proof | Basic pending-edit cancellation exists; `-n`/`-s`/`-r` combinations, multiple users/SIDs, q-file/locking recovery, permissions, classic files, and diagnostics remain. |
| 138 | [uniq](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uniq.html) | P1 known incompatibility | `-c`/`-d`/`-u` exist, but `-f`/`-s`, locale collation, long/NUL records, input/output aliasing, close/output errors, and option combinations are incomplete; output stream cleanup is defective. |
| 139 | [unlink](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unlink.html) | P2 incomplete proof | Direct `unlink()` wrapper exists; directory/symlink/missing/permission cases, `--`, exact diagnostics/status, and filesystem/race behavior lack cited tests. |
| 145 | [val](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/val.html) | P1 known incompatibility | Basic local checksum/SID validation exists; required options and diagnostic bitmask/status behavior, every structural error, long/binary data, multiple/classic histories, and output errors remain. |
| 147 | [wait](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wait.html) | P1 known incompatibility | Supports at most one numeric PID and otherwise waits only tracked/available children; multiple operands, job IDs, saved statuses, unknown/non-child rules, signals, 127 behavior, and running-shell jobs are incomplete. |
| 148 | [wc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wc.html) | P1 known incompatibility | `-c`/`-l`/`-w` exist; `-m`, multibyte/locale word semantics, multiple-file totals/labels, repeated stdin, huge counts/overflow, read interruption, and output failure are absent. |
| 149 | [what](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/what.html) | P2 incomplete proof | Identification scanning and `-s` exist; binary/NUL/chunk-boundary markers, stdin/multiple files, malformed/long text, read/write errors, and exact no-match status need review. |
| 150 | [who](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/who.html) | P1 known incompatibility | Ignores options/operands and prints a narrow utmpx view with fixed UTC formatting; `am i`/`am I`, headings/state/writeability/idle/PID fields, locale/time, file operands, and errors are absent. |
| 152 | [xargs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/xargs.html) | P1 known incompatibility | Tokenizes input then executes exactly once; batching and size limits, quoting/escaping correctness, EOF strings, `-I`/`-L`/`-n`/`-s`/`-x`/`-p`/`-t`/`-r`/`-0`, empty input, and 123--127 statuses are absent. |
| 155 | [zcat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/zcat.html) | P2 incomplete proof | `.Z` stdout decoding exists; multiple operands, suffix lookup, stdin, malformed/truncated streams, read/write interruption, broken pipe, diagnostics/status, and compatibility vectors remain. |

## Required follow-up order

1. Completed 2026-08-24: the local replacement plan in
   [`docs/phase10-local-reimplementation.md`](phase10-local-reimplementation.md)
   removed the three P0 external source trees and replaced `bc`, `ed`, and `m4`
   with honest local implementations.  Continue their P1 conformance work from
   the live master hand-offs.
2. Treat `awk` and `sh` as language implementation projects with grammar and
   semantic unit tests.  Do not extend their current ad-hoc parsers one syntax
   fragment at a time without an architecture review.
3. Complete the clearly subsetted P1 utilities by families: filesystem,
   stream/text, development/archive/SCCS, locale/terminal, process/IPC, then
   shell builtins.
4. Replace generic evidence with per-utility test files or explicit exact
   markers.  Add fault-injection shims for short reads/writes, `EINTR`,
   allocation failures, and filesystem errors, plus bounded amd64 QEMU tests
   for kernel-, tty-, credential-, process-, IPC-, and shell-state behavior.
5. Repeat this audit in matrix order.  Promote a row only when every applicable
   Phase 9 checklist item has both implementation and executable evidence.

## Matrix effect

This audit intentionally leaves all 111 rows as `implemented-unreviewed`.
That label means only that a command path/source exists; it is not a
conformance claim.  The 19 previously reviewed rows, five deferred stubs, and
20 disabled-option rows were outside this Phase 9 pass and were not changed.

## Validation performed

The following gates passed after the report was added:

- `make posix2024-utility-matrix-check` (155 rows, 19 reviewed, 116 pending);
- `make userland-command-host-test`;
- `make posix-shell-builtins-host-test`;
- `make -j16`; and
- the `posix-phase2-qemu-test` through `posix-phase8-qemu-test` targets and
  `posix-phase85-qemu-test`, using `qemu-system-x86_64`.

`git diff --check` also passed.  Every C source and header below `userland/`,
excluding the existing `userland/noct` external tree, passed
`clang-format-19 --dry-run --Werror`; the check included untracked Phase 1--8.5
sources as well as tracked files.

The host lacked `msgunfmt`.  Passwordless `sudo` was not effective in the
execution shell, so the Debian `gettext` package was downloaded and extracted
under `/tmp` and its host tool was placed on `PATH` for the test only.  Nothing
from that package was installed in the repository or zedBSD image.

The aggregate `make check` target was not run, as requested.

Phase 10 subsequently added and passed `make phase10-local-host-test`, three
direct package builds and staged `PREFIX=/` installs, `make -j16`, and
`make posix-phase10-qemu-test`.  It also reran clang-format over every C source
and header below `userland/`.  These later results are recorded in the live
master and do not alter the historical Phase 9 promotion count.
