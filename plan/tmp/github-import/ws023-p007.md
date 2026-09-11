<!-- awesome-plan project=zedbsd record=ws023-p007 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws023/phase007/phase.md`

親: [ws023](https://github.com/awemorris/zedBSD/issues/24)

# WS023-P007: Conform amd64 interrupt, page, SMP, and task code

Last updated: 2026-09-03

Phase ID: `ws023-p007`

Status: complete (`q067`)

Parent: [WS023](https://github.com/awemorris/zedBSD/issues/24)

Depends on: `ws023-p006`

## Objective

Apply the canonical style to amd64 core execution paths while preserving
interrupt, page-table, SMP, and context-switch semantics.

## Scope

- `int.c/h`, `irq.c/h`, `lib.c`
- `page.c`, `pic.h`, `smp.c/h`
- `task.c/h`

## Procedure

1. Add required prototypes/comments and put public definitions before static
   definitions.
2. Hoist declarations, expand compact bodies, split meaningful calls and
   compound results, and use explicit result exits.
3. Preserve IRQ state, atomic/lock ordering, page-table mutation, TLB actions,
   AP admission, task-frame layout, and release ownership exactly.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, and a full amd64
  configured build.
- Run bounded amd64 SMP BIOS and UEFI QEMU login smokes because the Phase
  touches interrupt, page, task, and AP paths.

## Completion conditions

- All scoped files meet the WS023 contract.
- Both SMP firmware modes boot with unchanged CPU admission and login behavior.

## Execution result

The interrupt, page, SMP, library, and task group now meets the canonical
form.  Final review retained atomic-target-before-current-CPU observation in
the task switch.  Strict compilation, full linking, and four-CPU BIOS and UEFI
runtime gates passed; see the [q067 result ledger](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws023-x86-hal-style/tests/q067-results.md).
