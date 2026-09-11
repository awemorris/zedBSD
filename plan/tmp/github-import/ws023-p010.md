<!-- awesome-plan project=zedbsd record=ws023-p010 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws023/phase010/phase.md`

親: [ws023](https://github.com/awemorris/zedBSD/issues/24)

# WS023-P010: Conform the amd64 console and input implementation

Last updated: 2026-09-03

Phase ID: `ws023-p010`

Status: complete (`q067`)

Parent: [WS023](https://github.com/awemorris/zedBSD/issues/24)

Depends on: `ws023-p009`

## Objective

Apply the canonical style to the large amd64 PC/AT console source, including
all production and focused-test conditional branches.

## Scope

- `bsp-pcat/cons.c`

## Procedure

1. Reorganize constants, tables, variables, declarations, public definitions,
   and static definitions without changing conditional compilation.
2. Convert every declared-`for` initializer and nested declaration to the
   leading ANSI declaration group.
3. Replace two cleanup `goto` paths with explicit ownership helpers.
4. Style output locking/rendering first, keyboard/input second, and
   initialization/test hooks last.
5. Preserve key tables, designated table initialization where positional
   replacement would reduce safety, console locking, cursor state, and evdev
   publication.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, the amd64 console
  output host fixture, and the input-ownership fixture so all conditional
  variants compile.
- Run a full amd64 UEFI build and bounded login/input smoke.

## Completion conditions

- `cons.c` meets the WS023 contract in every compiled variant.
- Output, keyboard, locking, and input-publication focused tests and runtime
  smoke pass.

## Execution result

The complete console/input implementation now meets the canonical form in its
production and fixture variants.  Original short-circuit device-read order was
retained; ordinary, ASan, UBSan, input-ownership, full-build, and UEFI runtime
gates passed.  See the [q067 result ledger](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws023-x86-hal-style/tests/q067-results.md).
