<!-- awesome-plan project=zedbsd record=ws328-p017 -->

# ws328-p017 CSS の gradient の解析（radial・conic・repeating・transition hint）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low）。
- 依存: p001 だけ（vector/ に依らない。いつでも始めてよい）（依存の Phase が main に merge され cleared の後に始める）。

## 1. 読む物

- design.md §9・§9.1。今の解析: `userland/desktop/libbrowser/css/gradient.c`（linear と repeating-linear）、`css/css.h` の `struct css_gradient`、`css/values.c` の呼び出し（背景・list・shape-outside の 3 か所）、computed value の直列化（`bind/computed.c` などで gradient を文字列にする所を grep）、`layout/shape.c`（shape-outside が gradient の透明度を使う）。
- 仕様: CSS Images 3 §3.1〜§3.4（文法・既定・直列化の形）、CSS Images 4 §3.2.2（radial の大きさの keyword）・§3.3（conic）・§3.4（repeating-conic）・§3.5.1（stop の list と transition hint の文法）。`build/sources/specs/ws328/txt/`。

## 2. 作る物

- `struct css_gradient` の拡張（design.md §9.1 の意味の項目: 種類、radial の circle／ellipse・大きさの keyword か明示の大きさ・位置、conic の from の角度と位置、stop の hint。field の名前と詰め方はこの Phase が決め、結果に書く。CSS_GRADIENT_STOPS は 16 のまま）。
- `css/gradient.c` に radial-gradient()・repeating-radial-gradient()・conic-gradient()・repeating-conic-gradient() と hint の解析。conic の stop の位置は角度（deg・grad・rad・turn）か %。
- computed value の直列化（getComputedStyle）を新しい形に。
- 既存の linear の利用（`paint/list.c` の `list_gradient`、`paint/gradient.c`、`layout/shape.c`）が新しい field で壊れない事（kind が linear の時だけ今の道を通す。radial・conic は p025 まで描かない: 今の描画で無視されるのは今と同じ）。
- 試験: `plan/ws328/tests/host-css-gradient.c`（WS074 の host の試験の形を倣い、build は `plan/ws074/tests/host-build.sh` の作りを読んで同じ flag で。build の script が要れば `plan/ws328/tests/` に足す）。WS328 の試験は plan/ws328/tests/ に置く（design.md §12）。

## 3. 試験の項目

- 各関数の正しい形と既定（radial の既定は ellipse farthest-corner at center、conic の既定は from 0deg at center）、keyword の組み合わせ（`circle closest-side at 10px 20%`）、明示の大きさ（circle は長さ 1 つ・負で無効、ellipse は 2 つ・% 可）、hint（stop の間だけ、連続する hint・最初と最後の hint で無効）、二つの位置の stop、無効な形で宣言全体が無効。
- 直列化の round trip（CSS Images の直列化の形）。
- 既存の linear の試験（WS074 の host の試験の linear の物）が通り続ける。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- hint は CSS の並びの entry として `struct css_gradient_stop` の hint 1 で持ち、`CSS_GRADIENT_STOPS` は stop と hint を合わせて 32（design.md §9.1）。33 個目で宣言が無効か、切り詰めるかは今の 16 の時の扱いに合わせる（結果に書く）。

## 4. 注意

- 解析の誤りは宣言を落とす（CSS Syntax）。値の範囲（負の半径）は仕様の文法どおり。

## 5. 守る事

- AGENTS.md の §4・§7・§9・§10、plan/coding-style.md の全文、plan/guardrail.md の出典・開発の管理情報・「画像の処理は GPU で」。
- vector/・svg/ の header は変えない（基点の SHA、`BASE=<SHA> sh plan/ws328/tests/check-skeleton.sh`）。header の契約の誤りを見つけたら header を変えずに止めて Q1 に返す（design.md §11 の再凍結。p023 は書き方だけで契約の直しを持ち越さない）。
- Makefile・`mk/`・`sources.mk` の変更は Q1 経由でユーザーの承認の後（design.md §13 の差分）。HAL・toolchain は変えない。
- 削除（rm、`git rm`）は担当がしない。path と理由を Q1 に送る。
- 他の実装（Blink・WebKit・Gecko・Skia・resvg・librsvg など）の code・comment・構造を写さない。

## 6. 確かめの command

- `sh plan/ws074/tests/host-build.sh asan` の後 `build/ws074-host/asan/host-css-gradient`（と既存の gradient に関わる host の試験）。
- `git diff --check`。

## 7. 終わりの条件

- 試験が PASS、わざと壊す確かめ、規約の checklist、`git commit -m WIP -- <path>`、結果の節（自分で決めた field の名前と形）、ws.md の行。

## 結果

（担当が書く。）
