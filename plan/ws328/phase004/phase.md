<!-- awesome-plan project=zedbsd record=ws328-p004 -->

# ws328-p004 vec-stroke（線の輪郭）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
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
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。zedBSD の clang が自分の worktree に無ければ `WS328_ZEDBSD_CC=/home/awe/zedBSD-claude1/build/llvm/bin/clang`（読むだけ）で check 6 も流す。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

（実装の担当が書く。）
