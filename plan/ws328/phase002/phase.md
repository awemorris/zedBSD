<!-- awesome-plan project=zedbsd record=ws328-p002 -->

# ws328-p002 vec-base と vec-path（行列・path の builder・弧・tight な bbox）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: planned（2026-11-14 p001 が作成）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: —（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/vector/vec-base.h`（仕様。comment を全部読む）。
- 実装する header: `userland/desktop/libbrowser/vector/vec-path.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§6。SVG 2 §8.5（行列）、§9.3（path の命令）、§9.5.1（弧の範囲の外の引数）、付録 B.2.3〜B.2.5（弧の変換、eq. 5.1〜5.6・6.1〜6.3）、§10.2（rect の等価な path）、§10.4（ellipse の等価な path）、§8.10（bounding box）。CSS Transforms 1 §9.1・§14（変換の関数の行列）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-base.c`（骨格を実装で置き換える）
- `userland/desktop/libbrowser/vector/vec-path.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-base.c`
- `plan/ws328/tests/unit/test-path.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("base");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 行列: identity、合成の順（first の後に then: 点で照合）、逆行列（行列 × 逆 ≈ identity、特異・NaN・無限で EINVAL）、scale_max（回転・非一様の scale・skew で手の計算の特異値）、translate・scale・rotate（90° で (1,0)→(0,1)）・skew（tan 90° で EINVAL）。
- path: 各命令の verb と点の数、close の後の描画の命令で暗黙の MOVE、現在の点の無い line_to で EINVAL、sticky な誤り（最初の誤りが残り、後の命令が何もしない）、上限（VEC_PATH_POINTS_MAX を超えて ENOBUFS。上限を小さく試すため大量の move_to の loop で）、realloc の失敗は試さなくてよい。
- 弧: 端点が同じ → 何も足さない、半径 0 → 直線、負の半径 → 絶対値、小さすぎる半径 → B.2.5 で拡大（半円になる場合の中点が楕円の上）、large と sweep の 4 通りで中心と角度（手の計算: (0,0)→(10,0) 半径 5 の半円の中点が (5,±5)）、分割数（90° ごとに 1 本の三次）、三次の中点が楕円の上（誤差 1e-3 の相対）。
- 角丸矩形・楕円: 開始点・向き（時計回り、shoelace で正しい符号）・点の数、半径 0 の角は角、半径の和が辺より大きいと EINVAL。
- bbox: 直線、二次（極値が内側の曲線: 手の計算）、三次（制御点が外に出る S 字で制御点を含まない事）、move だけの subpath、空の view で EINVAL、壊れた view（verb と点の数の不整合、範囲の外の verb）で EINVAL。
- 乱数の入力（固定の seed で 10 万回の命令の列）で ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- `vec_path_append_rounded_rect` は `const struct vec_rrect *` を取る（矩形と半径）。逆向きの矩形・半径の和の超えで EINVAL。
- `vec_atan2`: 8 方向の単位 vector（0・π/4・π/2・3π/4・π・−3π/4・−π/2・−π/4）、(−1, 0) で π、(0, 0) で 0、NaN で 0、乱数の vector で 倍精度の atan2 との差 ≤ 2e-5（A&S 4.4.47 の 1e-5 と float の丸め）。
- `vec_exp`: 0 → 1、1 → e（相対 1e-6）、−87 未満と NaN で 0、88 を超えて有限。

## 4. 注意

- 弧の変換は SVG 2 付録 B.2.4 の式をそのまま（角度の差 Δθ の符号の直し eq. 5.6 の下の文）。三次の handle の長さ 4/3·tan(θ/4) は単位円の上の定数で、header の comment のとおり。
- 行列の合成の順は header の式（`then x first`）で、SVG の transform の list の順（左が外）とは svg-values の側で合わせる。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh base path` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
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
