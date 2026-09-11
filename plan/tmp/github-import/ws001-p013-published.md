<!-- awesome-plan project=zedbsd record=ws001-p013 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase013/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# WS001 Phase 013: bounded link and unlink correction

Last updated: 2026-08-25

Phase ID: `ws001-p013`

Status: complete

Parent: [WS001](https://github.com/awemorris/zedBSD/issues/2)

Tests: [WS001 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md)

## Objective and scope

Prove the small pathname utilities against actual host filesystem semantics:
operand counts, `--`, same-inode hard linking, existing/missing operands,
directory unlink rejection, diagnostics/status, and native builds. Cross-device,
permission, link-count exhaustion, injected I/O failure, and guest runtime are
retained for a later filesystem-failure matrix.

## Work packages

- [x] Add the missing `link --` option terminator.
- [x] Add a focused temporary-filesystem test for both utilities.
- [x] Format, run the host test, and build both native amd64 commands.
- [x] Update the compliance ledger with remaining proof debt.

## Completion conditions

The focused host test and native builds pass and the ledger remains
conservative about untested filesystem/guest failure behavior.

## Evidence and result

The focused test passes for hard-link identity, removal, option delimiters,
operand counts, existing/missing targets, and directory rejection. The amd64
native build passes. Cross-device, injected I/O, permission-matrix, and guest
runtime behavior remain explicitly outside this bounded Phase.
