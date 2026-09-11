# WS018 Phase 005: core source consolidation

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p005`

Combined ID: `ws018-p005`

Status: Complete (`q025`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Make `exec.c` the sole owner of exec-target preparation and remove the dead
legacy image-loader API.  This is a source-ownership cleanup: exec behavior,
the declarations already exposed by `include/kern/exec.h`, and every supported
build result remain unchanged.

## Preconditions and dependencies

- Complete `ws018-p001` first so every build manifest has its final
  `src/drivers/` paths and this Phase edits each manifest only once.
- Before deletion, repeat a whole-tree definition/call/include audit for
  `boot_image_load`, `boot_image_loader`, and `kern/image.h`.  The current
  tree contains no consumer; build-list membership alone is not a live API.
- Copy or create any reusable focused fixture under the WS018 test directory.
  Do not recover an unselected test from `.internal/`.

## Work packages

### 1. Establish a behavior baseline

Record the current results for shebang recognition, the one optional shebang
argument, script `argv` reconstruction, `ARG_MAX`/vector bounds, allocation
failure, set-ID suppression for scripts, `MOUNT_NOSUID`, and saved credential
selection.  Include malformed, unterminated, overlong, CRLF, non-absolute, and
non-script inputs.  This baseline is the semantic contract for the merge.

### 2. Merge exec preparation into `exec.c`

Move the complete implementation of these existing routines from
`src/kern/exec-prepare.c` into `src/kern/exec.c`:

- `exec_shebang_parse()`;
- `exec_script_argv_build()` and `exec_script_argv_free()`;
- `exec_credential_prepare()`; and
- their private bounded-length helper.

Place the routines near the exec-target preparation code that consumes them,
retain their current external names and signatures, and keep private helpers
`static`.  Resolve include or static-name collisions explicitly; do not alter
error signs, limits, allocation ownership, argument order, credential rules,
or script recursion behavior as a cleanup convenience.

Keep `include/kern/exec.h` as the interface ledger.  This Phase does not split,
rename, inline away, or otherwise revise its existing declarations.  Update
all platform source/object manifests and focused test compile recipes to build
only `exec.c`, then delete `src/kern/exec-prepare.c` after the reference audit
is empty.

### 3. Remove the unused image-loader layer

Delete `src/kern/image.c` and `include/kern/image.h`, and remove `image.c` or
`image.o` from every architecture/platform build list.  Do not move
`boot_image_load()` into `boot.c`: an uncalled legacy `struct bootfs_file`
dispatcher is not part of the consolidated boot API.

The deletion audit distinguishes live source references from historical plan
text and generated/dependency output.  It must cover source, headers, build
manifests, linker scripts, and maintained tests.  If a live caller or loader
implementation is found, stop this deletion work and record its required
contract; do not silently preserve an unreviewed compatibility shim.

### 4. Repair manifests and dependency files

Update all supported `vmunix.mk` variants and any explicit host-test source
list.  Remove stale dependency/object expectations for both deleted source
files.  Do not edit generated dependency files or commit build artifacts.

## Verification

- KA-T040 covers the exec success/error, script-argument, allocation, mount
  flag, and credential cases before and after the source merge.
- A source/build audit finds exactly one definition of every retained exec
  preparation function and no reference to `exec-prepare.c`, `image.c`,
  `kern/image.h`, `boot_image_load`, or `boot_image_loader`.
- Link maps or symbol inspection show no duplicate exec helper and no newly
  exported private helper.
- All affected focused tests, `make -j16`, and `git diff --check` pass.  Do not
  use `make check`.

## Completion conditions

- all exec preparation is implemented in `src/kern/exec.c` behind the
  unchanged `include/kern/exec.h` contract;
- script parsing, reconstructed vectors, credentials, limits, errors, and
  ownership match the baseline;
- the unused image-loader source, header, objects, and references are absent;
  and
- every supported manifest resolves without either deleted translation unit.

## Reconsideration boundary

Stop and return the Phase `uncleared` if the final audit discovers a live image
loader, an external consumer that requires a changed exec declaration, or a
test that can pass only by changing exec semantics.  Extract that contract for
human review rather than merging live image code into boot or casually
changing `exec.h`.

## Execution result

Completed on 2026-08-28.

- KA-T040 passed both the pre-merge `exec-prepare.c` baseline and the
  post-merge `exec.c` implementation with 96 checks.  The retained shebang,
  argv-allocation, error-sign, set-ID, `MOUNT_NOSUID`, and credential behavior
  is unchanged.
- `exec_shebang_parse()`, `exec_script_argv_build()`,
  `exec_script_argv_free()`, and `exec_credential_prepare()` now have their
  only implementation in `src/kern/exec.c`; the bounded-length helper remains
  local to that translation unit and `include/kern/exec.h` was not changed.
- The live-tree and manifest audit found no consumer of the legacy image
  loader.  `src/kern/image.c`, `include/kern/image.h`, and all image/exec
  preparation manifest entries were removed without a compatibility shim.
- Fresh linked builds passed for amd64, i386 PC/AT, i386 PC-98, arm64/RPi4,
  sparcv9/sun4u, and m68k/X68k.  The normal configured `make -j16`, X68k target
  audit, symbol inspection, and `git diff --check` also passed.
- The rebuilt amd64 image passed the q35/xHCI USB-storage QEMU smoke through
  `login:`.  No human-decision boundary was reached.
