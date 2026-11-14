<!-- awesome-plan project=zedbsd record=ws328-p028 -->

# ws328-p028 `<img>` の SVG・背景の SVG、page/svg.c の rasterizer を消す

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p021（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §10 のうち: `<img src=*.svg>` を表示の大きさの picture から PAINT_PATH に（画像の cache が picture を持つ）、CSS の背景・list-style などの画像としての SVG は bitmap の道（`svg_picture_render`、大きさが変わった時に作り直す、`page_svg_decode` の口を保つ）。`page/svg.c` の自前の rasterizer（svg_fill・svg_stroke など）を消して薄い口にする（消す code は path と理由を Q1 へ）。

## 2. 試験

- host の試験: `<img>` の SVG の dump、背景の SVG の bitmap の画素（`svg_picture_render` の結果と CPU の参照）、大きさが変わった時の作り直し。
- 既存の WS074 の SVG の host の試験の期待の値で変わる物を直し、file を全部書く。
- WPT の SVG の reftest の数（p021 の後の数との比べ）。

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
