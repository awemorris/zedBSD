# POSIX.1-2024 Shell and Utilities implementation plan

## 1. Purpose

This document is the execution plan for completing the Shell and Utilities
volume of POSIX.1-2024 (Issue 8) in zedBSD. The requested filename is kept as
`docs/posix2004.md`; the target standard is **POSIX.1-2024**, not a 2004
edition.

The baseline used to prepare this plan is commit `cb860a1`. The authoritative
inventory is `tests/posix-2024-utilities.csv`; this document must not become a
second, manually maintained inventory.

The work has two goals:

1. provide an explicit, failing command for facilities that need a future
   provider or background service; and
2. implement and review every other utility currently targeted by the zedBSD
   POSIX/XSI profile.

Official utility index:
<https://pubs.opengroup.org/onlinepubs/9799919799/idx/utilities.html>

## 2. Current baseline

At the baseline, the matrix contains 155 utilities:

| Requirement | Rows |
|---|---:|
| required | 111 |
| enabled option | 24 |
| disabled option | 20 |

| Status | Rows |
|---|---:|
| reviewed | 6 |
| implemented but not reviewed | 80 |
| missing | 49 |
| option disabled | 20 |

The advertised values remain `_POSIX2_VERSION=200809L` and
`_XOPEN_VERSION=700`. A command that always reports that its facility is
unavailable is not a conforming implementation of that command. Therefore
the version values must not be raised merely because all command names are
present in the root filesystem.

## 3. Fixed policy decisions

### 3.1 Commands deferred until providers and services exist

The following command names shall exist in the base system for now, but shall
report a concise diagnostic to standard error and exit with a non-zero status:

| Command | Temporary diagnostic reason | Future owner |
|---|---|---|
| `at` | job scheduling service is unavailable | scheduler provider |
| `batch` | batch scheduling service is unavailable | scheduler provider |
| `crontab` | periodic scheduling service is unavailable | scheduler provider |
| `logger` | system logging facility is unavailable | logging provider |
| `mailx` | mail provider is not installed | mail provider package |
| `talk` | talk rendezvous service is unavailable | talk provider |
| `lp` | no print destination is configured | CUPS package, if installed |

The temporary commands must have these properties:

- no spool file, catalog, log record, or other persistent state is created;
- the diagnostic identifies the invoked command and unavailable facility;
- all ordinary invocations return a non-zero status;
- malformed input must not crash, block, read a password, or consume an
  unbounded standard input stream;
- a host test invokes every installed name and verifies the non-zero status,
  diagnostic, and absence of side effects.

One common source file may dispatch on the basename of `argv[0]`, but each
command must be registered as a normal `basic` userland package. Do not use
shell scripts or `.scripts/` in the disk image.

`lp` is a special case. POSIX permits `lp` to fail when the system has no
output device or configured destination. The base implementation can
therefore be marked `reviewed` after its no-destination behavior and exit
status are tested. A selected CUPS package must replace ownership of the same
installed command; path ordering must never leave the base stub ahead of the
CUPS client.

For the other commands, failure-only behavior is explicitly non-conforming:

- `at`, `batch`, `crontab`, `logger`, and `mailx` remain completion blockers;
- `talk` remains `option-disabled` until the UP/XSI profile and its provider
  are enabled;
- installing a file with the right name must not change a row to `reviewed`.

The matrix checker shall gain a `deferred-stub` status. That status requires
valid implementation and test evidence, but is counted as pending exactly like
`missing`. `talk` need not use this status while its requirement remains
`disabled-option`; its installed stub is documented in `notes`. This makes
temporary user experience visible without weakening the conformance gate.

### 3.2 Provider packaging policy

The documented POSIX conformance environment shall require a mail provider
package. The provider must replace the temporary `mailx` command and provide
Send Mode; the XSI/UP environment must additionally provide Receive Mode.

CUPS is not part of the conformance environment. The base `lp` command
represents a machine with no configured printing system. Installing CUPS may
replace it with a functional client through an explicit package conflict,
provider selection, or alternatives mechanism. Unmanaged overwriting of a
base file is not acceptable.

The eventual scheduler and talk providers may also be separate packages, but
installation alone is insufficient: their required background processes must
be running in a conformance test environment.

## 4. Missing utilities to implement now

After excluding the six currently missing service/provider commands (`at`,
`batch`, `crontab`, `logger`, `lp`, and `mailx`), 43 missing matrix rows remain.
They are all in scope for implementation:

| Work group | Utilities |
|---|---|
| small/base and shell | `cal`, `expr`, `getconf`, `hash`, `nice`, `renice`, `tsort`, `ulimit`, `uudecode`, `uuencode`, `write` |
| locale and terminal data | `gencat`, `locale`, `localedef`, `tabs`, `tput` |
| language, traversal, and archive | `bc`, `ed`, `find`, `m4`, `pax` |
| process, credentials, and IPC | `fuser`, `ipcrm`, `ipcs`, `newgrp`, `ps` |
| development tools | `ar`, `cflow`, `cxref`, `nm` |
| compression | `compress`, `uncompress`, `zcat` |
| SCCS | `admin`, `delta`, `get`, `prs`, `rmdel`, `sact`, `sccs`, `unget`, `val`, `what` |

This list is a snapshot only. At the beginning of every milestone, rerun the
matrix checker and derive the work list from the CSV. If a dependency API is
only declared, always returns `ENOSYS`, or lacks the semantics required by the
utility, it is part of the milestone even if it is not listed above.

## 5. Execution order

### Phase 0: protect the inventory and gates

1. Run `git status --short`, `make posix2024-utility-matrix-check`, and
   `make check`; preserve the output as the before-state.
2. Extend `tools/check-posix-2024-utility-matrix.py` with
   `deferred-stub` as described in section 3.1.
3. Add a checker assertion that every `reviewed` row has both a real source
   file and utility-specific positive and negative test evidence. A common
   test file is valid only when it names and exercises that utility.
4. Do not modify the ordering or standard URLs in the CSV. Do not rerun the
   import script over manually reviewed status/evidence fields.
5. Keep `_POSIX2_VERSION` and `_XOPEN_VERSION` unchanged throughout this
   plan. The deferred rows intentionally prevent the final Issue 8 gate.

### Phase 1: install the temporary failure commands

1. Add the seven package registrations and a small shared implementation.
2. Keep provider substitution decisions in package metadata rather than in
   platform-specific `vmunix.mk` files.
3. Verify all supported platform link rules discover the packages through the
   existing `ZEDBSD_USERLAND_PACKAGE` mechanism.
4. Add host tests and a rootfs manifest test for all seven names.
5. Set matrix state as follows:
   - `at`, `batch`, `crontab`, `logger`, `mailx`: `deferred-stub`;
   - `lp`: `reviewed`, after the no-destination test;
   - `talk`: keep `option-disabled`, and record the stub path in `notes`.

Do not add spool directories, fake request identifiers, successful no-op
paths, or a `/dev/log` sink in this phase.

### Phase 2: low-dependency commands and shell built-ins

Implement in this order:

1. `cal`, `expr`, and `tsort`;
2. `uuencode` and `uudecode` with shared codec code;
3. `nice` and `renice`, using the existing `nice()`, `getpriority()`, and
   `setpriority()` interfaces;
4. `write`, using `utmpx`, terminal ownership/mode, and the tty device
   directly; no daemon is needed;
5. the `hash` shell built-in, including cache invalidation when `PATH`
   changes and `hash -r`;
6. the `ulimit` shell built-in, backed by `getrlimit()`/`setrlimit()` and
   affecting the current shell process;
7. `getconf`, generated from one maintained mapping of `sysconf()`,
   `pathconf()`/`fpathconf()`, `confstr()`, constants, and standard version
   names.

Acceptance notes:

- `expr` needs BRE matching, integer overflow detection, correct operator
  precedence, and the specified zero/null exit-status rules.
- `tsort` must detect cycles, diagnose them, and still emit a useful ordering
  as specified; duplicate edges must not corrupt indegrees.
- `uudecode` must reject path traversal and must apply only the permitted
  output mode bits.
- `write` must honor `mesg`, reject a non-terminal recipient, and select a
  writable terminal deterministically when a user has multiple sessions.
- `getconf` must not contain copied numeric constants that can drift from the
  public headers or libc implementation.

### Phase 3: locale catalogs and terminal descriptions

Implement `gencat`, `locale`, `localedef`, `tabs`, and `tput` as one dependency
chain, but as separate commands.

1. Audit `setlocale()`, locale objects, `localeconv()`, and `nl_langinfo()`.
   The current built-in `C`/`C.UTF-8` implementation is not sufficient proof
   that a locale produced by `localedef` can be loaded.
2. Define a versioned, endian-neutral locale artifact format with checked
   lengths and offsets. Put the reader in libc and the writer in `localedef`;
   do not duplicate locale semantics in each command.
3. Implement all required locale categories, `charmap` and `copy` handling,
   symbolic character validation, duplicate definition diagnostics, and
   atomic output installation.
4. Add the POSIX message-catalog API dependency (`catopen`, `catgets`, and
   `catclose`) if the API audit confirms it is absent. `gencat` output and
   the libc reader must share a documented, versioned format.
5. Implement `locale` from the active locale objects and keyword metadata,
   not from a separate hard-coded table.
6. Introduce a small terminfo reader and checked parameter expander shared by
   `tput` and future curses code. Ship descriptions for every `TERM` value
   used by zedBSD consoles and terminals.
7. Implement `tabs` through terminfo capabilities and tty output. It must
   validate monotonically increasing tab stops and terminal width.

Do not claim an arbitrary terminal name is supported by emitting fixed ANSI
escape sequences. Unknown or malformed terminal descriptions must fail safely.

### Phase 4: parsers, editor, traversal, and archive

Implement `bc`, `ed`, `find`, `m4`, and `pax`. Each parser needs unit tests
for its grammar before command integration.

#### `bc`

- Use arbitrary-precision decimal integers and scale-aware arithmetic; native
  `long long` or floating point is not sufficient.
- Implement functions, arrays, control flow, `ibase`, `obase`, `scale`, and
  the standard math library selected by `-l`.
- Detect divide-by-zero, invalid bases, recursion/resource exhaustion, and
  syntax errors without corrupting interpreter state.

#### `ed`

- Use a line-addressed buffer with a temporary backing file or bounded chunk
  store, not one reallocating whole-file string.
- Implement addresses, marks, BRE operations, substitutions, global commands,
  file I/O, shell escapes where required, undo, diagnostics, and modified-file
  protection.
- Make interrupted writes atomic and retain the editing buffer after an I/O
  error.

#### `find`

- Separate traversal from expression parsing/evaluation.
- Implement symlink modes, depth order, pruning, metadata predicates, logical
  precedence, `-exec`, `-exec ... {} +`, and correct error accumulation.
- Use `openat()`/`fstatat()`-style traversal where available to reduce races;
  never follow an unbounded symlink loop.

#### `m4`

- Implement tokenization, quoting, argument collection, recursive expansion,
  definitions, diversions, include processing, arithmetic, and the required
  built-ins.
- Put explicit depth and allocation limits around recursive expansion while
  preserving conforming behavior for normal input.

#### `pax`

- Provide the required read, write, copy, append, and list modes.
- Implement the required interchange formats, hard links, metadata, pattern
  selection, rename rules, and partial-I/O handling.
- Reject archive paths that escape the extraction root. Use temporary files
  and rename when replacing a regular file so a failed extraction does not
  destroy the old file.

### Phase 5: process, credentials, and System V IPC tools

Implement `newgrp`, `ps`, `fuser`, `ipcs`, and `ipcrm`. Begin with a read-only
kernel/UAPI dependency audit.

#### Kernel/UAPI rules

- Extend the existing `/dev/system` snapshot interface only when a required
  datum cannot be obtained from existing APIs.
- Use versioned fixed-width UAPI structures, reserved zero fields, cursor-based
  enumeration, and ILP32/LP64 layout tests.
- Snapshot an object under its owning lock, take references before dropping
  the lock, and perform `copyout()` after locks are released.
- Never expose kernel pointers or keep an unpinned user pointer.
- Apply credential checks to other users' command names, descriptors, and
  terminal information.

`ps` should reuse and extend the process snapshot used by `top`, rather than
introducing procfs solely for one command. Add only fields required by the
selected POSIX/XSI formats, such as process group, session, controlling tty,
effective IDs, state, priority, and accumulated CPU time.

`fuser` needs a stable kernel query relating a vnode/mount/socket object to
referencing processes. Do not attempt to infer this by comparing pathname
strings. The query must account for current/root directories, executable
objects, open descriptors, and mapped files when required by the selected
profile.

The existing System V IPC operations are the base for `ipcs` and `ipcrm`.
Add enumeration/statistics operations only if the existing control calls
cannot enumerate all visible objects. Permission checks must be identical
between enumeration and mutation paths.

`newgrp` must resolve `/etc/group`, supplementary membership, effective gid,
and the requested login-shell environment. Password-protected groups may use
the existing account/crypt facilities. It must not grant a group solely
because its textual name exists.

### Phase 6: development utilities

Implement `ar` and `nm` first, then `cflow` and `cxref`.

- Put archive parsing/writing in shared code used by `ar`, `nm`, and archive
  member handling elsewhere. Preserve unknown members and metadata where the
  operation does not replace them, and update archives atomically.
- `ar` must implement the standard operation/modifier combinations, symbol
  table behavior, duplicate member names, ordering, and deterministic error
  handling.
- `nm` must parse every ELF class and byte order produced for supported
  zedBSD platforms, as well as archive members. All offsets, counts, and
  string indices need overflow and bounds checks.
- `cflow` and `cxref` must share a real C tokenizer and declaration parser.
  Stripping comments and searching for parentheses is not acceptable. Handle
  preprocessing directives, strings, old-style declarations where required,
  storage class, definitions versus calls, stdin, and multiple input files.

Do not invoke the host `ar`, `nm`, compiler, or preprocessor at runtime and
call the result a zedBSD implementation.

### Phase 7: compression utilities

Implement the historical `.Z` LZW format once in shared code and expose it as
`compress`, `uncompress`, and `zcat`.

- Support streaming input/output and partial writes; do not buffer the whole
  input file.
- Validate code-width transitions, dictionary reset, truncated streams, and
  corrupt headers.
- `uncompress` and `zcat` may share the same executable, but behavior must be
  selected by the invoked name and options.
- File replacement must preserve the original when encoding/decoding fails.
- Include known-answer `.Z` fixtures and cross-check them with an independent
  implementation during development. The fixture bytes, not a host command,
  become the repository test oracle.

### Phase 8: SCCS suite

Implement the SCCS storage library before the ten front ends: `admin`,
`delta`, `get`, `prs`, `rmdel`, `sact`, `sccs`, `unget`, `val`, and `what`.

1. Write a strict parser and serializer for SCCS history files, including
   checksums, delta tables, weave/control records, flags, user lists, MRs,
   comments, and SID validation.
2. Preserve unrecognized-but-valid data when rewriting a file. Use a lock
   file, temporary output, `fsync()`, and atomic rename for every mutation.
3. Implement read-only commands (`val`, `what`, `prs`, read-only `get`) before
   mutating commands.
4. Add edit ownership and p-file handling (`get -e`, `unget`, `sact`).
5. Add history creation and mutation (`admin`, `delta`, `rmdel`).
6. Implement `sccs` last as an argument-safe dispatcher; do not construct a
   shell command string.
7. Test branching SIDs, concurrent edit attempts, corrupt checksums, keyword
   substitution, binary/long input limits, interrupted updates, and recovery
   from a stale lock.

The ten commands are one conformance feature. Do not mark early front ends
`reviewed` while they rely on an incomplete or non-transactional history
library.

### Phase 9: review all existing utilities

The 80 `implemented-unreviewed` rows remain part of the release work. Review
them in matrix order after the missing implementations stabilize. For each
utility:

1. read its Issue 8 page and change history;
2. enumerate option, operand, stdin/stdout/stderr, locale, environment, exit
   status, and asynchronous-event requirements;
3. add a positive test, a usage/error test, and boundary tests appropriate to
   the command;
4. verify short reads/writes, `EINTR`, broken stdout, allocation failure paths,
   and filesystem errors where applicable;
5. record the exact source and test evidence in the CSV; and
6. change the row to `reviewed` only after the review checklist passes.

Shell built-ins must be tested both as isolated parser/executor units and in a
running zshell, because changes to the current environment, descriptors,
traps, or process state cannot be proven by a host helper alone.

## 6. Source and build layout

- Put each normal command in `userland/base/<command>/main.c` with its local
  `Makefile` package registration.
- Put genuinely shared command code under `userland/base/common/` in focused
  modules. Do not turn unrelated commands into one large multi-call binary.
- Keep public libc declarations in `libc/include/`, architecture-neutral libc
  implementation in `libc/` or `userland/base/libc/` according to the current
  ownership convention, and generic kernel work in `src/kern/`.
- Platform makefiles may provide ABI-specific linking, but must not maintain
  separate command inventories. Package discovery remains the source of truth.
- `make` and checked-in sources must not depend on `.scripts/`.
- All targets continue to read platform selection from `config.mk`; do not add
  `make ARCH=...` paths.

When several commands share one source, ensure dependency generation and
object paths do not let platform builds silently use an object compiled with
the wrong ABI or feature flags.

## 7. Testing and evidence

### 7.1 Test layers

1. **Pure host tests:** parsers, codecs, archive formats, SCCS, locale artifact
   validation, arbitrary precision arithmetic, and expression evaluation.
2. **Hosted command tests:** complete command option/operand behavior where
   the host libc supplies equivalent primitives. These tests must not use host
   utility output as the specification.
3. **zedBSD runtime tests:** process credentials, tty/utmp, priorities,
   resource limits, IPC, process snapshots, file-use queries, and shell state.
4. **Rootfs tests:** every selected command is installed once at the intended
   path, provider replacement is unambiguous, and no host executable leaks
   into the image.
5. **Multi-ABI checks:** ILP32 and LP64 headers/UAPI layouts plus builds for
   every platform supported by each registered package.

Every test must have a bounded timeout. A hang, skipped execution, or command
name that merely exists is not a pass.

### 7.2 Per-milestone gate

Run, at minimum:

```text
git diff --check
make posix2024-utility-matrix-check
make userland-command-host-test
make check
make world
```

Use the configured platform from `config.mk`. If a test cannot run on the
selected platform, record it as unverified and run the appropriate platform
build before changing the matrix status.

For a row to become `reviewed`, evidence must cover successful behavior,
required failures/exit statuses, and the Issue 8 semantic delta. Fuzz or
malformed-input tests are mandatory for binary formats and recursive parsers.

## 8. Commit and hand-off discipline for lower-capability agents

An implementing agent shall work on one numbered phase, and within a large
phase on one utility family, at a time.

1. Read this document, the corresponding CSV rows, and every official utility
   page in scope before editing.
2. Record `git status --short` and inspect existing changes. Preserve user
   changes and unrelated untracked audit/plan material.
3. Search all declarations, definitions, syscall numbers, package names, and
   tests before changing a public interface.
4. Write a failing test that demonstrates the missing behavior before the
   implementation, except where the only initial failure is compile/link
   absence.
5. Prefer completing an existing libc/kernel primitive over adding a private
   command-only workaround.
6. Never mark a success stub, unconditional `ENOSYS`, host-tool wrapper, or
   failure-only provider stub as conforming.
7. Keep implementation, tests, package registration, and matrix evidence in
   the same logical change.
8. At the end, run the per-milestone gate and document commands actually run;
   do not report unexecuted tests as passing.

Large utilities (`bc`, `ed`, `find`, `m4`, `pax`, locale, and SCCS) should be
split into parser/engine/front-end commits, but no intermediate commit should
raise a conformance advertisement.

## 9. Completion state before init/service work

This plan is complete when:

- all 43 non-service missing rows are `reviewed`;
- `lp` is `reviewed` for the no-output-device profile;
- the 80 previously implemented rows have been reviewed with evidence;
- `at`, `batch`, `crontab`, `logger`, and `mailx` remain visibly
  `deferred-stub`, not falsely implemented;
- `talk` remains disabled and its installed failure command is documented;
- all matrix, host, runtime, rootfs, ABI, platform build, and disk-image checks
  applicable to the selected profiles pass; and
- `_POSIX2_VERSION` and `_XOPEN_VERSION` have **not** been raised past a gate
  blocked by the deferred facilities.

## 10. Follow-up: init and service implementation

The next project after this plan is an init and service-management system.
This is a formal hand-off item, not optional cleanup.

The follow-up must provide:

1. a real PID 1 with orphan reaping, ordered startup, shutdown, signal
   handling, service credentials, restart policy, and failure reporting;
2. a scheduler service for both one-shot (`at`/`batch`) and periodic
   (`crontab`) jobs, with durable spools, stored uid/gid/cwd/umask/environment,
   separate process groups, captured output, and mail-provider delivery;
3. a logging facility consumed by `logger` and libc `syslog()`, either through
   a supervised `logd` receiving `/dev/log` or a documented kernel-backed
   facility with equivalent observable behavior;
4. activation of the required mail provider in every documented conformance
   image;
5. a local talk rendezvous provider if the UP/XSI profile is enabled; and
6. service lifecycle and boot-integration tests, including crash/restart,
   malformed spool data, permissions, shutdown, and full-disk behavior.

After those facilities are implemented, replace the relevant stubs, change
their matrix rows to `implemented-unreviewed`, perform the normal review, and
only then consider `_POSIX2_VERSION=202405L` and `_XOPEN_VERSION=800`.
The conformance document must describe the exact required package set and
which services must be active. A minimal image without those providers is not
the POSIX.1-2024 conformance environment even if it uses the same libc headers.
