<!-- awesome-plan project=zedbsd record=ws328-p017 -->

# ws328-p017 CSS の gradient の解析（radial・conic・repeating・transition hint）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（Q1 の受け入れ待ち。2026-11-15 P8 が実装・試験 PASS）
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

2026-11-15 P8（q2128-i01、worktree `p8-ws328p017`、基点 main 7f0fdcfd2b、header の基点 57bf94e62f）。vector/・svg/ の header は変えていない。Makefile・`mk/`・`sources.mk` も変えていない（`css/gradient.c` は既に一覧にある）。

### 作った物（file）

- `userland/desktop/libbrowser/css/css.h`: `struct css_gradient` の拡張（下の「field の名前と形」）。
- `css/gradient.c`: 6 つの関数（linear・radial・conic と repeating の 3 つ）の解析、hint、radial の形・大きさ・位置、conic の from・位置、`css_gradient_function()`（関数名の判定）、`css_gradient_color_stops()`（hint を除いた写し）。
- `css/values.c`・`css/internal.h`: 関数名の判定を `css_gradient_function()` に（背景・background の shorthand・shape-outside の 3 か所）。
- `css/cascade.c`: `cascade_gradient` が em を px に、px にページの zoom を掛け、radius・at の em も px にする。
- `bind/computed.c`: getComputedStyle の background-image が gradient を直列化する（`computed_gradient` ほか）。
- `layout/shape.c`・`paint/gradient.c`・`paint/list.c`: 既存の linear の道を kind が linear の時だけにし、hint を除いた stop だけを読む（radial・conic は今と同じく描かない）。
- 試験: `plan/ws328/tests/host-css-gradient.c`、`plan/ws328/tests/host-css-gradient.sh`（host-build.sh が作ったエンジンの object に link して流す）、`plan/ws328/tests/pages/css-gradient.html`。

### field の名前と形（design.md §9.1 の案との違いを含む）

- `enum css_gradient_kind { LINEAR, RADIAL, CONIC }`（LINEAR が 0）。`struct css_gradient`: `kind`、`angle`（linear の角度、conic の from）、`corner`、`repeating`、`circle`（radial、1 が circle）、`extent[2]`（radial の横・縦の大きさの keyword。`enum css_gradient_extent`: NONE（明示の大きさ）・CLOSEST_SIDE・CLOSEST_CORNER・FARTHEST_SIDE・FARTHEST_CORNER。circle は `extent[0]` だけ）、`radius[2]`・`at[2]`（`struct css_gradient_length`）、`count`、`stops[CSS_GRADIENT_STOPS]`。全 field が 4 byte で padding は無い（`cascade_keep` の memcmp のため）。
- `struct css_gradient_length { float percent; float pixels; float ems; }`: 「percent% ＋ pixels ＋ ems」の和。`at right 10px` は {100, -10, 0}、`2em` は {0, 0, 2}。案の `struct css_length` を使わなかったのは、解析の時点で font size が無く em を持てないため（cascade が em を px にして ems を 0 にする）。
- `struct css_gradient_stop { color; position; unit; hint }`。`hint` が 1 の entry は色の無い transition hint。unit に `CSS_GRADIENT_AT_DEGREES` を足した（conic の角度。保持は度）。案の `position2`・`unit2` は持たず、2 つの位置の stop は今までどおり同じ色の 2 entry に展開する（仕様の「2 つの stop と等価」と同じで、既存の利用が変わらず、fixup の入力の形でもある）。
- `CSS_GRADIENT_STOPS` は 32（stop と hint の entry の合計）。33 個目で宣言全体が無効（今の 16 の時の扱いと同じ）。

### 自分で決めた点（header が黙っている所。仕様を文字どおりに）

1. stop は 1 つで有効（CSS Images 4 §3.5.1 の `#?`）。linear にも及ぶ（今までは 2 つ未満が無効）。既存の消費者（paint/gradient.c・shape.c）は 1 つでも壊れない事を読んで確かめた。
2. 先頭の引数が geometry か stop かは、geometry の解析が通るかで決める（今の linear と同じ）。通らなければ stop として解析し、色でなければ hint、それも違えば無効。
3. hint: 直前が色の stop の時だけ（先頭・連続・最後は無効）。linear・radial の hint は length-percentage、conic は angle-percentage か 0。linear の stop に角度、conic の stop に長さは無効。
4. radial の大きさ（Images 3 §3.2.1 と Images 4 §3.2.2）: 形を省くと、長さ 1 つなら circle、それ以外は ellipse。circle の大きさは keyword 1 つか length（Images 4 により percentage も可）1 つ、2 成分は無効。ellipse は keyword 1 か 2 つ、または length-percentage 2 つ。`ellipse 10px`（1 つ）は無効。keyword と長さの混在は無効。負の半径は無効。位置は CSS Values 3 §7.3（1・2・4 値、3 値は無効）。
5. 単位は今の stop と同じ px・em・%・0 だけ（rem・pt・vw・calc() などは解析しない = 無効）。角度は deg・grad・rad・turn と 0。`in srgb` などの color interpolation method は解析せず、その gradient は無効。
6. 直列化（Images 4 §8 の「意味を変えず省ける物を省く」）: 既定の geometry（to bottom、ellipse farthest-corner at center、from 0deg at center）と、先頭の stop の 0（どの単位でも）、最後の stop の 100%（conic は 360deg も）を省く。色は rgb()・rgba()（currentcolor の印は `currentcolor`）。位置は長さと百分率（`at left top` は `at 0px 0px`）。far edge からの offset は `at right 10px bottom 20px` の 4 値の形（calc() の形は parse し返せないため）。circle の語は、length 1 つで言える時（`10px`）に省く。2 つの位置の stop は 2 entry で書く（`red 10% 20%` は `rgb(255, 0, 0) 10%, rgb(255, 0, 0) 20%`）。
7. px の位置・半径・中心に zoom を掛けた（`engine->zoom_eff`、他の length と同じ。今までの stop の px は zoom を掛けておらず、getComputedStyle の px が zoom で割られて合わなかった）。linear の既存の動作の変更はこの 1 点だけ。
8. stop の `currentcolor` は今までどおり印（0x00000001）のまま cascade が解決しない（この Phase の範囲外。直列化は `currentcolor` と書く）。

### 実行した確かめ（2026-11-15）

- build（warning 0）: `cc`・`clang` で `-std=gnu11 -Wall -Wextra -Werror`（host-build.sh の flag）と `-std=c11 -Wpedantic` の両方、変えた 7 file（gradient.c・cascade.c・values.c・computed.c・shape.c・paint/gradient.c・paint/list.c）を `-fsyntax-only` と object で。
- `BROWSER_HOST_BUILD=build/ws328-host CC=cc sh plan/ws074/tests/host-build.sh asan` の後 `sh plan/ws328/tests/host-css-gradient.sh asan`: `css gradient: 223/223 passed`（cc、ASan・UBSan）。
- `LD_LIBRARY_PATH=/usr/lib/llvm-19/lib/clang/19/lib/linux BROWSER_HOST_BUILD=build/ws328-host-clang CC=clang sh plan/ws328/tests/host-css-gradient.sh asan`: `css gradient: 223/223 passed`（clang、ASan・UBSan）。
- 試験の中身: 宣言 114 本（各 kind の正しい形と既定、keyword の組み合わせ、明示の大きさ、hint、2 つの位置、zoom、background の shorthand、無効な形で宣言全体が無効 = `none`）の getComputedStyle の値を、仕様から手で書いた期待の文字列と比べ、正しい物はその直列化をもう一度解析して同じ文字列になる事（round trip）。解析した field の確認（既定値・em の保持・hint の entry・2 つの位置・`css_gradient_color_stops`）。stop 31・32・33 個と、hint を含む 31・33・35 個の境。実ページの layout と paint（linear の gradient だけが IMAGE、radial・conic・shape-outside の gradient で壊れない）。
- 既存の host 試験（asan、cc）: host-canvas-background・host-css-numbers 28/28・host-cssom 12/12・host-css-rule-model 51/51・host-hsl-color 14/14・host-color-media 39/39・host-cssom-mutation 12/12 が PASS。host-tall-image は GPU driver（lavapipe）由来の leak 報告（未解決の module）。今回の変更との関係は未確認。
- わざと壊す確かめ（git の外、object だけ差し替え）: 43 通り（gradient.c 27・computed.c 12・cascade.c 4。既定の角度・stop 数の下限・hint の位置の検査・既定の大きさ・形の検査・負の半径・offset の向き・2 keyword の入れ替え・from の位置・単位の換算・上限・repeating・直列化の省略の各条件・位置の形・em と zoom の換算）で 42 が FAIL（うち 4 は ASan の crash）。残り 1（hint を足す時の `count + 1 > STOPS` の上限を緩める）は、境を越えた後は必ず末尾が hint で無効になるので結果が変わらない等価の変異（配列の範囲の保護だけの条件）。壊した後は元の object に戻して全 PASS を確認。
- `git diff --check`: 出力無し。`BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: 検査 1〜5 は FAIL 無し、検査 6 は worktree に zedBSD の toolchain・sysroot が無いため未実施で最後の行は `check-skeleton: INCOMPLETE`（vector/・svg/ は変えていない）。

### 未実施・残り

- check-skeleton の検査 6（zedBSD の toolchain での build）、plain・tsan の host build、arm64・riscv64 の build、QEMU・実機、WPT（css/css-images）の reftest は未実施。
- radial・conic の描画は p025（今は描かない）。hint を描画が honor するのも p025（今の linear の描画は hint を除いて描く）。
- 規約の見直しの Phase へ: 新しい code は既存の gradient.c の書き方に合わせ、`css_ident_equal` を条件の中で呼んでいる（規約 §6 の「条件の中で関数を呼ばない」に厳密には合わない）。
