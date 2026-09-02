# WS008 Phase 010: restore the Noct host-script CLI contract

Last updated: 2026-09-02

WSID: `ws008`

Phase ID: `p010`

Combined ID: `ws008-p010`

Status: complete (`q063`, 2026-09-02)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Restore the established Noct command-line module-search contract used by
zedBSD's repository-owned build scripts, then prove that the host interpreter
can run those scripts throughout an ordinary production build.

This is an upstream Noct compatibility Phase. It does not authorize a zedBSD
wrapper which strips `--path`, copies required modules into each script, or
edits the verified release source extraction below `build/NoctLang`.

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
At q047 the Phase therefore selected the accepted published revision; it did
not authorize an agent-authored downstream or upstream Noct source patch.

## Q047 released decision and selected path

Q047 selected published revision
`e56274ff00894182da5c44f1b8a2fb2fcf2c3dac`. That full commit was pinned
because it restored the interpreter-mode `--path` needed by zedBSD recipes.
Verification found that the same revision did not yet implement the
documented compile/application form. The ordinary zedBSD build no longer
depends on Noct's optional, unregistered `Binary` API: its own `zedbuild.noct`
module now implements the three required little-endian operations with public
language and `Packed.uint8` facilities. At q047 another maintainer-owned
upstream repair was therefore required for the frozen compile/application CLI
contract before this Phase could complete.

A tracked downstream patch is authorized only for the separate zedBSD target
CMake/link integration owned by p009. Host language/CLI behavior remains the
unmodified `v2.0.1` release implementation; no option-stripping wrapper or
dirty source extraction is accepted.
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
- Host source is extracted from one verified official release archive. Its
  URL, tag commit, exact size, and SHA-256 are the source identity; the
  extracted source and built executable are not tracked by Git.

## Work packages after release

1. Record the user's selected release, tag commit, archive size, and digest.
2. Replace the host Git pin/fetch path with the verified release archive and
   rebuild the Process-enabled static host interpreter from a clean extraction.
3. Add a focused project-owned regression which loads one helper through
   `--path` in interpreter mode and compiles/runs a small required-module
   application through `--compile --app --path`.
4. Run the existing WS010 File/Process/System toolchain smoke.
5. Run an ordinary `make -j16` production build far enough to exercise all
   selected Noct image/ELF post-processing steps, not merely `make toolchain`.
6. Audit all live `$(NOCT)` recipes so every `--path` consumer is covered by
   either the ordinary build or a focused representative test; do not rewrite
   historical Queue evidence.
7. Record the archive identity, extraction manifest, executable checksum,
   focused CLI results, and full-build result.

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

## Q063 release decision

The user selected official release `v2.0.1`, tag commit
`ed621e79139f55d06dd1a474243afbf0ce5efe0a`, source archive size `2524680`,
and SHA-256
`68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
Pre-Queue probing showed that this release supports both interpreter
`--path=...` and the literal `--compile --app --path=...` form, so the q047
resume condition is satisfied. P010 replaced host Git checkout/fetch with the
verified release archive shared by the new source lifecycle, built the static
Process-enabled host interpreter below `build/NoctLang`, and reran
`NOCT-T080`--`NOCT-T086` plus clean/incremental `make -j16 toolchain` and the
ordinary build.

## Q063 execution result

Complete. Host acquisition uses the official `v2.0.1` archive at tag commit
`ed621e79139f55d06dd1a474243afbf0ce5efe0a`, exact size `2524680`, SHA-256
`68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
The ignored extraction carries the recorded source identity and manifest; its
source verifier passed after the build. The Process-enabled host executable
SHA-256 is
`db128557cacc7385976e491a26528bf14e3cfd47a2b9dbff78a63a64617653f6`.

`NOCT-T080`--`NOCT-T082` and `NOCT-T085` pass, including interpreter and
compiled-application module lookup plus malformed/missing-path negatives and
73 live `--path` recipe consumers. `NOCT-T083` and `NOCT-T086`, clean and
incremental `make -j16 toolchain`, removal/recovery of the generated host
artifact, and the ordinary configured `make -j16` pass. No host compatibility
wrapper or host language/CLI patch was added. P009's separately authorized
two-hunk target final-link CMake patch is not part of the host behavior.

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
- `NOCT-T085`: source/archive audit proves the host tree comes from the
  recorded release identity and zedBSD contains no host CLI compatibility
  wrapper. P009's independently authorized target-only CMake patch is allowed.
- `NOCT-T086`: zedbuild little-endian byte primitives pass signed/unsigned
  boundaries, round trips, and invalid inputs in interpreter and JIT modes
  without the optional `Binary` API.
- Run `git diff --check`. Do not run `make check` and do not consume
  `.internal/`.

## Completion conditions

- One user-approved immutable upstream release satisfies the runtime and
  compile/application `--path` contracts.
- Focused module-search and zedbuild byte-primitive regressions, the existing
  toolchain smoke, and the ordinary production build all pass with the same
  host executable.
- No supported zedBSD script is converted to Python, shell, duplicated helper
  source, or an ad-hoc option-stripping workaround.
- The Noct archive remains pristine and upstream-owned; any extracted patch is
  limited to p009's separately recorded target CMake integration.

## Relationship to p008 and p009

`ws008-p008` remains an honest completed record of the revision-selection,
clean-checkout, Process-enabled build, and bounded smoke that it actually ran.
This Phase records and closes the production-path regression which that smoke
did not cover. P008 remains truthful history; q063's verified release archive
supersedes its former active Git pin for the whole zedBSD build.

`ws008-p009` followed p010 in q063 and uses the same release archive plus its
authorized target-only CMake patch. P009 separately re-enabled and accepted
the target package.

## Authorization boundary

Q047 consumed its then-published revision and honestly left this Phase
uncleared. Q063 consumed `v2.0.1`, replaced Git acquisition with the verified
tarball, and passed the frozen host gates without weakening them or adding a
host CLI compatibility layer. The separately authorized p009 patch connects
only the zedBSD target final-link adapter.
