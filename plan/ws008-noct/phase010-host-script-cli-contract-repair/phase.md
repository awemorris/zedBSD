# WS008 Phase 010: restore the Noct host-script CLI contract

Last updated: 2026-08-31

WSID: `ws008`

Phase ID: `p010`

Combined ID: `ws008-p010`

Status: Uncleared (`q047`); runtime `--path` and the ordinary production build
pass, but the independent `NOCT-T082` compile/application form still fails

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

The user retained Principal Engineer ownership after rejecting prior automated
source changes. On 2026-08-31 the maintainer reported that the upstream Noct
update now restores `--path`, released `MB-008`, and authorized this Phase to
resume after the current USB implementation reaches a buildable boundary.
The Phase therefore selects the accepted published revision; it still does
not authorize an agent-authored downstream or upstream Noct source patch.

## Released decision and selected path

The preferred maintainer path selected published revision
`e56274ff00894182da5c44f1b8a2fb2fcf2c3dac`. That full commit remains pinned
because it restores the interpreter-mode `--path` needed by zedBSD recipes.
Verification found that the same revision does not yet implement the
documented compile/application form. The ordinary zedBSD build no longer
depends on Noct's optional, unregistered `Binary` API: its own `zedbuild.noct`
module now implements the three required little-endian operations with public
language and `Packed.uint8` facilities. Another maintainer-owned upstream
repair is therefore required only for the frozen compile/application CLI
contract before this Phase can complete.

A downstream source patch, a dirty ignored checkout, a shell/Python adapter,
or a zedBSD-wide rewrite which avoids module search is not an implicit third
choice. Any such proposal requires a newly documented ownership decision.
Repository-owned byte encoding inside `zedbuild.noct` is permitted: it changes
neither Noct source nor module lookup, and is checked in interpreter and JIT
modes at signed/unsigned boundaries and invalid inputs.

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

## q047 execution result

The zedBSD host pin now names
`e56274ff00894182da5c44f1b8a2fb2fcf2c3dac`. The Process-enabled static build
and existing File/Process/System smoke completed. The resulting executable is
`build/NoctLang/build-static/noct`, with SHA-256
`10f58889a4a62023d65a747f04f9b7312fb71898190ab38f56deff45b8171d4f`.

The checkout is detached at that exact commit and
`git status --porcelain=v1 --untracked-files=all` is empty. Host checkout and
build stamps now live under `build/host-noct-state/`, outside the upstream
worktree. The transition removed four zero-length legacy
`.zedbsd-checkout-<revision>` stamps and one zero-length legacy
`build-static/.zedbsd-built-<revision>-process` stamp which the old zedBSD
Makefile had generated; no other untracked upstream file was removed.

The project-owned
[`noct-host-path-contract.sh`](../tests/noct-host-path-contract.sh) produced
these bounded results:

- `NOCT-T080` passed: help advertises `--path=DIR1:DIR2`, and empty and
  missing-`=` forms fail deterministically;
- `NOCT-T081` passed: interpreter mode resolved a helper available only via a
  two-entry path and selected the leftmost duplicate;
- `NOCT-T082` failed: the documented literal form
  `--compile --app --path=...` reports `Cannot open file --app.` before module
  resolution;
- the missing-module negative check passed with
  `Cannot resolve required module 'absent_module'`; and
- the source/state/live-recipe portion of `NOCT-T085` passed, covering 77
  actual top-level or platform recipe invocations after excluding the
  path-free toolchain smoke and nested `--checker-runner` arguments.

`NOCT-T083` passed through `make -j16 toolchain`. The focused `NOCT-T086`
zedbuild byte-primitive gate passes in interpreter and JIT modes for U32
maximum, I64 minimum/maximum, negative values, round trips, range/type
failures, and confirms that the live build module has no `Binary.` reference.
`NOCT-T084` then passed through the ordinary `make -j16`, including the
selected PC-98 image post-processing and image builder. No Noct source, zedBSD
wrapper, option-stripping adapter, or duplicated module was added. Of the
q047 gates, only the unrelated `NOCT-T082` compile/application form remains
uncleared.

The host initially exhausted the already full `/tmp` tmpfs during parallel C
compilation. Repeating the identical build with `TMPDIR` under `build/`
completed; this environmental retry is not a product failure.

## Resume condition

Publish a new maintainer-reviewed Noct commit which parses
`--compile --app --path=...`, resolves and bundles the required module graph,
and passes `NOCT-T082`.

Then update the full pin and rerun `NOCT-T080`--`NOCT-T086`. Do not weaken the
compile/application contract or patch the pristine checkout locally.

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
- `NOCT-T086`: zedbuild little-endian byte primitives pass signed/unsigned
  boundaries, round trips, and invalid inputs in interpreter and JIT modes
  without the optional `Binary` API.
- Run `git diff --check`. Do not run `make check` and do not consume
  `.internal/`.

## Completion conditions

- One user-approved immutable upstream revision satisfies the runtime and
  compile/application `--path` contracts.
- Focused module-search and zedbuild byte-primitive regressions, the existing
  toolchain smoke, and the ordinary production build all pass with the same
  host executable.
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

Q047 consumed the released revision and left this Phase uncleared at its frozen
gates. A subsequently published maintainer revision may update the zedBSD pin
and rerun them. Editing or publishing Noct source, selecting an unrelated
older pin, weakening the failed gates, or implementing a downstream
compatibility layer remains outside authorization.
