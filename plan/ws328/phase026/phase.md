<!-- awesome-plan project=zedbsd record=ws328-p026 -->

# ws328-p026 Vulkan の path（coverage の atlas・tile の表・cache と予算）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、GPU）。
- 依存: p020（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §4（cache の粒度と予算）と §8.2 の binding 1（coverage の atlas）・binding 3（tile の表）: p018 の coverage の cache の module を描画の thread の GPU の側に繋ぎ、path の item を bbox の quad 1 枚で描く（fragment が tile の表から slot を引いて atlas を読む）。窓（viewport ± 1 画面）を tile に揃えて作り、scroll で足りない部分だけ足す。1 frame の予算（交差の数）を測って決め、超えた path は次の frame に回す。足りない atlas の時の数え方と dump の統計。倍率の key（1/64 に量子化）。SVG の clip の鎖の coverage の cache。

## 2. 試験

- gpu-compare（lavapipe）で合成の path の item（塗り・線・dash・clip の鎖・倍率）の頁の GPU と CPU が合う事。
- cache の当たりの数（scroll で作り直さない）、予算を超える頁（大きな path を多数）で frame が止まらず、数 frame で揃う事。
- 全画面の複雑な path の頁で frame ごとの頂点の bytes を測り結果に書く（item の数に比例し tile の数に比例しない事）。
- 実機の一覧に Pi 4 の全画面の SVG の frame の時間を足す（未実施と書く）。

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
