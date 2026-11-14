<!-- awesome-plan project=zedbsd record=ws328-p020 -->

# ws328-p020 Vulkan のベクトルの pipeline

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: Opus（phase-runner、GPU）。
- 依存: p018・p019（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §8 の全部: ベクトルの pipeline（shader `paint/shaders/vector.vert`・`vector.frag`、`regenerate.py` で `shaders.h` に足す。host の glslc と spirv-val）、instance の詰め方（52 語以内、§8.1）、coverage の atlas と ramp の texture（§8.2、cache と LRU と足りない時の数え方）、shader の式（§8.3、vector/ の関数と同じ順と定数）、driver の制約（§8.4）、角丸の clip を矩形・glyph・画像にも（ベクトルの pipeline の矩形・glyph・画像の種類）。list の順を保ち、同じ pipeline の続く item をまとめて draw。

## 2. 試験

- host: `plan/ws074/tests/gpu-compare.py` で `plan/ws328/tests/pages/*.html` を lavapipe の GPU と CPU で比べる（channel の差 ≤ 2、超える pixel ≤ 0.1%）。cache の当たりと LRU（scroll で coverage を作り直さない事を数で確かめる: 試験の hook か dump の counter）。
- QEMU の Venus: Q1 が割り当てた QEMU（名前・port）で、同じ頁を `--render-gpu` と `--render` で描いて読み戻し比べる（WS074 の browser-guest.sh の形、1 本の script）。
- i915（5330）・V3D（Pi）・PowerVR（VF2）の実機は Q1 の割り当ての時に流す試験の一覧に載せる（未実施と書く）。

## 3. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の誤りは規約の見直しの Phase（p023）へ持ち越し、Q1 に報告する。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。
- frame ごとに CPU が画素を触らない（cache の miss の時の coverage と ramp の作成は内容が変わった時だけ）。

## 結果

（担当が書く。）
