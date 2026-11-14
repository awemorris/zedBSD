<!-- awesome-plan project=zedbsd record=ws328-p009 -->

# ws328-p009 svg-pathdata（path の `d`）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（Q1 の受け入れ待ち。2026-11-15 P2 q2129 が実装、試験 PASS）
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

2026-11-15 03:36（host の `date`）、P2（q2129-i01）。実装: `userland/desktop/libbrowser/svg/svg-pathdata.c`（`svg_path_data_parse` と static 関数）、試験: `plan/ws328/tests/unit/test-pathdata.c`。header・他の module・試験の道具・Makefile は変えていない。

### 実行した command と最後の行

- `sh plan/ws328/tests/run-unit.sh pathdata`（cc）: `test-pathdata: PASS (391002 checks)`、`run-unit: PASS`。`CC=clang` でも同じ（391002）。ASan・UBSan 付き。
- `sh plan/ws328/tests/run-unit.sh all`（cc）: test-base 204220・test-flatten 93774・test-gradient 277264・test-harness 1127・test-path 348081・test-pathdata 391002・test-raster 1055903・test-rrect 419610・test-stroke 28392433・test-values 741102、`run-unit: PASS`。`CC=clang` の all も PASS。
- `BASE=57bf94e62f WS328_ZEDBSD_CC=<p004 の build/llvm/bin/clang> WS328_ZEDBSD_SYSROOT=<p004 の build/x86_64/sysroot> sh plan/ws328/tests/check-skeleton.sh`: 最後の行 `check-skeleton: PASS`（check 6 まで流れた。p004 の worktree は読むだけ）。
- `git diff --check`: 出力なし。
- 仕様の資料: `build/sources/specs/ws328/` に p4 の worktree から写し、`sha256sum -c SHA256SUMS` が全 OK。

### 試験の内容

期待値は仕様の文法・式・例と手の計算だけ（弧は header の関数 `vec_path_arc_to` を手で選んだ引数で呼んだ参照の path と、四半円の control point（4/3 tan(pi/8) x 10 = 5.522847）の手計算）。
- 各命令（絶対・相対）: M の後の暗黙の L・l（最初の m は絶対）、L・H・V・C・S・Q・T・A、複数の組、Z の後の現在の点と次の部分 path の始点（M 以外の命令は MOVE が入る）、Z の連続。
- S・T の反射（§9.5.2）: C・S の後の S、Q・T の後の T、連鎖、相対、前が L・H・M・Z・A・別種の曲線の時は現在の点、複数の組。
- 弧: flag の区切り無し（`a1 1 0 00 1 1`、`01.5-1`、`0110 10` など）、comma の位置、相対の終点（半径・回転は相対にならない）、負の半径・回転、半径 0・終点が現在の点、不正な flag（2・符号・小数点・文字・欠け）、途中で切れた弧。
- 文法の境: `M 100-200`・`M 0.6.5`（仕様の例）、符号・小数点の区切り、指数、先頭の 0、comma の位置（`,,`・`, ,` は誤り、命令の間・命令の直後・最後の組の後の comma は誤り）、wsp の 5 種と垂直 tab・NBSP・非 ASCII（wsp ではない）、空のデータ・空白だけは complete 1 で何も描かない。
- 誤り（§9.5.4）: `M 10,10 L 20,20,30` は M と 20,20 への L を残す、各命令で最後の完全な組まで、M で始まらないデータは何も無し・complete 0、`5.`・符号だけ・指数の欠け・float に入らない数（1e39）、未知の文字。
- 座標の範囲: VEC_COORD_MAX ちょうどは通り、それを超える絶対・相対・反射の control point・Q の control point は EINVAL（path の sticky）で complete 0、VEC_PATH_VERBS_MAX ちょうどの path は通り 1 つ多いと ENOBUFS。NULL の text・path・complete は EINVAL、sticky 済みの path は EINVAL を返す。既存の内容への追記。
- 生成の試験: 数・区切り・flag の書き方を乱数で選んで path data を書く生成器（試験の中。逆方向の書き手）が、命令の意味のモデル（現在の点・始点・反射）から期待の path を path builder で作り、読んだ path と完全一致を 20000 回確かめる。
- 切り詰め: 生成した 300 個の text の全ての長さの prefix を読み、error 0・path が view_check を通る・全長で complete 1 かつモデルと一致。
- 乱数: 命令の文字・数字・符号・区切り・垂直 tab・非 ASCII などから 60000 個、`M 1 1` の後に数の文字を並べた 20000 個を、正確な大きさの block で（ASan・UBSan）。

### 壊す確かめ（git の外）

`svg-pathdata.c` を 71 通りに壊した（white space の種類・数の始まりの文字・comma の扱いの 5 通り・S・T の反射の式と条件・control point の記録・Z 後の現在の点と previous・H・V・M・弧の相対化・弧の flag と必須の区切り・暗黙の L・始点の更新・最後の組の後の comma の誤り・complete の設定・M で始まる確認・path の誤りの伝播・NULL の確認・command の文字の大文字化・組の大きさ など）。71 通り全部で試験が FAIL（または sanitizer・NULL で異常終了）。最初の run で見逃しは 0（`plain-sep-required` は壊し方の変数宣言の不足で最初は build 失敗、直して再実行し FAIL）。元に戻した後 `git diff` は空。script は `build/tmp/mut/mutate.py`（git の外）。

### 自分で決めた点（header が黙っている所・仕様の文字どおり）

- **弧の rx・ry・回転は符号を受ける**。SVG 2 §9.3.9 の EBNF は `elliptical_arc_argument ::= number ... number ... number comma_wsp flag ...` で、この 3 つに `number`（符号なし。`coordinate::= sign? number` と別）を使っており、文字どおりなら `-30` は誤り。しかし同じ仕様の §9.5.1（負の半径は絶対値を使う）と §9.3.8 の例 arcs01（`a25,50 -30 0,1 50,-25`）は符号付きを前提にするので、header の「numbers as svg_number_read reads them」に従い符号を受けた（EBNF の誤り（erratum）と判断。仕様の内部の食い違い）。符号を断る形に変えるなら `pathdata_read_arc_set` の 3 つの数の前に符号の検査を足すだけ。
- 弧の flag の後の区切りは任意（`00 1 1`、`0110 10`、`01.5-1`）、回転と最初の flag の間は EBNF どおり comma_wsp が必須（number が数字を食うので flag 直前の区切り無しは実際には起きない）。
- comma は comma_wsp の 1 個だけ（`,,`・`, ,` は誤り）。命令の間（`L 1 2 , Z`）・命令の文字の直後（`H,5`）・最後の組の後（`L 1 2,`）の comma は文法に無く誤り（その前までの完全な組は残る。complete 0）。命令の後の wsp* は文法どおり comma 不可。
- Z の直後に数（`Z 3`）は誤り（Z の引数は無く、暗黙の繰り返しは無い）。
- 数の読みは svg_number_read（p008）: float に入らない数（1e39）は誤り（complete 0、その前までは残る）。FLT_MAX 以下で VEC_COORD_MAX を超える値は path の sticky の EINVAL（関数が EINVAL を返し complete 0）で、header の「caller draws nothing」どおり。`5.` の `.` は誤り（p008 と同じ判断）。`-0` は `0 + -0` で +0 になる（M の座標は 0）。
- 現在の点・始点は path の内部の値を読まず自分で持つ（text の先頭は常に原点から。最初の `m` は絶対）ので、out が空でなくても相対の命令は text の中の点から測る。out の既存の内容には追記する。out が sticky の誤りを持つと最初の命令が同じ誤りを返す（complete 0）。空のデータは out の誤りに関係なく complete 1・戻り値 0。
- 反射は `2 * current - previous control` を float で計算し、範囲の外なら path が EINVAL にする。S・T が反射するのは直前の命令（暗黙の繰り返しの直前の組を含む）が曲線の時だけで、M・L・H・V・A・Z の後は現在の点。
- NULL の text・out・complete は EINVAL（complete が NULL でなければ 0 にする）。
- 弧の flag は 0・1 を float の 0・1 で持ち、`vec_path_arc_to` に int で渡す。

### 未実施

- QEMU・実機の試験（この Phase の範囲外）。
- 対象 platform の build（Makefile への source の登録は p001 で済み、host の cc・clang と zedBSD の clang の compile は check-skeleton が確かめた）。

### header への指摘

なし（header の不備で止まった所は無い。弧の 3 つの数の符号は上の「自分で決めた点」の仕様内の食い違いで、header は変えていない）。
