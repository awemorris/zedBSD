<!-- awesome-plan project=zedbsd record=ws001-p007 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase007/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p007: compression utilities

WSID: `ws001`

Phase ID: `p007`

Status: complete milestone

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Add the planned compression/decompression utilities with local source,
round-trip behavior, malformed-stream handling, and package installation.

## Design and acceptance

- Bound dictionary/table/input sizes and reject corrupt streams safely.
- Verify deterministic round trips and documented format compatibility.
- Run host fixtures, Phase 7 QEMU tests, standalone installs, and top build.
- Leave unsupported format variants as explicit ledger items.

Cases and fixtures are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md).

## Result and resumption

The implementation milestone completed without claiming every historical
format/option combination reviewed.

## Completion conditions

- Selected compression and decompression commands build and install locally.
- Round-trip, corrupt-stream, bounds, and documented interoperability cases pass.
- Phase 7 host, build, and QEMU gates pass.
