<!-- awesome-plan project=zedbsd record=ws328-p013 -->

# ws328-p013 svg-picture（描く命令の列）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p011・p012（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/svg/svg-picture.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§10。SVG 2 §3.2（描く木と描かない要素）・§3.2.3・§3.6.1（opacity の group）・§5.1・§5.2・§5.4・§5.5（use と symbol、循環）・§5.6（switch）・§8.8（入れ子の viewport）・§13.2（paint の fallback）・§13.8（fill の後に stroke）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/svg/svg-picture.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-picture.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("picture");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 命令の列を試験の木で: rect 1 つの fill（行列が viewBox の変換）、fill と stroke の順、fill none で fill 無し、display none の部分木、visibility hidden で描かない子と描く孫、g の transform の合成、opacity < 1 の group、入れ子の svg の viewport、use（x・y の translate、symbol の viewBox、自分の祖先を指す use で何も無し、SVG_USE_DEPTH_MAX）、switch の最初の子、描かない要素（defs・text・image など）。
- paint の解決: currentColor、url の gradient（SVG_SERVER_GRADIENT と stop の所有）、url の先が無い時の fallback と none。
- stroke の幅の解決（% は正規化した対角線）、負の幅で stroke 無し、dash の解決。
- clip-path: この Phase では svg_clip_build は骨格（ENOSYS）なので、clip-path の要素は clip 無しで描かれる事（header: ENOMEM 以外の失敗は clip 無し）。clip の試験は p014。
- 上限（SVG_COMMANDS_MAX で ENOBUFS と描ける picture）、release の後に leak が無い（ASan）。
- 乱数の木（固定の seed で深さ・要素を混ぜた markup を作る）で ASan・UBSan が落ちない。

## 4. 注意

- 容器（g・use・svg）の bbox は clip の objectBoundingBox のためだけに要る（header）。p013 では計算の関数を作り、clip の試験は p014。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh picture` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
