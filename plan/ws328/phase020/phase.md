<!-- awesome-plan project=zedbsd record=ws328-p020 -->

# ws328-p020 Vulkan のベクトルの pipeline（rrect・ring・影・gradient）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、GPU）。
- 依存: p018（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §8 のうち: ベクトルの pipeline（shader `paint/shaders/vector.vert`・`vector.frag`、`regenerate.py` で `shaders.h` に足す。host の glslc と spirv-val）、instance の詰め方（52 語以内、§8.1、影は箱と spread・offset）、ramp の texture（binding 2、key と衝突の確かめ、LRU、256 行を超えた時の平均の色）、shader の式（§8.3: vector/ の関数と同じ順と定数、`vec_atan2` の多項式、`exp2` の形）、driver の制約（§8.4: atan・exp を使わない、loop は定数回、影の早い抜け）。vulkan.c が PAINT_RRECT・PAINT_SHADOW と gradient の paint を描く（PAINT_PATH は p026）。list の順を保ち、同じ pipeline の続く item をまとめて draw。

## 2. 試験

- host: `plan/ws074/tests/gpu-compare.py` で合成の item の頁（p018 の host の試験の item を描く頁、`plan/ws328/tests/pages/`）を lavapipe の GPU と CPU で比べる（channel の差 ≤ 2、超える pixel ≤ 0.1%）。
- 新しい SPIR-V を zedBSD の shader compiler に通す: host で build できる物（共通の compiler、PowerVR の compiler）で断られない事を確かめる 1 本の script（`plan/ws328/tests/` に）。host で build できない物は実機の一覧へ。
- 実機の一覧（Q1 の割り当ての時に流す、未実施と書く）: i915（5330）・Pi 4（V3D 4.2: 全画面の影 1 枚の frame の時間）・Pi 5・VF2（PowerVR）。

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
