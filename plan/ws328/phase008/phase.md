<!-- awesome-plan project=zedbsd record=ws328-p008 -->

# ws328-p008 svg-values（数・長さ・list・transform・viewBox・preserveAspectRatio・paint）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
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

（実装の担当が書く。）
