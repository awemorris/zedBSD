<!-- awesome-plan project=zedbsd record=ws328-p004 -->

# ws328-p004 vec-stroke（線の輪郭）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: 実装済み・試験 PASS（2026-11-15 P2 q2122、cleared は Q1）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p003（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/vector/vec-stroke.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§17 の D3〜D5。SVG 2 §13.5.3（幅）・§13.5.4（cap）・§13.5.5（join と miterlimit）・§13.5.6（dash の性質）・§13.5.7（線の形と dash の位置の算法）・§9.5.3（長さ 0 の subpath）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-stroke.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-stroke.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("stroke");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 出力の全ての contour が閉じ、正の符号付き面積（shoelace）。to_device が鏡映（scale(-1,1)）でも正。
- 面積の照合（出力を nonzero で塗った面積を、vec-raster に頼らず試験の中の独立の計算で: 例えば `raster-reference.h` の reference_coverage の合計 / 255 で）: 長さ L・幅 w の線分 butt = L·w、square = (L + w)·w、round = L·w + π(w/2)²（誤差は多角形の近似の分）。
- join: 90° の折れ線の miter（外側の角が (w/2)·√2 先）、miterlimit を下回ると bevel、round の扇形、180° の折り返し（round は半円、他は無し）、共線で join 無し。
- dash: SVG 2 §13.5.7 の算法の例を手で（[5,5] offset 0 の長さ 22 の線 → [0,5],[10,15],[20,22]）、奇数の配列の繰り返し、負の offset、和 0 で dash 無し、閉じた contour の始めと終わりの dash の継ぎ、pathLength の dash_scale、VEC_DASH_PIECES_MAX で止まる。
- 長さ 0 の subpath: round で円盤、square で x 軸に揃った正方形、butt で無し。
- 曲線の内側の頂点（CORNER の無い）は round の join。
- style の範囲の外（幅 0・負・NaN、miter_limit < 1、dashes が NULL で count > 0、count > VEC_DASHES_MAX）で EINVAL、失敗で out が戻る。
- 乱数の折れ線と style で ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- moveto だけの contour（頂点 1 つ、closed 0）は何も描かない（SVG 2 §13.5.1）。頂点 1 つで closed 1、全ての線分が長さ 0 の contour は cap だけ。
- dash_scale: 長さと offset に掛けてから並べる（pathLength = 80 で長さ 40 の線に dash [10] は実長 5 の dash: dash_scale 0.5）。
- 長さ 0 の線分を挟む頂点の join は前後の長さのある線分で決まる（NaN が出ない）。
- `VEC_DASH_PIECES_MAX` に達した後の残りは実線（最後の dash の始まりから contour の終わりまで）。
- 出力の各 contour の shoelace の和が正（y 下向きで画面の上で時計回り）。

## 4. 注意

- SVG 2 §13.5.7 の本文は「within distance stroke-width」と書くが、§13.5.3 と線の幅の意味から半分（width / 2）が正。header のとおり width / 2 で書く。
- round の多角形の段の数は header の式（M の上限 256）。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh stroke` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。check 6（zedBSD の clang と x86_64 の sysroot での compile）は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を使う。無ければ、Q1 が起動の依頼文で許した時に `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk sysroot` で自分の build/ に入れる（共有の toolchain と main の build/ は使わない・書かない。許しが無ければ Q1 に頼む）。最後の行が `check-skeleton: INCOMPLETE` の時は PASS ではない。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

2026-11-15 02:40（host の `date`）、P2（q2122-i01）。実装: `userland/desktop/libbrowser/vector/vec-stroke.c`（`vec_stroke`・`vec_contour_length` と static 関数）、試験: `plan/ws328/tests/unit/test-stroke.c`。header・他の module・試験の道具・Makefile は変えていない。

### 実行した command と最後の行

- `sh plan/ws328/tests/run-unit.sh stroke`（cc）: `test-stroke: PASS (28392433 checks)`、`run-unit: PASS`。
- `CC=clang sh plan/ws328/tests/run-unit.sh stroke`: `test-stroke: PASS (27637132 checks)`、`run-unit: PASS`（件数が違うのは乱数の run の中の分岐が浮動小数点の丸めで変わるため）。
- `sh plan/ws328/tests/run-unit.sh all`: test-base 204220・test-flatten 93774・test-harness 1127・test-path 348081・test-stroke 28392433、`run-unit: PASS`。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: `check-skeleton: PASS`（check 6 は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` で実施。`make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `sysroot`）。
- `git diff --check`: 出力なし。

### 試験の内容

contour の長さ（開・閉・範囲外）、butt・square・round の cap（union の面積を `raster-reference.h` の coverage で: 80・96・80 + 4π、斜めの線の cap の頂点位置）、直角の join（右折・左折、miter 80・bevel 78・round 76 + 多角形の扇形、miter limit が √2 の前後）、折り返し 180 度（round は半円、他は無し）、共線（join 無し）と 0.1 rad・1e-7 rad の境、鋭角 150 度の miter 点の距離（2 / sin 15° = 7.7274）と limit 3.8・3.9、弧の段の数（式、to_device の倍率、tolerance ≥ r、上限 256）、to_device（鏡映・点対称・非等方・平行移動、線は写像の前に太らせる）、dash（SVG 2 §13.5.7 を手で: [5,5] の長さ 22、奇数、offset 3・-3・-13・5・23、和 0、dash_scale と offset の倍率、[4,2,1] の長さ 30）、dash の cap と長さ 0 の dash（点）、dash が角を跨ぐ・角で止まる、閉じた contour（loop の join、全体を覆う dash、始めと終わりの継ぎ、継がれない場合、継がれない零長の dash）、`VEC_DASH_PIECES_MAX`（65536 個目が contour の終わりまで）、長さ 0 の subpath（円盤・軸に揃った正方形・butt で無し・moveto だけは何も描かない）、長さ 0 の線分を挟む join、頂点の flags 無し（曲線の内側）は round、style の範囲外 22 通りと matrix・範囲外の頂点・壊れた lines の EINVAL と out の巻き戻し、`VEC_LINES_VERTICES_MAX` での ENOBUFS と巻き戻し、乱数 4000 本（出力の全 contour が閉じ、頂点 3 以上、正の shoelace、flags 0、dash 無しなら線分の中点の巻き数が非 0）、極端な値（幅・dash・offset・scale・座標が 1e-30〜3e38）1200 本。

### わざと壊す確かめ

実装を git の外で 41 通りに書き換え（符号・境界の比較・定数・分岐の削除。miter limit の境、外側の向き、square cap の長さ、dash の offset・倍率・奇数の反復・和 0・最後の dash の延長・継ぎ、向きの反転、moveto だけ、曲線の join、segment の境、弧の段、幅、join の閉じ、倍率、閉じた線分の長さ、巻き戻し、零長の向き、cap の向きと位置、範囲の検査など）、毎回 `sh plan/ws328/tests/run-unit.sh stroke` を流した: 40 通りが FAIL、1 通りだけ PASS（「x の座標の範囲の検査を外す」: 出力の vec_lines_begin・add が同じ検査をするので動作が同じ、等価）。1 回目の確かめで見逃した 4 通り（segment の境、共線の閾値、和 0 の dash を round で、零長の dash の向き、継ぎの零長、単独の dash）は試験を足して塞いだ。ほかに等価・無害と判断した変更が 2 通りあった（共線の閾値を 0 にする: 面積 0 の piece は捨てるので出力が同じ。atan2 の引数を 1e-3 ずらす: 面積の差が許容の内）。壊した後は `cmp` で元の file と一致を確かめ、`git diff` が空に戻った。

### 自分で決めた点（header が黙っている所）

1. 内部の計算は double、出力の頂点だけ float（header の「単精度」は値の受け渡しの形と解釈）。出力の座標が範囲外なら EINVAL（float に切る前に double で検査）。
2. 面積が 0 の piece（退化した三角形、行列が潰す物）は出力しない（「正の面積」の不変条件のため）。向きは写像後の float の頂点の shoelace（最初の頂点からの相対）で決め、負なら逆順。
3. offset が dash の境（例: [5,5] の offset 5）にぴったりの時は §13.5.7 を文字どおり、始めに長さ 0 の dash を作る（round・square の cap で点が出る）。
4. 長さ 0 の dash（`<p, p>`）は 2 つの cap を 1 つの円盤（round）か正方形（square）にし、向きは p から始まる線分（無ければ最後の長さのある線分、それも無ければ +x）。長さ 0 の subpath（contour 全体の長さ 0）にも dash は適用する（最初の項が dash なら点、gap なら何も無し）。
5. 閉じた contour で始めと終わりの dash を 1 つに継ぐ条件は、最初の dash が長さを持ち（end > 0）、最後の dash が contour の終わりに届く時。最初が長さ 0 の dash なら継がない。dash が 1 つで `<0, L>` なら loop（全部の頂点で join、cap 無し）。長さ 0 の閉じた contour は loop にしない。
6. 長さ 0 の線分を挟む join は 1 つ。VEC_VERTEX_CORNER はその間の頂点のどれかが持てば style の join、全部持たなければ round。
7. miter の判定は `(1 + cos 転向) * miter_limit^2 >= 2`（比 ≤ limit なら miter、等しければ miter）。180 度の折り返しの miter・bevel は面積 0 で捨てる。
8. round の弧の段数は header の式。丸め誤差の保険に、半円は 2 段以上・円盤は 3 段以上。
9. `center == out` と NULL の引数は EINVAL。`dash_count == 0` なら `dashes` は NULL でなくてもよい。
10. 閉じた contour の dash が継ぎ目を跨ぐ時は、長さを 2 周分まで展開して処理（1 つの dash の中の join は同じ関数）。
11. `vec_contour_length`: 範囲外の contour・配列に収まらない contour・長さが有限でない場合は 0。閉じた contour は閉じる線分を含む。

### header への疑問（変更はしていない）

- vec-stroke.h は「offset が dash の境にぴったりの時」に何も書かない。§13.5.7 の文字どおりに長さ 0 の dash を作ったが、cap が butt でない時に始めに点が出るのは意図と違うかもしれない（実在の描画は dash が offset で既に終わっていれば、その dash の終端の cap のみを描く）。Q1 の判断が要る。
- 「閉じた contour の始めの dash が 0 から始まり終わりの dash が長さで終わるなら 1 つの dash」は、最初の dash が長さ 0 の時を決めていない（上の 5 で継がないとした）。
- 実装の `VEC_LINES_VERTICES_MAX` の comment の 8 MiB の件は p003 から持ち越し（動作に関わらない）。

### 未実施

- QEMU・実機での確認（この Phase は host の単体試験だけ）。
- rasterizer（vec-raster）との結合の確認（p005 以降）。
- 大きな contour（頂点 2^21）での時間の測定。
