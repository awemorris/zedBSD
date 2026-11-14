<!-- awesome-plan project=zedbsd record=ws328-p008 -->

# ws328-p008 svg-values（数・長さ・list・transform・viewBox・preserveAspectRatio・paint）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: 実装済み・試験 PASS（2026-11-15 P2 q2127、cleared は Q1。check-skeleton の check 6 は未実施）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p002（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/svg/svg-values.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5。SVG 2 §4.2（属性の文法、無効な値）・§8.2（viewport の等価な変換の算法）・§8.6（viewBox）・§8.7（preserveAspectRatio）・§8.9（単位と % の軸）・§13.2（paint）、CSS Transforms 1 §7.2（transform の属性の文法）・§7.3（関数）、CSS Values 3 §5.1・§5.2（単位）。svg-tree.h（色の callback）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/svg/svg-values.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-values.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("values");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- number_read: 符号・小数・指数・`.5`・`5.`・`1e`（指数の数字無しは 1 まで読む）・overflow（1e39 で EINVAL）、cursor の進み。
- length: 各単位（大文字小文字）、未知の単位で EINVAL、resolve の各単位と % の 3 軸（正規化した対角線）。
- numbers・lengths: 区切り（空白・1 つの comma）、先頭・末尾の comma と 2 つの comma で EINVAL、途中の誤りで読めた数を返す、ENOBUFS。
- transform: 各関数・引数の数の違い（translate の 1 つ、rotate の 3 つ）・list の順（`translate(10) scale(2)` で点 (1,0) → (12,0)）・空で identity・誤りで全体が無効・上限。
- viewBox（負の幅で EINVAL、0 は可）、aspect（10 の値と meet・slice、大文字で EINVAL）、view_box_transform（SVG 2 §8.2 の算法の手の計算: xMidYMid meet・slice・none の 3 例）。
- url_read（引用符の 3 形、空白、外の文書で ENOTSUP、長すぎる id）、paint（none・currentColor・色・url と fallback・context-fill で ENOTSUP）。色は試験の木の parse_color。
- 乱数の文字列で ASan・UBSan が落ちない（NUL の後を読まない: 文字列を malloc の正確な大きさに置く）。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- transform の単位: `rotate(45deg)`・`rotate(0.125turn)`・`rotate(50grad)`・`rotate(0.785398rad)` が同じ、`translate(10px 5)`、未知の単位で EINVAL。

## 4. 注意

- transform の list の合成は header の「左が外」（M1 × M2 × …）。vec_matrix_multiply の引数の順に注意。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh values` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。check 6（zedBSD の clang と x86_64 の sysroot での compile）は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を使う。無ければ、Q1 が起動の依頼文で許した時に `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk sysroot` で自分の build/ に入れる（共有の toolchain と main の build/ は使わない・書かない。許しが無ければ Q1 に頼む）。最後の行が `check-skeleton: INCOMPLETE` の時は PASS ではない。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

2026-11-15 03:10（host の `date`）、P2（q2127-i01）。実装: `userland/desktop/libbrowser/svg/svg-values.c`（header の 14 関数と static 関数）、試験: `plan/ws328/tests/unit/test-values.c`。header・他の module・試験の道具・Makefile は変えていない。

### 実行した command と最後の行

- `sh plan/ws328/tests/run-unit.sh values`（cc）: `test-values: PASS (741102 checks)`、`run-unit: PASS`。`CC=clang` でも同じ（741102）。ASan・UBSan 付き。
- `sh plan/ws328/tests/run-unit.sh all`（cc）: test-base 204220・test-flatten 93774・test-harness 1127・test-path 348081・test-stroke 28392433・test-values 741102、`run-unit: PASS`。`CC=clang` の all も PASS（test-stroke 27637132）。
- `cc`・`clang` の `-std=c89 -pedantic -Wall -Wextra -Werror` で `svg-values.c` を compile: 警告 0。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: check 1〜5 は FAIL 無し、最後の行は `check-skeleton: INCOMPLETE`（check 6 の zedBSD の clang と x86_64 の sysroot が自分の worktree の build/ に無く、起動の依頼文で toolchain の build が許されていなかったため未実施。**PASS ではない**）。
- `git diff --check`: 出力なし。
- 仕様の資料: `build/sources/specs/ws328/` に p4 の worktree から写し、`sha256sum -c SHA256SUMS` が全 OK。

### 試験の内容

phase.md の項目を全部。期待値は仕様の式と手の計算だけ。
- number_read: 符号・小数・`.5`・指数・`1e`/`1e+`/`1em` は 1 まで・`5.` は 5 まで・桁の多い数・19 桁の mantissa の境（20 桁の 9）・denormal・1e-50 は 0・1e39 など overflow は EINVAL・cursor の進みと失敗時に cursor・値が不変・NULL。
- length: 全単位の大文字小文字、未知の単位（`10px5`・`10px-5`・長い名前）で EINVAL、resolve の全単位と % の 3 軸（`sqrt(25000)`）、overflow・NULL・非有限の context で 0。
- numbers・lengths: 区切り、先頭・末尾・二重の comma、途中の誤りで読めた数、capacity と ENOBUFS（SVG_NUMBERS_MAX の境）、`none`。
- transform: 全関数・引数の数・単位（deg・grad・rad・turn・px）・list の順（`translate(10) scale(2)` で (1,0) → (12,0)）・空で identity・誤りで matrix 不変・256 と 257 個の境・overflow。
- viewBox・aspect（10 の値と meet・slice、大文字小文字）・view_box_transform（手の計算 13 例と 20000 回の性質の試験）・url_read・paint（recording の parse_color で trim の確認）。
- 乱数: 60000 回の token の乱数の文字列を malloc の正確な大きさで全 parser に（ASan・UBSan）、20 万回の float の 9 桁の往復。

### 壊す確かめ（git の外）

`svg-values.c` を 54 通りに壊した（符号・境界の比較・定数・分岐の除去・順序の入れ替えなど）。最初の run で 51 FAIL、生き残った 3 のうち 1 つ（mantissa の桁数 `<` → `<=`）は 20 桁の試験を足して FAIL にした。残り 2 つは等価で、コード側を直した: 数を 10 進に直した後の overflow の再確認は mantissa が 2^64 未満なので到達せず（削除）、aspect の meet・slice の前の空白の確認は keyword が文字を全部読むので他の確認と重複（削除）。直した後の `svg-values.c` で試験は PASS、mantissa の壊しは 3 checks の FAIL。

### 自分で決めた点（header が黙っている所・仕様の文字どおり）

- `5.` は数 5 と残りの `.`（CSS Values 3 §4.3 と SVG 2 §9.3.9 の fractional-constant は小数点の後に数字が要る）。したがって `svg_parse_number("5.")` は EINVAL。p009（path data）も同じ svg_number_read を使うので `M5.` の `.` は誤り（SVG 2 §9.3 の文法どおり）。
- 数の list は区切り（空白または comma）を要る（comma-wsp）ので `1-2` は EINVAL（`points="10-20"` を受ける実装が多いが、仕様の文法に従った）。transform の関数どうしも区切りを要る（`translate(1)scale(2)` は EINVAL。CSS Transforms 1 §7.2 の transforms = transform comma-wsp transforms）。関数の中の引数の間は区切りが任意（`translate(1-2)` は 1 と -2）。区切りを許す方が現実の SVG に寛容なら、Q1 の判断で緩める。
- transform の関数名は大文字小文字を区別する（属性の文法の literal）。単位は ASCII の大文字小文字を区別しない。
- 数の後の単位は CSS の ident のつづき（英字・数字・`-`・`_`・非 ASCII）まで読む。`10px5`・`10px-5` は単位 `px5`・`px-5` で未知、EINVAL。
- 数の読みは C library の変換を使わず mantissa（最大 19 桁）と 10 進の exponent を double で畳み、float に丸める（locale に依らない）。FLT_MAX より大きく、float で無限大に丸まる値（約 3.4028235678e38）未満は FLT_MAX に丸め、それ以上は EINVAL。
- transform の合成後の matrix は有限（vec_finite）だけを確かめ、VEC_COORD_MAX の範囲は確かめない（範囲は後続の module が vec_matrix_valid で見る）。個々の関数の matrix は vec_matrix_translate などの EINVAL（非有限・90 度の skew）をそのまま返す。ENOBUFS は 257 個目の関数が正しく読めた時点で返す（その後ろの誤りは見ない）。
- viewBox の rect は right = min-x + width を float で足すだけ（min-x が大きく width が小さいと right - left が width と一致しない）。非有限なら EINVAL。
- `svg_parse_lengths` の ENOBUFS は *count に入った個数（capacity）を残す（誤りの EINVAL は 0）。`none` の後に何かあれば EINVAL（*count 0）。
- `svg_url_read` は先頭の空白を読み飛ばす。url() の中が空・`#` だけ・引用符が閉じない・id の後に `)` 以外は EINVAL。最初の文字が `#` でなければ（後ろを見ずに）ENOTSUP。引用符の中の id は空白や `)` を含められる。bare の id は空白で終わる。
- `svg_parse_paint` の url の後の fallback が url・context-fill・未知の語なら EINVAL（ENOTSUP にしない）。url の誤りは ENOTSUP 以外すべて EINVAL（id の長すぎも）。非 URL の paint の fallback_kind は SVG_PAINT_NONE、id は空。色は trim した text を parse_color に渡す（NUL を要しない）。tree が NULL でも none・currentColor・url は読める。
- `svg_length_resolve` は double で計算し、float に入らない結果・非有限は 0。axis が範囲の外なら正規化した対角線。

### 未実施

- check-skeleton の check 6（zedBSD の clang と x86_64 の sysroot での compile）。起動の依頼文で toolchain の build が許されていなかったので worktree の build/ に作っていない。`make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` と `sysroot` を Q1 が許せば、同じ worktree で `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh` を流す。
- QEMU・実機の試験（この Phase の範囲外）。

### header への指摘

なし（header の不備で止まった所は無い）。
