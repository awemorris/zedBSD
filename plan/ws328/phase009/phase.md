<!-- awesome-plan project=zedbsd record=ws328-p009 -->

# ws328-p009 svg-pathdata（path の `d`）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p008（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/svg/svg-pathdata.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5。SVG 2 §9.3.2〜§9.3.9（命令と文法）・§9.5.1（弧）・§9.5.2（反射の制御点）・§9.5.4（誤りの扱い）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/svg/svg-pathdata.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-pathdata.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("pathdata");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 各命令（絶対・相対）の verb と点（手で書いた期待の列）。M の後の暗黙の L、Z の後の現在の点、H・V、S・T の反射（前が C・Q でない時は現在の点）、A の flag の区切り無し（`a1 1 0 00 1 1`）。
- 文法の境: 符号の区切り（`M1-2`）、小数点の区切り（`M.5.5`）、指数、comma の位置。
- 誤り: 途中の誤りでそれまでの path と complete 0、M で始まらない data で何も無し、空。
- 座標の範囲の外で sticky の EINVAL。
- 乱数の文字列と、正しい data の切り詰め（全ての長さで）で ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- 誤りの扱い（§9.5.4）: `M 10,10 L 20,20,30` は M と 20,20 への L を残す、命令の途中の誤りでその命令の完全な組までを残す。

## 4. 注意

- 数の読みは svg_number_read（p008）を使う。弧は vec_path_arc_to（p002）に任せる。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh pathdata` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
