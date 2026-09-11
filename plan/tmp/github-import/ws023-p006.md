<!-- awesome-plan project=zedbsd record=ws023-p006 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws023/phase006/phase.md`

親: [ws023](https://github.com/awemorris/zedBSD/issues/24)

# WS023-P006: Conform amd64 leaf and small core modules

Last updated: 2026-09-03

Phase ID: `ws023-p006`

Status: complete (`q067`)

Parent: [WS023](https://github.com/awemorris/zedBSD/issues/24)

Depends on: `ws023-p005`

## Objective

Establish the amd64 file/private-helper convention and style the small,
lower-risk HAL modules before touching the interrupt, MMU, and firmware paths.

## Scope

- `defs.h`, `asm.c/h`, `acpi-window.c/h`, `bsp.h`, and `clock.h`
- `cmain.c`, `descriptor.c/h`, `msi-source.c`, and `percpu.c/h`
- Small BSP policy/validation units:
  `bsp-pcat/early-init-policy.c/h`,
  `bsp-pcat/handoff-validation.c/h`,
  `bsp-pcat/mcfg.c`, and
  `bsp-pcat/timecounter-policy.c/h`

## Procedure

1. Add canonical envelopes and the duplicated private amd64
   `UNUSED_PARAMETER` helper.
2. Apply file ordering, prototypes, definition layout, leading declarations,
   intent comments, and explicit returns.
3. Preserve descriptor layouts, assembly wrappers, CPUID results, MCFG policy,
   and validation ordering exactly.

## Verification

- Run `git diff --check` and `make -j16 amd64-hal-compile`.
- Run ACPI-window, handoff-validation, early-init-policy, MCFG, MSI-source, and
  timecounter-policy focused fixtures.

## Completion conditions

- Every scoped file meets the WS023 contract.
- The focused fixtures and amd64 HAL compile gate pass without ABI changes.

## Execution result

The scoped leaf/core and policy modules now meet the canonical form.  ACPI
window, handoff, early-init, MCFG, MSI, and timecounter fixtures passed,
including applicable sanitizer variants; symbol/API/ABI comparison found no
delta.  See the [q067 result ledger](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws023-x86-hal-style/tests/q067-results.md).
