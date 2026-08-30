# WS008 Phase 010: restore the Noct host-script CLI contract

Last updated: 2026-08-31

WSID: `ws008`

Phase ID: `p010`

Combined ID: `ws008-p010`

Status: Blocked (`MB-008`); upstream maintainer repair or an explicit
compatible-revision decision is required

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Restore the established Noct command-line module-search contract used by
zedBSD's repository-owned build scripts, then prove that the host interpreter
can run those scripts throughout an ordinary production build.

This is an upstream Noct compatibility Phase. It does not authorize a zedBSD
wrapper which strips `--path`, copies required modules into each script, or
edits the pristine checkout below `build/NoctLang`.

## Discovery and blocking evidence

`ws008-p008` correctly pinned and built upstream commit
`3bf3d236aa8ce014c63853dee3b21fa023d877ed`, and its bounded File/Process/System
toolchain smoke passed. A later full zedBSD build reached the first PC-98 image
post-processing script and failed at:

```text
build/NoctLang/build-static/noct --path=tools/build \
    platform/pc98/tools/patch-stage2.noct build/pc98/vmunix
Unknown option --path=tools/build.
```

The failure is not a kernel or PC-98 compiler defect. At the pinned revision:

- `noct --help` no longer advertises `--path`;
- the runtime CLI rejects `--path=...` before loading the script;
- invoking the script without the option does not load its required
  `tools/build` module and therefore leaves helpers such as `zbReadFile` and
  `zbParseNumber` unavailable;
- upstream `README.md` and multiple upstream tests still document and invoke
  `--path`, including the `--compile --app --path=...` form; and
- zedBSD has many supported Noct build-script entry points which rely on that
  published contract, so changing only the first failing PC-98 recipe would
  conceal the regression rather than repair it.

The user has already stated that Noct will be repaired manually under
Principal Engineer ownership after rejecting prior automated source changes.
Consequently no agent may modify or publish Noct for this Phase until the user
provides an accepted upstream revision or explicitly selects the compatible-pin
alternative.

## Human decision and resume paths

Choose exactly one path before this Phase enters a Queue:

1. **Preferred maintainer path:** publish an accepted upstream Noct revision
   which restores the existing `--path`/`require` runtime contract (and the
   documented compile/application form), then pin that immutable revision.
2. **Compatible revision path:** explicitly select an older immutable upstream
   revision whose host CLI satisfies the same contract, accepting that it
   supersedes the `ws008-p008` "latest main" selection until a repaired newer
   revision is available.

A downstream source patch, a dirty ignored checkout, a shell/Python adapter,
or a zedBSD-wide rewrite which avoids module search is not an implicit third
choice. Any such proposal requires a newly documented ownership decision.

## Fixed compatibility contract

- `--path=DIR[:DIR...]` is accepted for ordinary script execution and makes
  modules in those directories available to `require` without copying source.
- The documented `--compile --app --path=...` form remains accepted and
  resolves the same module graph for application compilation.
- Unknown options remain errors; restoring `--path` must not turn malformed
  CLI input into positional source names.
- Relative path entries are interpreted from the invocation working directory,
  preserving current zedBSD recipes.
- Multiple path entries have deterministic left-to-right lookup and duplicate
  module resolution is deterministic.
- A missing required module or invalid path fails nonzero with a bounded,
  actionable diagnostic.
- Host source remains a clean detached checkout at one full commit ID. Noct
  source changes are authored, reviewed, and published in
  `awemorris/NoctLang`, not embedded in zedBSD.

## Work packages after release

1. Record the user's selected resume path and full accepted upstream commit.
2. Update `ZEDBSD_HOST_NOCT_REVISION` only to that immutable commit and rebuild
   the Process-enabled static host interpreter from a clean detached checkout.
3. Add a focused project-owned regression which loads one helper through
   `--path` in interpreter mode and compiles/runs a small required-module
   application through `--compile --app --path`.
4. Run the existing WS010 File/Process/System toolchain smoke.
5. Run an ordinary `make -j16` production build far enough to exercise all
   selected Noct image/ELF post-processing steps, not merely `make toolchain`.
6. Audit all live `$(NOCT)` recipes so every `--path` consumer is covered by
   either the ordinary build or a focused representative test; do not rewrite
   historical Queue evidence.
7. Record the source SHA, checkout cleanliness, executable checksum, focused
   CLI results, and full-build result.

## Verification

- `NOCT-T080`: `noct --help` advertises the restored option and malformed
  `--path` forms fail deterministically.
- `NOCT-T081`: a runtime fixture imports a helper available only through one
  `--path` entry and produces the expected result.
- `NOCT-T082`: a compile/application fixture using
  `--compile --app --path=...` builds and runs with the expected result.
- `NOCT-T083`: `make -j16 toolchain` passes the existing WS010 smoke with the
  accepted pinned interpreter.
- `NOCT-T084`: an ordinary `make -j16` production build completes its selected
  Noct-backed ELF/image checks and generators without `Unknown option`, missing
  helper symbols, or a non-Noct fallback.
- `NOCT-T085`: source and checkout audit proves the host is clean and detached
  at the recorded revision and zedBSD contains no downstream Noct source patch
  or compatibility wrapper.
- Run `git diff --check`. Do not run `make check` and do not consume
  `.internal/`.

## Completion conditions

- One user-approved immutable upstream revision satisfies the runtime and
  compile/application `--path` contracts.
- Focused module-search regression, the existing toolchain smoke, and the
  ordinary production build all pass with the same host executable.
- No supported zedBSD script is converted to Python, shell, duplicated helper
  source, or an ad-hoc option-stripping workaround.
- The Noct checkout remains pristine and upstream-owned.

## Relationship to p008 and p009

`ws008-p008` remains an honest completed record of the revision-selection,
clean-checkout, Process-enabled build, and bounded smoke that it actually ran.
This Phase records the production-path regression which that smoke did not
cover; it must complete before p008's pin can again be considered usable by the
whole zedBSD build.

`ws008-p009` remains separately blocked on a buildable upstream zedBSD target.
If one accepted revision resolves both p009's target defects and this Phase's
host CLI defect, p010 runs first and p009 may reuse that revision after its own
target gates. Passing p010 alone does not re-enable the target package.

## Authorization boundary

This P book is not Queue-ready while `MB-008` remains active. Planning and
read-only inspection are allowed. Editing, committing, or publishing Noct,
selecting an older pin, or implementing a downstream compatibility layer
requires the user's explicit release decision.
