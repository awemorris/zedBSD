<!-- awesome-plan project=zedbsd record=ws328-p021 -->

# ws328-p021 SVG の統合（inline・`<img>`・背景の SVG、page/svg.c の置き換え）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: Opus（phase-runner）。
- 依存: p016・p018（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §10 の全部: DOM の上の `struct svg_tree`（`page/svg-tree.c` など。名前・属性の UTF-8・id の表・css の色の parser）、inline の `<svg>` と `<img>` の SVG を picture → `svg_picture_draw` → PAINT_PATH の item（group の opacity は alpha に掛ける近似、§7）、picture の保持と作り直し（DOM の変更で）、CSS の背景などの画像としての SVG は `svg_picture_render` の bitmap、`page_svg_render`・`page_svg_decode` の口を保つ。`page/svg.c` の自前の rasterizer の関数は消す（sources.mk の追加の差分は Q1 経由で承認）。

## 2. 試験

- WS074 の host の試験の SVG に関わる物（`host-svg-*`、images の試験）を通し、変わる期待の値を直す。
- 頁 `plan/ws328/tests/pages/svg-*.html`（図形・線・dash・join・gradient・clip・use・viewBox）を CPU と GPU（p020 の後なら）で。
- WPT の `svg/shapes/`・`svg/painting/`・`svg/pservers/`・`svg/coordinate-systems/`・`svg/paths/`・`css/css-masking/clip-path/` の reftest（SVG の clipPath）の前と後の数。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の誤りは規約の見直しの Phase（p023）へ持ち越し、Q1 に報告する。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。

## 結果

（担当が書く。）
