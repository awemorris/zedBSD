<!-- awesome-plan project=zedbsd record=ws002-p011 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase011/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p011: service foundation and administrative layout

WSID: `ws002`

Phase ID: `p011`

Status: complete

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Freeze service/rc.conf parsing, declarative `/etc/service.d` records, runtime
paths, `/bin` versus `/sbin` classification, and independent package Makefiles
before PID 1 depends on them. Configuration is parsed as data and never sourced
as shell code.

## Acceptance and result

Parser/path/install host gates and the top-level build passed as part of the
completed WS002 baseline. Exact original scope and transitions remain in the
[legacy plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/history/phase011-019-legacy-plan.md); shared cases are indexed
in [WS002 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/tests/README.md).

## Interruption record

Not interrupted. Later parser defects are new WS002 maintenance or a consumer
WS Phase; they do not reopen this historical record silently.

## Completion conditions

- rc.conf and service-definition parsers reject malformed/unsafe input predictably.
- Administrative paths and `/bin` versus `/sbin` placement are fixed and tested.
- Relevant base packages honor the documented standalone build/install contract.
