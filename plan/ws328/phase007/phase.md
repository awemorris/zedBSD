<!-- awesome-plan project=zedbsd record=ws328-p007 -->

# ws328-p007 vec-rrect（角丸・ring・辺の分け前・影）

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
- 実装する header: `userland/desktop/libbrowser/vector/vec-rrect.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §4・§5・§17 の D2・D7。CSS Backgrounds 3 §4.1（border-radius）・§4.2（角の形と内側の半径）・§4.4（色の移り）・§4.5（重なる曲線の縮小）・§6.1.1（影の shape と spread の半径の式）・§6.1.2（σ = blur / 2、5% の許容）。Abramowitz and Stegun 式 7.1.26（erf）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-rrect.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-rrect.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("rrect");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- normalize: CSS Backgrounds 3 §4.5 の f の手の計算（例: 幅 100 高さ 50 で全部 50 → f = 0.5）、負・NaN の半径が 0、片方 0 の角が両方 0。
- inset: 外の半径 − 幅（下限 0）、幅の大きい border で内側が角。spread: §6.1.1 の例（半径 10・spread 20 → 27.5）、負の spread の floor。
- coverage: 角丸の無い矩形で今の矩形の規則と同じ（端数の重なり）、円の角の中心の pixel が 1、角の外の遠い pixel が 0、曲線の上の pixel が約 0.5（中心が曲線上の時ちょうど 0.5）、楕円の角（rx ≠ ry）。
- ring: 外 − 内、border の中の pixel が 1、padding の中が 0。
- side_share: 4 辺の和が 1（多数の pixel で）、辺の帯の中で 1、角の対角線の上で 0.5、片方の幅 0 で他方が全部。
- erf: 0 → 0、大きい x → 1、奇関数、表の値（erf(0.5) = 0.5204999、erf(1) = 0.8427008）を 5e-7 で（A&S の誤差 1.5e-7 と float の丸め）。
- shadow: σ < VEC_SIGMA_MIN で coverage と同じ、角丸の無い矩形で erf の積の厳密な値（手の式）と 1e-4、遠い所で 0、中で 1、辺の上で 0.5、角丸の矩形で密な 2 次元の数値積分（試験の中で書く、例えば 400×400 の標本）と 0.01 以内。
- 乱数の矩形・半径・σ で範囲 [0,1] と ASan・UBSan。

## 4. 注意

- 角の箱の判定は pixel の中心で（header）。d の式の平方根の引数 0 の場合は header の値。
- shadow の角の帯は中点則 VEC_SHADOW_NODES 点、範囲は [cy − 4σ, cy + 4σ] との交わり。GPU の shader が同じ順で計算するので、式の順を変えない。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh rrect` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
