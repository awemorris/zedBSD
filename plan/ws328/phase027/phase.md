<!-- awesome-plan project=zedbsd record=ws328-p027 -->

# ws328-p027 角丸の clip の下の矩形・glyph・画像、pipeline の切り替えの計測、QEMU の Venus

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成、2026-11-15 design-reviewer の反映で改訂）
- 担当: Opus（phase-runner、GPU）。
- 依存: p026（依存の Phase が main に merge され cleared の後に始める）。

## 1. 範囲

design.md §7 の角丸の clip を今の display の pipeline の矩形・glyph・画像にも掛ける（ベクトルの pipeline の矩形・glyph・画像の種類で描くか、今の pipeline に clip の半径を渡すかをこの Phase が決める。決め手は §8.1 の頂点の data の量と pipeline の切り替えの数）。文字とベクトルが交互の頁で pipeline の切り替えの数と時間を測り dump の統計に出す。QEMU の Venus で GPU の描画全体（p020・p026・この Phase）を確かめる。

## 2. 試験

- gpu-compare（lavapipe）で角丸の clip の中の矩形・文字・画像の頁。
- QEMU の Venus: Q1 が割り当てた QEMU（名前・port）で、`plan/ws328/tests/pages/` の頁を `--render-gpu` と `--render` で描いて読み戻し比べる 1 本の script（WS074 の browser-guest.sh の形、配置 → 起動 → 実行 → 回収 → 要約）。
- これが main に入ると CSS・SVG の切り替え（p019・p024・p025・p021・p028）を始めてよい（design.md §11）。

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
