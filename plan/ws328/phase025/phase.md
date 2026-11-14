<!-- awesome-plan project=zedbsd record=ws328-p025 -->

# ws328-p025 CSS の gradient の背景（linear・radial・conic・repeating、tile、退化）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p017・p019（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §9 の gradient の行: p017 の `struct css_gradient` から `struct vec_gradient` を作る（CSS Images 3 の gradient line の長さと角、radial の大きさの keyword の解決、楕円は行列で円に、conic の from と位置）、stop の位置の解決（長さと % を線の上の割合に）と hint の entry、`vec_gradient_fixup_css`・`vec_gradient_extend_span`・`vec_gradient_repeat_span`・退化の `vec_gradient_average`、退化した radial（CSS Images 3 §3.2.3 の文を読み場合ごとに）、background-repeat・size の tile の周期（item 1 つ）。`paint/gradient.c`（CPU の bitmap）を使わなくし、file の削除は `git rm` の一覧を Q1 へ、sources.mk の案 2 は U5 の承認の後。list-style・shape-outside（`layout/shape.c`）が gradient を使う所も新しい形に合わせる。

## 2. 試験

- host の試験: 各種の gradient の dump と CPU の画素（手の計算の t と色）、0% より前・100% より後の stop、repeating の退化、半径 0 の radial、tile の繰り返し。
- WPT の `css/css-images/` の gradient の reftest を CPU で、p018 の基準との比べ。
- gpu-compare（lavapipe）で gradient の頁の GPU と CPU が合う事。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`、check 6 の toolchain は module の phase.md §6 と同じ）。header の契約の誤りを見つけたら header を変えずに止めて Q1 に返す（design.md §11 の再凍結。p023 は書き方だけで契約の直しを持ち越さない）。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分、user-questions の U5）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。
- WS074 の host の試験の期待の値を直す事は Q1 の許可の範囲（design.md §12）。直した file を結果に全部書く。WS328 の新しい試験は `plan/ws328/tests/` に置く。
- QEMU の console・serial の log で判定しない（boot-test.sh、screendump、gdbstub・QMP）。
- frame ごとに CPU が画素を触らない（cache の miss の時の coverage と ramp の作成は内容が変わった時だけ、§4 の予算の中）。

## 結果

（担当が書く。）
