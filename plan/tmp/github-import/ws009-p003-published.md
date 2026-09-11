<!-- awesome-plan project=zedbsd record=ws009-p003 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws009/phase003/phase.md`

親: [ws009](https://github.com/awemorris/zedBSD/issues/10)

# ws009-p003: init and service reference

WSID: `ws009`  
Phase ID: `p003`  
Combined ID: `ws009-p003`  
Status: complete  
Parent WS: [WS009](https://github.com/awemorris/zedBSD/issues/10)

## Objective

Publish the current native init and service-management contract without
presenting planned dependency-aware shutdown or definition reload as complete.

## Completion result

- [x] Boot, mount, hostname, and configuration ownership are described.
- [x] `rc.conf` and `service.d` grammars and limits are recorded.
- [x] FD 3 readiness, service commands, restart behavior, and shutdown are
  documented from the current implementation.
- [x] Current gaps (`required`, shutdown order, definition reload) are explicit.
- [x] Documentation navigation and relative-link validation pass.

## Evidence

Run DOC-T00 from the [shared test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws009-documentation/tests/README.md). Behavioral source
anchors are linked from the published
[reference](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/docs/reference/init-services.md).
