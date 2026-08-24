# Phase 10: local reimplementation of bc, ed, and m4

## 1. Purpose

Phase 10 removes the three imported implementations currently present below
`userland/base` and replaces them with zedBSD-local implementations:

| Utility | Imported tree to remove | Replacement ownership |
|---|---|---|
| `bc` | Gavin D. Howard sources under `userland/base/bc/{src,include,gen}`, `config.h`, and `LICENSE.md` | new zedBSD arithmetic, parser, and evaluator code |
| `ed` | OpenBSD-derived `buf.c`, `ed.h`, `glbl.c`, `io.c`, `main.c`, `re.c`, `sub.c`, and `undo.c` | new zedBSD line editor code |
| `m4` | OpenBSD-derived sources and headers under `userland/base/m4`, including generated parser/tokenizer code | new zedBSD macro scanner and expansion engine |

The removal policy has priority over preserving the current level of
functionality.  A smaller, honest local implementation is acceptable as the
first replacement.  It must fail clearly for unsupported or invalid input and
must remain `implemented-unreviewed`; it must not claim POSIX conformance merely
because the command builds and runs.

The target is still the complete POSIX.1-2024 behavior when that can be reached
without importing another implementation.  The official specification may be
used as a behavioral reference.  Source code, generated parsers, compatibility
layers, tests copied from other implementations, or implementation-specific
tables must not be copied into base.

## 2. Fixed decisions

1. All production C and header files for these three utilities shall be
   authored for zedBSD and use the repository's normal Zlib license header.
2. No external library is introduced to provide a parser, arbitrary-precision
   arithmetic, editor engine, or macro processor.  Existing zedBSD libc APIs,
   including the local regex implementation, may be reused.
3. The imported files shall not remain as disabled sources, reference copies,
   fallback binaries, generated artifacts, or a separate base subdirectory.
4. Each package shall retain the standalone package interface from Phase 8.5:
   direct build, `PREFIX`, `DESTDIR`, and top-level cross-build registration.
5. Replacement is performed one package at a time as one coherent working-tree
   change: remove that package's imported source list, add the local source
   list, update tests, and restore both host and amd64 builds.  Do not leave the
   normal build broken between utilities.
6. Do not commit automatically.  Keep all work uncommitted until the user
   explicitly changes the existing no-commit instruction.
7. The three matrix rows remain `implemented-unreviewed` after a usable local
   replacement exists.  If a command temporarily has no buildable local
   implementation, its row shall be `missing`, not retained as implemented by
   pointing at imported code.
8. A row changes to `reviewed` only after its complete Phase 9 checklist and
   Issue 8 behavior pass.  The initial replacement gate is intentionally
   weaker than the conformance gate.

## 3. Phase 10.0: provenance and transition gate

Before changing a utility:

1. record its current file manifest, package source list, matrix row, host
   target, and QEMU fixture only to identify deletion and integration scope;
2. retain project-authored behavioral tests where useful, but remove host
   compatibility files that exist solely to port an imported implementation;
3. add a repository check that rejects the known imported file set and source
   fingerprints from `bc`, `ed`, and `m4`;
4. require every new production source and header in these packages to carry
   the zedBSD license header;
5. update the Phase 9 report and CSV notes to say `local replacement` and list
   known missing behavior; and
6. update the Phase 10 progress and affected component rows in
   `docs/posix-compliance-master.md`.  Do not leave notes that describe the
   removed port.

The mechanical check supplements review; it cannot prove authorship.  Review
the complete added source rather than relying only on copyright-string scans.

### Immediate replacement gate

Every first local replacement must:

- build with the host compiler using warnings as errors;
- build through `make -j16` for the configured amd64 target;
- build and install through `make -C userland/base/<utility>` with staged
  `PREFIX` and `DESTDIR`;
- accept standard input and file operands required by its initial documented
  subset;
- reject unknown options, malformed input, numeric overflow, and resource-limit
  exhaustion without crashing or corrupting files;
- distinguish success, false/no-result where applicable, and execution or
  syntax failure with non-zero status;
- handle output errors and interrupted I/O in the common I/O paths; and
- pass an amd64 `qemu-system-x86_64` smoke test that executes the actual zedBSD
  binary from the image.

Unsupported functionality must not be silently ignored.  Where the POSIX
syntax can be recognized safely, issue a concise unsupported-feature diagnostic
and return non-zero until the feature is implemented.

## 4. Phase 10.1: replace bc

### 4.1 Architecture

Implement `bc` as focused local modules rather than another single-file ad-hoc
evaluator:

| Module | Responsibility |
|---|---|
| decimal number | signed arbitrary-length magnitude, decimal scale, checked normalization and allocation |
| lexer | identifiers, numbers, strings, newlines, operators, comments, and source positions |
| parser | statements and expressions with explicit precedence and bounded recursion |
| runtime | variables, arrays, functions, call frames, control flow, `ibase`, `obase`, and `scale` |
| front end | `-l`, file sequencing, standard input, diagnostics, and exit status |
| math library | locally implemented functions loaded only for `-l` |

Use a project-local big integer representation, for example base 1,000,000,000
limbs, with checked size arithmetic.  Decimal numbers shall store magnitude and
scale separately.  Do not use host floating point as the representation of bc
numbers.

### 4.2 Initial local subset

The first buildable replacement may initially provide:

- decimal integer literals and newline/semicolon-separated statements;
- unary sign, parentheses, `+`, `-`, `*`, `/`, `%`, and integer exponentiation;
- scalar assignment and expression printing;
- deterministic division-by-zero, syntax, overflow, and allocation errors;
- sequential input files followed by standard input where required; and
- arbitrary-length integer results, so the replacement is not tied to the host
  integer width.

If scale-aware division is not ready at the first gate, reject non-zero `scale`
instead of computing with binary floating point.  If `-l` is not ready, reject
it rather than claiming that the math library was loaded.

### 4.3 POSIX completion order

After the local subset is stable, implement in this order:

1. decimal scale rules for all arithmetic and comparison operations;
2. `scale`, `ibase`, `obase`, and `last`, including legal ranges and conversion;
3. relational, boolean, assignment, increment, and decrement operators;
4. `if`, `while`, `for`, `break`, `continue`, and statement blocks;
5. functions, parameters, `auto` variables, return values, recursion limits,
   and arrays;
6. strings and the standard `length()`, `scale()`, and `sqrt()` functions;
7. the locally implemented `-l` math functions and required initial scale; and
8. locale, line-wrapping, diagnostics, asynchronous events, and resource
   exhaustion behavior.

### 4.4 bc tests

Split the current upstream-oriented host target into a local replacement test
and a full conformance target.  The replacement test must cover the implemented
subset and explicit rejection of every postponed option or construct.  Keep
arbitrary-precision vectors, divide-by-zero, syntax failure, file ordering,
broken stdout, allocation failure, and bounded parser depth from the beginning.

Retain functions, arrays, scale, base conversion, and `-l` cases as future
conformance expectations even if they are not part of the first gate.  Do not
rewrite those expectations to bless incorrect results.  Remove the non-standard
`-q` dependency from zedBSD tests unless `-q` is deliberately retained as a
documented extension.

## 5. Phase 10.2: replace ed

### 5.1 Architecture

Build the local editor around these components:

| Module | Responsibility |
|---|---|
| line store | owned line records and stable line identifiers, initially memory-backed |
| address parser | numeric, `.`, `$`, relative, mark, BRE, range, `,`, and `;` addresses |
| command parser | one command at a time with suffixes and command-specific operands |
| regex/substitution | existing zedBSD libc BRE API, replacement expansion, and global selection |
| history | reversible edit transactions for `u` |
| file I/O | checked reads, atomic writes where possible, byte counts, modified state, and recovery |

A memory-backed line vector is acceptable for the first local replacement.  It
must use checked growth and a documented resource ceiling.  Move to a temporary
backing file or bounded chunk store before claiming full conformance for large
files; do not repeatedly reallocate one whole-file string.

### 5.2 Initial local subset

The first replacement shall aim to preserve the useful project-authored smoke
case with:

- numeric, `.`, `$`, and simple range addresses;
- `a`, `i`, `c`, `d`, `p`, `n`, `=`, `q`, and `Q`;
- `r` and `w` for regular files;
- a modified-buffer flag and refusal by `q` to discard changes;
- deterministic diagnostics and an optional quiet/script mode; and
- no file replacement after a failed or partial write.

Substitution, global commands, and undo may follow immediately after this
subset if they cannot be completed in the same work unit.  Unsupported commands
must return `?`/diagnostic behavior consistently rather than being ignored.

### 5.3 POSIX completion order

1. complete address grammar, marks, BRE search, and wraparound;
2. `s` with replacement backreferences/flags and empty-RE reuse;
3. `g`, `v`, `G`, and `V` with safe command-list execution;
4. `j`, `m`, `t`, `k`, `l`, `P`, `f`, `e`, `E`, and complete file semantics;
5. single-level undo with a well-defined edit transaction boundary;
6. shell escape and filename substitution only after argument construction and
   process execution are safe;
7. temporary backing storage, signal recovery, hangup preservation, and
   interrupted/full-disk write behavior; and
8. multibyte BRE, locale, exact diagnostics, byte counts, and all exit paths.

### 5.4 ed tests

Keep all editor tests script-driven and bounded.  Add cases for every address
form, empty/non-newline-terminated files, NUL rejection policy, long lines,
modified-file protection, regex errors, failed reads, output aliasing, full
disk, `EINTR`, signals, and undo after each modifying command.  The amd64 QEMU
test must edit and write a file using the local binary and verify the result
after reopening it.

## 6. Phase 10.3: replace m4

### 6.1 Architecture

Implement a local streaming macro processor with no generated parser:

| Module | Responsibility |
|---|---|
| input stack | files, included files, macro expansion text, positions, and bounded nesting |
| scanner | words, quote/comment delimiters, commas, parentheses, and literal text |
| symbol table | definition stacks for `define`, `pushdef`, `popdef`, and `undefine` |
| expansion engine | argument collection, `$0`--`$9`, rescanning, and recursion limits |
| diversions | bounded temporary streams and ordered `undivert` output |
| builtins | one locally implemented function per standard builtin family |
| front end | `-D`, `-U`, file sequencing, locale, diagnostics, and status |

Use an explicit input/pushback stack and iterative processing where possible.
Set checked limits for expansion depth, argument count, token size, total
temporary diversion storage, and included-file nesting.  A limit failure must
be diagnosed; it must not truncate output and return success.

### 6.2 Initial local subset

The first replacement may provide:

- ordinary text copying;
- default quote recognition and nested macro argument collection;
- `define`, `undefine`, `dnl`, `ifdef`, `ifelse`, `include`, and `sinclude`;
- positional substitution and recursive rescanning;
- `-Dname[=value]` and `-Uname`; and
- checked recursion, malformed-call, include, read, and output failures.

If diversions or expression evaluation are postponed, their builtins must fail
explicitly.  Do not retain the imported generated parser or the
`tests/m4-host-compat.[ch]` porting layer as part of the replacement.

### 6.3 POSIX completion order

1. `changequote`, `changecom`, definition stacks, `defn`, `dumpdef`, and
   indirect invocation;
2. `shift`, `len`, `index`, `substr`, and `translit` with byte/locale rules;
3. a checked local integer expression parser for `eval`, plus `incr` and
   `decr`;
4. diversions, `divnum`, `undivert`, and deterministic temporary-file cleanup;
5. `m4wrap`, diagnostics, tracing, and requested exit status;
6. standard system/file builtins with argument-safe execution and exact status;
7. all remaining Issue 8 builtins and option interactions; and
8. locale, quoting/comment corner cases, recursive expansion cycles, resource
   exhaustion, signals, broken stdout, and temporary-storage failure.

The exact builtin inventory shall be generated as a checklist from the Issue 8
page before step 1 and kept in the test file; do not rely on the removed
implementation's builtin table as the inventory.

### 6.4 m4 tests

Preserve the current project-authored examples for recursive expansion,
arithmetic precedence, string operations, include, and diversions as future
expectations.  Add focused tests for quoting, empty arguments, `$0`--`$9`,
definition stacks, rescanning order, include cycles, expansion/depth limits,
temporary storage, malformed expressions, short I/O, and broken stdout.

## 7. Integration and evidence

Add a Phase 10-specific provenance and behavior gate instead of weakening the
old Phase 4 meaning silently:

```text
make phase10-local-source-check
make phase10-bc-host-test
make phase10-ed-host-test
make phase10-m4-host-test
make phase10-local-host-test
make posix-phase10-qemu-test
```

`posix-phase10-qemu-test` shall use `qemu-system-x86_64`, a headless debug
console, explicit per-command markers, and a bounded timeout.  It must prove
that `/bin/bc`, `/bin/ed`, and `/bin/m4` are the locally built zedBSD binaries,
not host executables or stale image contents.

Keep the existing complete behavior tests callable as conformance targets.
They may remain failing and outside normal pass gates while a gap is explicitly
listed in the Phase 9 report.  Do not delete a correct behavioral expectation
merely because the first local replacement does not satisfy it.  Conversely,
do not leave a permanently failing target in the normal build/check target
list; separate replacement gates from conformance gates by name and purpose.

At the end of each utility replacement, run at least:

```text
git diff --check
make posix2024-utility-matrix-check
make phase10-local-source-check
make phase10-<utility>-host-test
make -C userland/base/<utility> all
make -C userland/base/<utility> install DESTDIR=<staging> PREFIX=/
make -j16
make posix-phase10-qemu-test
```

Do not run the aggregate `make check`; it remains outside this work as
previously requested.  Run clang-format over every zedBSD-authored C source and
header below `userland/` and verify with `--dry-run --Werror`.

## 8. Matrix and documentation transitions

For each replacement:

1. change `implementation_evidence` to the new local entry point and supporting
   modules;
2. change `test_evidence` to the exact local replacement test;
3. replace notes describing an imported/ported engine with a concise list of
   the currently implemented local subset and a link to the Phase 9 gap report;
4. leave status `implemented-unreviewed` while known POSIX gaps remain;
5. update the per-utility Phase 9 finding as features are completed, without
   erasing the history of the external-source removal; and
6. update the `P10-*` replacement gates plus every affected subsystem/API/libc
   row in `docs/posix-compliance-master.md` during the same work unit.

The conformance version macros remain unchanged.  Replacing external source is
a provenance milestone, not evidence for `_POSIX2_VERSION=202405L` or
`_XOPEN_VERSION=800`.

## 9. Completion criteria

Phase 10's required replacement milestone is complete when:

- no imported `bc`, `ed`, or `m4` implementation file remains under
  `userland/base`;
- no obsolete m4 host-port compatibility source remains;
- all three commands are built exclusively from zedBSD-local source;
- all three direct package builds and staged installs pass;
- the local replacement host and amd64 QEMU gates pass;
- unsupported features fail safely and are recorded in the Phase 9 report;
- the matrix evidence points only to local source and current tests;
- `docs/posix-compliance-master.md` records the completed replacement gates and
  every remaining conformance gap;
- `make -j16`, the matrix checker, formatting check, and `git diff --check`
  pass; and
- no row has been promoted beyond its evidence.

Full POSIX conformance is a subsequent completion level within the same phase.
It is reached utility by utility only after the POSIX completion lists and the
normal Phase 9 review checklist pass.  Phase 10 does not have to delay removal
of external code until that higher level is reached.

## 10. Reconsideration points

Stop and ask for direction instead of expanding scope automatically if:

- an implementation would require a new general-purpose kernel ABI rather than
  an existing libc/userland facility;
- the local regex, stdio, temporary-file, locale, or process APIs are unable to
  express required semantics without a material libc redesign;
- maintaining a useful minimal command would require preserving any imported
  source or generated artifact; or
- the replacement requires changing the public package/provider policy or
  advertised conformance environment.
