<!-- awesome-plan project=zedbsd record=ws001-p002 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase002/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p002: low-dependency commands and shell builtins

WSID: `ws001`

Phase ID: `p002`

Status: complete milestone; compliance review remains iterative

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Implement the planned low-dependency utilities and shell builtins without
waiting for large kernel or service providers. Split host-testable parsing and
algorithms from runtime behavior that required zedBSD/QEMU.

## Design and acceptance

- Use independent base-package Makefiles and the established staged install.
- Test option/operand errors and malformed inputs as well as normal output.
- Verify shell builtin state changes in the shell process rather than a child.
- Run the Phase 2 host groups and bounded Phase 2 QEMU scenario.

Exact executable cases are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The Phase implementation gates completed. Full Issue 8 review was not implied;
open semantics remain component rows in [WS001](https://github.com/awemorris/zedBSD/issues/2).

## Completion conditions

- All utilities and builtins selected for this Phase build and install locally.
- Normal, invalid-option, malformed-input, and state-changing builtin cases pass.
- The bounded Phase 2 QEMU scenario and top-level build pass.
