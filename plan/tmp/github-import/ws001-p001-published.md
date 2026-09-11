<!-- awesome-plan project=zedbsd record=ws001-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase001/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p001: temporary failure commands

WSID: `ws001`

Phase ID: `p001`

Status: complete

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Install explicit commands for utilities whose required provider/service was not
yet available. The commands had to fail clearly and nonzero rather than appear
implemented or silently succeed.

## Design and acceptance

- Share one local implementation pattern without importing external code.
- Emit a stable diagnostic naming the unavailable provider.
- Reject unsupported options/operands and return failure.
- Exercise staged rootfs and QEMU command behavior.

Test ownership is recorded in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md), including the
deferred-stub host/rootfs/QEMU cases.

## Result and resumption

The temporary failure behavior was completed. Each stub remains compliance debt
until a later provider Phase replaces it and updates the WS001 ledger.

## Completion conditions

- Every selected provider-missing command installs in the staged image.
- Each command emits the documented diagnostic and exits nonzero.
- Host, rootfs, and QEMU deferred-stub cases pass.
