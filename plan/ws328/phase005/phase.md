<!-- awesome-plan project=zedbsd record=ws328-p005 -->

# ws328-p005 vec-raster（coverage の tile）

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
- 実装する header: `userland/desktop/libbrowser/vector/vec-raster.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§17 の D3。vec-raster.h の file の comment の coverage の定義（SVG 2 §13.4.2 の fill rule、CSS Masking 1 §5.1・§6.1 の clip の積と和）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-raster.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-raster.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("raster");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 照合の要: `plan/ws328/tests/unit/raster-reference.h` の reference_coverage（独立の力ずく）と、tile の全 pixel で差 ≤ 3（8 bit の段）。形: 矩形（端数の位置）、三角形、星形（自己交差、nonzero と evenodd で違う）、穴のある形（逆向きの内側の contour）、重なる 2 つの正方形（nonzero で和、evenodd で重なりが抜ける）、細い線（1 pixel 未満の幅）、tile の境をまたぐ形、offset（0.25・0.5）。
- 手の計算: header の定義から、矩形 [1.25,3.75)×[1.5,3) の pixel ごとの alpha（試験の道具の test-harness.c と同じ値）。
- tile の分類: 全部 255 の tile は run に、全部 0 は無し、他は partial。run の連続（同じ行で隣り合う全被覆の tile が 1 つの run）。並びが行 → 列の順。bounds が alpha 0 でない pixel を全部含む最小の矩形。
- clip: clip の外の pixel は 0、空の clip で空の結果。
- vec_coverage_at（tile・run・外）、intersect（255 が恒等、0 が吸収、(a·b + 127)/255）、unite（大きい方）。
- 上限: 一辺 VEC_RASTER_SIDE_MAX を超えて ENOBUFS、線分の数の上限、NaN・範囲の外で EINVAL、失敗で out が空。
- 乱数の多角形（固定の seed、各 1000 形）で参照と照合、ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- `VEC_RASTER_CROSSINGS_MAX` を超える形（長い線分を多数）は走査の前に ENOBUFS（時間が掛からない事を試験の中で確かめる）、`VEC_RASTER_TILES_MAX` を超える partial の tile も ENOBUFS。
- tile に揃えない clip で切られた tile は partial（clip の外は 0）。tile に揃えた 2 つの窓で作った tile が、1 つの大きな窓で作った物と同じ。
- 参照の rasterizer（`reference_coverage`）の戻り値が 0 である事を毎回確かめる（-1 は試験の FAIL）。

## 4. 注意

- 副走査線ごとに交点を x で並べ、winding の区間を pixel に配る（区間の両端の pixel は端数、間は 1）。float の丸めで 1 段の差まで（header）。
- 試験の参照は遅いので形は 64×64 pixel 以下に。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh raster` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
