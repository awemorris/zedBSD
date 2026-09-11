<!-- awesome-plan project=zedbsd record=ws001-p000 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/phase000/phase.md`

親: [ws001](https://github.com/awemorris/zedBSD/issues/2)

# ws001-p000: protect the inventory and gates

WSID: `ws001`

Phase ID: `p000`

Status: complete

Completed: 2026-08-24

Parent WS: [WS001](https://github.com/awemorris/zedBSD/issues/2)

## Objective and scope

Establish the POSIX utility inventory, provider-state vocabulary, source/build
rules, and acceptance gates before adding commands. This Phase also protected
deferred commands from being mistaken for successful implementations.

## Design and acceptance

- Keep the machine-readable utility matrix authoritative for utility rows.
- Distinguish missing, deferred-provider, implemented-unreviewed, and reviewed.
- Require host, build/install, and QEMU evidence appropriate to each utility.
- Keep external implementations out of `userland/base`.
- Preserve correct failing tests and record unsupported behavior honestly.

Shared cases are indexed in [WS001 tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md). Original detail
is retained in the [legacy plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/history/phase000-010-legacy-plan.md).

## Result and resumption

Completed as part of the historical Phase 0–10 series. No interrupted work
remains; later inventory changes are owned directly by WS001 or a new Phase.

## Completion conditions

- The utility inventory and status vocabulary exist and are internally consistent.
- Build, provenance, host, and QEMU gate policy is documented.
- Deferred or missing commands cannot be mistaken for conforming success.
