<!-- awesome-plan project=zedbsd record=ws328-p019 -->

# ws328-p019 CSS の装飾を新しい item で（角丸・border・影・gradient の背景・角丸の clip）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: Opus（phase-runner）。
- 依存: p004・p017・p018（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §9 の表の全行を `paint/list.c` で: 角丸の背景（background-clip を含む）、border（同じ色の ring、辺ごとの色の 4 ring、dashed・dotted を stroker で、double・groove・ridge・inset・outset）、box-shadow（外と inset、spread、knockout）、gradient の背景（linear・radial・conic・repeating、tile の繰り返し）、overflow の角丸の clip、置換要素の角丸。

- 角丸のない箱も ring・影は新しい item で描く（同じ式で角丸 0）。今の `list_round_fill`・`list_round_row`・`list_round_span`・`list_round_borders`・`list_shadow`・`list_grow_shape`・`list_shadow_quantiles`・`list_gradient` と `paint/gradient.c` は使われなくなる。関数は消し、file（`paint/gradient.c`）と sources.mk の行の削除は path と理由を Q1 に送る（`git rm` は Q1 の OK の後）。
- dashed・dotted の dash の長さ、groove などの明暗の式は CSS Backgrounds 3 §3.2 が UA に任せる所: この Phase が決めて list.c の comment と結果に書く。
- `layout/shape.c`（shape-outside の gradient）は linear のまま動く事を確かめる（radial・conic の shape-outside は範囲の外として結果に書く）。

## 2. 試験

- WS074 の host の試験の dump と画素の期待の値で変わる物を直す（理由を各 file の結果に）。新しい頁 `plan/ws328/tests/pages/*.html`（角丸・border の各 style・影・各 gradient・角丸の clip）を CPU（`browser --render`）で描き、手で確かめた画像を `build/review/` に置いて Q1 に示す。
- WPT（`plan/ws074/tests/fetch-suites.sh wpt`、`run-wpt-reftests.py`）の `css/css-backgrounds/`（border-radius・box-shadow・border-style）と `css/css-images/`（gradient）の reftest を CPU で、変更の前と後の数。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の誤りは規約の見直しの Phase（p023）へ持ち越し、Q1 に報告する。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。

## 結果

（担当が書く。）
