<!-- awesome-plan project=zedbsd record=ws334-p004 -->

# ws334-p004 設計の確定: design-reviewer と基点の SHA

- 親: [WS334](../ws.md)。手順 docs/agent/spec-module-delegation.md の §4.7・§4.8。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p003。

## 目的

p002・p003 の header・design.md・全部の phase.md を design-reviewer 1 本に敵対的に読ませ、指摘を全部 design.md の表に記録し、反映するか理由を付けて見送る。header を確定させた commit を基点の SHA として ws.md の Resume point と全部の phase.md に書き、`BASE=<基点> sh plan/ws334/tests/check-skeleton.sh` が PASS。Q1 に、起動できる実装の Phase の一覧（順と並べ方）を返す。
