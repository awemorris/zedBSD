<!-- awesome-plan project=zedbsd record=ws328-p018 -->

# ws328-p018 paint の統合（新しい item・CPU の参照の描画・coverage の cache）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、統合なので推論が多い）。
- 依存: p005・p006・p007（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

- 始めに: WPT の該当の reftest（design.md §15 の directory）を CPU で流した数（基準）を結果に書く（main のこの Phase の前の commit で）。sources.mk の差分（design.md §13 の案 1）は Q1 経由でユーザーの承認を得てから当てる（承認の有無を最初に Q1 に確かめる）。
- design.md §7 の display list の追加: `PAINT_PATH`・`PAINT_RRECT`・`PAINT_SHADOW`、`struct paint_fill`・`struct paint_vector`（倍率 `scale_x`・`scale_y`、影の spread・offset と箱）、list の clip の表（`struct paint_vector_clip`、部品の和と親の鎖）、`paint_item.vector`、PAINT_CLIP の角丸（§7 の clip の規則）。
- item を作る関数（list.c と page から使う）: 例 `paint_add_path`・`paint_add_rrect`・`paint_add_shadow`・`paint_add_vector_clip`。名前と引数はこの Phase が決め、paint.h に仕様の comment を書く。rrect の辺は PAINT_RECT と同じく整数に丸めて入れる（§7「辺の丸め」）。
- 倍率: list の CSS の scale（今 x・y・width・height と glyph を倍にしている所）と view transition の写像（`list_vt_remap`）で、ベクトルの item は幾何でなく倍率を掛ける。
- 深い写し: `paint_transition_capture` と snapshot を list に戻す所で `item->vector`（path・stop・clip の表と部品）を写す。gradient の IMAGE の item が arena の bitmap を指す今の問題も同じ所で直す。
- coverage の cache の module（描画の thread、§4 の窓・予算・key の衝突の確かめ）を作り、software.c が使う（p026 の GPU も同じ module を使う）。
- software.c が新しい item を vector/ の関数で描く: PAINT_PATH は cache の coverage と clip の鎖（`vec_coverage_unite`・`vec_coverage_intersect`）、paint は色か ramp（`vec_gradient_ramp`・`vec_gradient_sample` を pixel の中心で）、PAINT_RRECT は `vec_rrect_coverage`・`vec_rrect_ring_coverage`・`vec_rrect_side_share`、PAINT_SHADOW は `vec_rrect_spread` と `vec_rrect_shadow` と箱の外（内）、角丸の clip を全ての item に。合成は今の software_blend（straight）と同じ規則。
- vulkan.c は新しい item をまだ描かない（p020・p026 まで飛ばす）。list はまだ新しい item を作らない（CSS・SVG の切り替えは p019 以後）ので、main の画面は変わらない。gpu_stage の最後の「それ以外は text」の分岐に新しい種類が落ちないようにする。
- `paint_dump` の新しい行（倍率・clip の番号を含む）、`scene.c`（arena の中だけを指す事、frame の移動で (x, y) だけ動かす）。
- `paint/paint.h` の file の comment の開発の管理情報（設計の文書への参照）を消す。

## 2. 試験

- `plan/ws328/tests/host-paint-vector.c`（WS074 の host の試験の形を倣う）: 各 item の dump、合成の item の CPU の画素（手の計算の角丸・ring・影の値、path の coverage を raster-reference と）、clip の鎖（2 つの部品で rule が違う和、親との積）、倍率、scene の作成と frame の移動、snapshot と復元を ASan で（arena を捨てた後に描く）、cache が scroll で作り直さない事（counter）。
- 既存の WS074 の host の試験（paint の dump・画素）が通り続ける（list はまだ新しい item を作らないので変わらない筈）。
- `sh plan/ws328/tests/run-unit.sh all`、check-skeleton（BASE）。

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
