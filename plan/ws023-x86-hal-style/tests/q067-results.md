# Q067 x86 HAL style-conformance results

Last updated: 2026-09-03

Queue: [q067](../../queue.md)

Workstream: [WS023](../ws.md)

## Result

All 88 maintained C and header files below `src/hal/i386/` and
`src/hal/amd64/` were refactored to the project coding style.  The review
preserved the five inherited i386 edits from `b4be6eb`, found and corrected
evaluation-order drift introduced during the style rewrite, and found no
public API, ABI, global-symbol, or structure-layout change.

Assembly files were explicitly outside this C/header Phase.  Concurrent
assembly edits and editor working files already present in the shared working
tree were neither modified nor staged by q067.

## Publication

The user-owned inherited checkpoint is `b4be6eb`.  Q067 published one `WIP`
checkpoint per completed Phase to `origin/main`:

| Phase | Commit |
| --- | --- |
| `ws023-p001` | `800808e` |
| `ws023-p002` | `98af9cf` |
| `ws023-p003` | `a4448fa` |
| `ws023-p004` | `daeba48` |
| `ws023-p005` | `8533329` |
| `ws023-p006` | `227d0d1` |
| `ws023-p007` | `47bc603` |
| `ws023-p008` | `7a43b12` |
| `ws023-p009` | `6fb7372` |
| `ws023-p010` | `2ab7a94` |
| `ws023-p011` | `7f4b9bc` |

Immediately before publication, `origin/main` remained at `106023a`; merging
it was a no-op.  The push of `106023a..7f4b9bc` to `origin/main` succeeded.

## Source audit

- `tests/x86-hal-style-audit.noct`: PASS, exact inventory 88/88.
- `git diff --check`: PASS.
- Strict compile review used `-Wstrict-prototypes`,
  `-Wdeclaration-after-statement`, and `-Werror`: PASS.
- Baseline/current object comparison covered 63 configured x86 translation
  units.  Public symbol names and types were identical; ABI delta: zero.
- Independent semantic review covered volatile I/O and MMIO order, atomic and
  short-circuit evaluation, interrupt/lock state, SMP admission, page-table
  ownership and shootdown, context-frame layout, firmware handoff, controller
  setup, and cleanup order.

The retained narrow language extensions are inline assembly; ABI-required
`packed`, `aligned`, `noreturn`, and `weak` attributes; compiler atomic and
variadic builtins; and fixed table initializers whose positional replacement
would be less safe.  Replaceable `_Static_assert` and the MADT flexible-array
member were converted to C89-compatible forms.

## Focused regression evidence

- i386 PC/AT and PC-98 HAL compile gates: PASS.
- amd64 HAL compile and complete `vmunix` link gates: PASS.
- ACPI-window, BR-T43 handoff, BR-T52 early-init/timecounter, MCFG, and MSI
  source fixtures: PASS, including the applicable ASan/UBSan variants.
- amd64 console ordinary, ASan, and UBSan fixtures: PASS.
- input ownership/resynchronization ordinary, ASan, and UBSan fixtures: PASS.
- PC-98 PIC-cascade fixture: PASS.
- PC-98 Xzed cursor regression: PASS, `(320,240) -> (420,290)`.
- WS003 p025 timecounter configuration-stamp rebuild: PASS.

## Configured builds and runtime

Fresh `make -j16` disk-image builds passed for:

- `config/ci/config-pcat.mk` at `build/q067-final-pcat`;
- `config/ci/config-pc98.mk` at `build/q067-final-pc98`;
- `config/ci/config-amd64.mk` at `build/q067-final-amd64`;
- `config/ci/config-intelmac.mk` at `build/q067-final-intelmac`.

Bounded runtime acceptance passed in all four required environments:

- i386 PC/AT reached `login:` (`build/q067-runtime-pcat-final`);
- i386 PC-98 reached `login:` (`build/q067-runtime-pc98-final`);
- amd64 BIOS, four CPUs, reached `init: system running` and `login:`
  (`build/q067-amd64-runtime-v1/bios/guest.log`);
- amd64 UEFI q35/xHCI USB, four CPUs, reached `init: system running` and
  `login:` (`build/q067-amd64-runtime-v1/uefi/guest.log`).

The older combined runtime wrapper also passed its PC/AT and PC-98 cells, but
could no longer construct its hard-coded 128-MiB amd64 FAT fixture after the
normal root/data/swap payload grew beyond that capacity.  The production
amd64 image and both direct firmware-mode cells passed, so q067 did not change
that unrelated test-fixture capacity.

The final post-review source state was recompiled in independent roots
`build/q067-postreview-pcat`, `build/q067-postreview-pc98`, and
`build/q067-postreview-amd64`; all three HAL gates passed.  The aggregate
`make check` target was not run, as required by the Queue.

## Retained baseline risks

The review observed the following pre-existing risks without changing them
inside this style-only Workstream:

- i386 fixed-page claim reads the caller descriptor before ownership is
  acquired;
- i386 SMP timeout/failure retains an AP bootstrap stack;
- PC-98 console pointer arithmetic precedes one null-input guard;
- i386 integer formatting has an `INT_MIN` negation edge case;
- PC-98 declares an unreferenced cursor-move operation without a definition.

These are follow-up candidates, not q067 regressions or hidden functional
changes.
