<!-- awesome-plan project=zedbsd record=ws328-p023 -->

# ws328-p023 規約の全文の見直し

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: conformance-reviewer（Sonnet）。
- 依存: p022（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

- WS328 が足した・変えた C（`vector/`・`svg/`・`paint/` の変更・`page/` の変更・`css/` の変更）を plan/coding-style.md の全文と照らし、書き方だけを直す（動作は変えない）。
- header の持ち越し（実装の Phase で見つかった header の comment の誤り・足りない所）を直す。header を変えるので、直した後の SHA を新しい基点として結果に書く。
- 出典の comment の形（guardrail の「Values referenced:」、複数行）と、source に開発の管理情報が無い事。

## 2. 確かめ

- `sh plan/ws328/tests/run-unit.sh all`（cc と clang）、`sh plan/ws328/tests/check-skeleton.sh`（BASE 無し）、WS074 の host の試験、`git diff --check`。

## 結果

（担当が書く。）
