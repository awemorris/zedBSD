<!-- awesome-plan project=zedbsd record=ws001-p008 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase008/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p008: SCCS suite

WSID: `ws001`

Phase ID: `p008`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Implement the planned SCCS command set and shared file/history logic using
local source and deterministic fixtures.

## Design and acceptance

- Centralize SCCS file parsing, validation, locking, and atomic updates.
- Exercise create/update/extract/diff flows and malformed histories.
- Verify commands as standalone base packages and in Phase 8 QEMU.
- Record unimplemented administrative/history semantics in WS001.

Shared fixtures and scripts are indexed in
[WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The planned suite milestone completed. Future SCCS conformance corrections are
new Phases selected from the ledger.

## Completion conditions

- The selected SCCS commands and shared history logic build and install.
- Create, update, extract, diff, locking/atomicity, and malformed-history cases pass.
- Phase 8 host, build, and QEMU gates pass with open semantics recorded.
