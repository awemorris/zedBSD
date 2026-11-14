<!-- awesome-plan project=zedbsd record=ws328-p021 -->

# ws328-p021 SVG の統合: 木の adapter・継承の口・inline の `<svg>`

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p016・p026・p029（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §10 のうち: 木の adapter（`page/svg-tree.c` など、`parent` を含む）、HTML の祖先の computed style から `outer`（color・fill・stroke などの継承される性質、font-size）を作る口、inline の `<svg>` を `svg_picture_build` → `svg_picture_draw` → sink で PAINT_PATH の item に（clip の表を含む）、svg の content box の PAINT_CLIP で囲む（overflow が visible の時を除く）、picture の表（style の thread の物、作り直しの条件: subtree・外側の style・大きさ・参照した id の先の変化）。sources.mk の案 3 は U5 の承認の後。

## 2. 試験

- host の試験（`plan/ws328/tests/` に）: inline の SVG の dump（item・clip の表）、`fill="currentColor"` が頁の color で描かれる、slice の preserveAspectRatio で箱の外に出ない、DOM の変化と参照先の変化で picture が作り直される事。
- WPT の `svg/shapes/`・`svg/painting/`・`svg/pservers/`・`svg/coordinate-systems/`・`svg/paths/`・`css/css-masking/clip-path/` の reftest を CPU で、p018 の基準との比べ。
- gpu-compare（lavapipe）で SVG の頁の GPU と CPU が合う事。

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
