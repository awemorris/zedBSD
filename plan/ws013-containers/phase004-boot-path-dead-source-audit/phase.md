# WS013 Phase 004: boot-path dead-source audit

Last updated: 2026-08-30

Phase ID: `ws013-p004`

Status: completed in `q031`; depends on completed `ws013-p003`

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Remove the already confirmed, unreferenced kernel startup-menu source and then
audit the complete supported boot path for other source files that are absent
from production builds or reachable only from explicit test targets. Delete
only candidates supported by build-manifest and reference evidence; preserve
platform and test ownership that merely looks inactive from the amd64 UEFI
path.

## Why this is a separate Phase

The p002/p003 UEFI change replaces its own fixed-kernel and `LoadOptions`
flow, while the repository also retains legacy BIOS loaders, six target
platforms, and focused test binaries. A source can be dead for
`BOOTX64.EFI` yet remain required by another production or test entry. This
Phase follows the settled UEFI path so obsolete helpers can be classified
against actual consumers without widening p002/p003 during bootloader work.

## Confirmed removal set

The initial audit established that `src/kern/startup.c` is not present in any
production or focused-test object list and that neither `startup_menu()` nor
`startup_config_file()` has a caller. Its removal set is:

- delete `src/kern/startup.c`;
- remove startup-only constants, enums, state, and prototypes from
  `src/kern/internal.h`;
- remove the stale startup-message statement from `BUILDING.md`; and
- rewrite the stale startup-specific comment in `src/kern/device.c`.

This deletion does not remove the legacy loader paths themselves and does not
claim that every shell/device helper is production code.

## Residual audit inventory

The next execution records each candidate in the Phase evidence as
`production`, `test-only`, `unreferenced`, or `uncertain`:

- `src/kern/sched-stub.c`: currently absent from production object lists
  while `src/kern/sched.c` supplies supported kernels; verify there is no
  historical/focused target before deleting it.
- `src/kern/test-fault.c` and `include/kern/test-fault.h`: the implementation
  is conditionally compiled for `ZEDBSD_TEST_FAULTS`, but the source is not
  visibly owned by current production lists. Determine whether a focused test
  target intentionally links it, whether the feature is incomplete, or
  whether both implementation and hooks are stale.
- `src/kern/shell.c` and `src/kern/device.c`: classify them as test-only
  where the PC-98 `vmunix-m9` rules compile
  `shell-m9-test.o`/`device-m9-test.o`. They are not production-live merely
  because stale object filters mention `shell.o`/`device.o`, but they also
  must not be deleted as globally dead while the explicit M9 target and
  shutdown-order source fixture consume them.
- p003-obsoleted UEFI inputs such as `bootloader/uefi/load-options.c` and its
  header/tests: classify after `BOOTX64.EFI` stops consuming `LoadOptions`;
  remove them only if no retained compatibility or focused test owns them.
- declarations, comments, documentation, generated-source rules, and object
  filters that name a removed implementation even when they do not create a
  linker reference.

The inventory is a starting set, not authorization for an unrelated
repository-wide dead-code purge.

## Audit procedure

1. Enumerate every supported production kernel and loader entry and every
   focused boot/test image from the platform makefiles. Record which source
   list or explicit rule owns each candidate.
2. Trace entry-to-kernel call/reference paths for amd64 UEFI, amd64 BIOS,
   i386 PC/AT, i386 PC-98, arm64, sparcv9, and x68k. Treat architecture-specific
   entry assembly and link-script references as live evidence.
3. Search declarations, function/data references, build rules, tests,
   documentation, and generated tooling for each source. Do not equate
   `--gc-sections` removal or absence from one target with repository-wide
   deadness.
4. Classify test-only code by its named test target and fixture. Prefer moving
   clearly test-owned source under its owning test area in a later bounded
   Phase when relocation would obscure this audit.
5. Delete an additional source only when no production or retained test target
   compiles it, no entry/reference reaches it, and its intended capability is
   already replaced or explicitly abandoned. Remove its declarations, rules,
   comments, and docs in the same change.
6. Record retained candidates and why they remain. An uncertain candidate is
   retained and becomes a named follow-up rather than being guessed away.
7. Run focused source/reference checks, the supported `make -j16` build gate,
   UEFI p002/p003 acceptance, and representative legacy/test targets affected
   by the classified files.

## Non-goals

- changing the `zedbsd.cfg` contract or implementing new boot behavior;
- deleting a whole legacy platform because it is not the current machine;
- removing test-only fault hooks, shell helpers, or device helpers without
  first retiring or relocating their named fixtures;
- refactoring live scheduler, device, filesystem, or init architecture;
- using a successful amd64 UEFI link as the sole evidence for other targets.

## Completion conditions

- `src/kern/startup.c`, its declarations/types, and its stale documentation
  have no remaining tracked reference or build input.
- Every residual candidate above has a recorded classification, owning target
  or no-owner evidence, reference evidence, and retain/delete decision.
- Every additionally deleted file has no production/test build owner and no
  live symbol consumer; companion headers, make rules, comments, and docs are
  cleaned in the same bounded change.
- The audit explicitly records `shell.c`/`device.c` as test-only while
  their M9/shutdown-order fixtures remain, rather than reporting them as
  production boot sources or silently deleting them.
- The supported `make -j16` build gate passes without aggregate
  `make check`, and the affected focused host tests pass.
- amd64 OVMF boots through p002/p003, representative legacy x86 boot smoke
  tests still enter the kernel, and retained PC-98 M9 test objects still link
  if they remain in scope.

## Actual results and evidence

The 2026-08-30 audit produced the following complete candidate matrix. A
source was classified from explicit make prerequisites, symbol/reference
searches, retained focused fixtures, and the replacement path rather than from
one successful amd64 link.

| Candidate | Classification and ownership evidence | Decision |
| --- | --- | --- |
| `src/kern/startup.c` and startup-only declarations/docs | **Unreferenced.** No production or focused-test object list named the source, and neither `startup_menu()` nor `startup_config_file()` had a caller. | Deleted the source and startup-only internal declarations; removed the stale `BUILDING.md` claim and rewrote the `device.c` comment. |
| `src/kern/sched-stub.c` | **Unreferenced/replaced.** No platform or focused-test rule named this source; supported kernel object lists select `src/kern/sched.c`, which supplies the same scheduler entry points. | Deleted. |
| `src/kern/test-fault.c` and `include/kern/test-fault.h` | **Uncertain/incomplete, with live hook intent.** The header is included by UFS1, UFS2, packet-buffer, and Unix-socket paths. The implementation exists only under `ZEDBSD_TEST_FAULTS`, but no current production/focused source list owns `test-fault.c` and no retained target defines that option. | Retained the implementation, header, and hooks. Removing or activating the incomplete facility needs a separate decision and target. |
| `src/kern/shell.c` | **Test-only.** `platform/pc98/vmunix.mk` explicitly compiles it as `platform/pc98/shell-m9-test.o` for `build/pc98/vmunix-m9`; `run-system-shutdown-order-test.sh` also inspects its reachable halt/reboot calls. | Retained while both named fixtures remain. |
| `src/kern/device.c` | **Test-only.** The same PC-98 M9 target explicitly compiles `platform/pc98/device-m9-test.o`. Its appearance in `M9_STAGE2_OBJS`' normal-object filter is not a production owner because the ordinary `STAGE2_OBJS` list does not add `shell.o` or `device.o`. | Retained while the M9 target remains. |
| `bootloader/uefi/load-options.c`, `.h`, and the WS003 BR-T48/BR-T43 compatibility fixtures | **Obsolete/replaced.** The configured `BOOTX64.EFI` link no longer builds the helper and unconditionally ignores `LoadOptions`; `zedbsd.cfg` owns parameters. After the old QEMU script and BR-T43 conversion block were removed, `uefi-load-option-wrapper.c` had no reference outside itself. | Deleted both helper files, the BR-T48 script/wrapper, and only the LoadOptions-specific BR-T43 code. WS003 documentation labels BR-T48 as replaced by q031 `CT-T016`. |
| `bootloader/pc98/stage1.S`, `stage2.S`, and `stage3.S` | **Unreferenced/replaced.** No retained rule names these files. The native PC-98 chain explicitly owns `disk-ipl.S`, `lba2.S`, `partition-pbr.S`, and `bootzbsd.S` instead. | Deleted all three obsolete sources; the supported PC-98 loader and M9 kernel paths remain. |

The bounded deletion set therefore contains `startup.c`, `sched-stub.c`, the
three obsolete PC-98 stage sources, the UEFI LoadOptions helper pair, and the
two obsolete WS003 BR-T48 files. `test-fault.*`, its hook sites,
`shell.c`, and `device.c` remain intentionally present.

Verification evidence from the audit:

- exact path, symbol, build-rule, test, and documentation searches found no
  retained production/focused consumer of any deleted source; immediately
  before deletion the old WS003 `uefi-load-option-wrapper.c` matched only
  itself;
- the updated BR-T43 handoff fixture passed its ordinary run, ASan/UBSan run
  (LeakSanitizer disabled because the execution environment uses ptrace), and
  `-fanalyzer` build;
- `run-uefi-volume-discovery-test.sh` and
  `run-zedbsd-config-host-test.sh` passed their ordinary, sanitizer, and static
  analyzer coverage;
- `plan/ws004-hardware/tests/run-system-shutdown-order-test.sh` passed;
- the supported `make -j16` gate passed, and a forced
  `build/amd64/uefi/BOOTX64.EFI` rebuild passed the strict compile, link, and
  `check-bootx64.noct` validation without a LoadOptions object;
- the explicit retained M9 link target is `build/pc98/vmunix-m9`. It passed
  with `make -j16 ZEDBSD_PLATFORM=pc98 ZEDBSD_ARCHITECTURE=i386
  ZEDBSD_BOARD=pc98 build/pc98/vmunix-m9`; the patch check reported 614296
  bytes and checksum `68990931`; and
- scoped `git diff --check` and the final deleted-file/reference checks passed.
- A production amd64 SeaBIOS default-root cell reached `login:` after the
  deletion set, while the retained PC-98 M9 link remained the explicit
  cross-platform ownership check for `shell.c` and `device.c`.

This source-audit evidence does not claim a new physical or emulated boot run.
The q031 p002/p003 OVMF acceptance owns configured-loader runtime behavior;
this Phase proves the bounded deletion and retained PC-98 M9 link without
substituting an amd64-only link for cross-platform ownership evidence.

## Reconsideration boundary

Return to planning before deleting a candidate whose only apparent consumer is
generated, external, or hardware-only; whose removal would retire a supported
platform/test capability; or whose replacement depends on work outside the
approved boot-path audit.
