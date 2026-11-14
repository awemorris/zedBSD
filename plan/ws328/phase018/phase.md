<!-- awesome-plan project=zedbsd record=ws328-p018 -->

# ws328-p018 paint の統合（新しい item と CPU の参照の描画）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p005・p006・p007（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

- design.md §7 の display list の追加: `PAINT_PATH`・`PAINT_RRECT`・`PAINT_SHADOW`、`struct paint_fill`・`struct paint_vector`、`paint_item.vector`、PAINT_CLIP の角丸（§7 の clip の規則、`paint_clips` の top に一番内側の角丸 1 つ）。
- item を作る関数（list.c と page から使う）: 例 `paint_add_path(list, x, y, lines, rule, fill, clip)`（lines を arena へ写し key の hash を計算）、`paint_add_rrect`、`paint_add_shadow`。名前と引数はこの Phase が決め、paint.h に仕様の comment を書く。
- software.c が新しい item を vector/ の関数で描く: PAINT_PATH は `vec_rasterize`（原点を整数に丸めた位相 0）と clip の `vec_coverage_intersect`、paint は色か ramp（`vec_gradient_ramp`・`vec_gradient_sample`）、PAINT_RRECT は `vec_rrect_coverage`・`vec_rrect_ring_coverage`・`vec_rrect_side_share`、PAINT_SHADOW は `vec_rrect_shadow` と箱の外（内）、角丸の clip を全ての item に。合成は今の software_blend（straight）と同じ規則。
- vulkan.c は新しい item を今は描かない（p020 まで飛ばす。dump と CPU で確かめる）。gpu_stage の最後の「それ以外は text」の分岐に新しい種類が落ちないようにする。
- `paint_dump` の新しい行、`scene.c`（arena の中だけを指す事、frame の移動で (x, y) だけ動かす）。
- sources.mk の差分（design.md §13）: Q1 経由でユーザーの承認を得てから当てる。承認前に始める時は、この Phase の最初に Q1 に承認の有無を確かめる。

## 2. 試験

- WS074 の host の試験に `host-paint-vector.c` を足す: 各 item の dump、CPU の画素（手の計算の角丸・ring・影の値、path の coverage を raster-reference と）、scene の作成と frame の移動。
- 既存の WS074 の host の試験（paint の dump・画素）が通り続ける（新しい item は p019 まで list が作らないので変わらない筈）。
- `sh plan/ws328/tests/run-unit.sh all`、check-skeleton（BASE）。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の誤りは規約の見直しの Phase（p023）へ持ち越し、Q1 に報告する。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。
- WS074 の試験の file を足す・直す事は Q1 の許可の範囲（design.md §12）。直した file を結果に全部書く。

## 結果

（担当が書く。）
