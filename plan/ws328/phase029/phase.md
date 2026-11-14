<!-- awesome-plan project=zedbsd record=ws328-p029 -->

# ws328-p029 svg-picture の use・symbol・入れ子の svg・switch

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-15 p001 が design-reviewer の反映で p013 から分けた）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p013（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/svg/svg-picture.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§10。SVG 2 §5.1（svg）・§5.4（symbol）・§5.5（use、§5.5.1 の循環、§5.5.2 の x・y の translate と clip-path の座標系）・§5.6（switch）・§5.8（条件の属性）・§8.2・§8.8（入れ子の viewport）。svg-picture.h の file の comment の switch の定義と svg_picture_build の use の段落。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/svg/svg-picture.c`（p013 の実装に use・symbol・入れ子の svg・switch を足す）
- `plan/ws328/tests/unit/test-picture-use.c`（`#include "check.h"`・`svg-test-tree.h`。最後に `return check_finish("picture-use");`）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- use: x・y の translate（use の transform の右に付く: 点に先に translate）、symbol の viewBox と use の width・height（無い時は 100%）、svg を指す use、use の clip-path の座標系が translate 後の物、`#id` 以外の href で何も無し、自分の祖先を指す use で何も無し、`SVG_USE_DEPTH_MAX` の入れ子、10 個ずつ参照する use の入れ子が `SVG_VISITS_MAX` で ENOBUFS（時間の上限の中で）と描ける picture。
- 入れ子の svg: x・y・width・height（無い時は 100%）の viewport と viewBox・preserveAspectRatio の変換（clip しない: 既知の差）。
- switch: requiredExtensions のある子は選ばない（空の値も）、systemLanguage が "en"・"en-US"・"fr, en" で選ぶ・"fr" で選ばない、requiredFeatures は無視、最初に条件を満たす子だけ、選ばれた子が描かない要素（text）なら何も無い。
- 描かない symbol（use の外）。
- 乱数の木（use・symbol・switch・入れ子の svg を混ぜる）で ASan・UBSan が落ちない。

## 4. 注意

- p013 の試験（test-picture）が通り続ける事。p013 が決めた内部の形（walk の関数）に足す。
- p013 が use・symbol・入れ子の svg・switch を描かずに飛ばしている所（とその comment）を実装に置き換える（source に Phase の番号を書かない）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh picture picture-use` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
