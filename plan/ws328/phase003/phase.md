<!-- awesome-plan project=zedbsd record=ws328-p003 -->

# ws328-p003 vec-flatten（平坦化）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（2026-11-15 Q1）
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p002（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/vector/vec-flatten.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5。SVG 2 §9.3.6・§9.3.7（Bézier の定義）、§9.5.3（長さ 0 の subpath）。分割数の式は vec-flatten.h の comment の導出（二階差分の上界）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-flatten.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-flatten.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("flatten");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- 直線だけの path（頂点の数と CORNER の印、close の印）。
- 二次・三次の分割数を手の計算と照合（例: 三次 (0,0),(0,10),(10,10),(10,0)、tolerance 0.1 → D と N を手で計算）。全ての頂点が曲線の上（t = i/N の評価と一致）。曲線と折れ線の最大の距離が tolerance 以下（密な t の標本で測る）。
- VEC_FLATTEN_SEGMENTS_MAX で止まる事（巨大な曲線）。tolerance の範囲の外・NaN で EINVAL、失敗で out が元の contour の数に戻る。
- 行列を掛けた平坦化（scale 10 で分割数が増える）。長さ 0 の線分を残す。move だけの subpath が 1 頂点の contour。
- vec_lines_check（first + count の overflow を含む壊れた構造で EINVAL）、vec_lines_begin・add・close、transform、bounds。
- 乱数の path の平坦化で ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- tolerance の下限は `VEC_TOLERANCE_MIN`（1e-6）: 1e-6 で受け、それ未満で EINVAL。極小の tolerance でも分割数は `VEC_FLATTEN_SEGMENTS_MAX` を超えない。

## 4. 注意

- close の時に閉じる線分の頂点を足さない（暗黙）。最後の頂点が最初と同じでもそのまま残す（header のとおり）。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh flatten` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。check 6（zedBSD の clang と x86_64 の sysroot での compile）は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を使う。無ければ、Q1 が起動の依頼文で許した時に `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk sysroot` で自分の build/ に入れる（共有の toolchain と main の build/ は使わない・書かない。許しが無ければ Q1 に頼む）。最後の行が `check-skeleton: INCOMPLETE` の時は PASS ではない。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

日付: 2026-11-15（host の `date`）。担当 P2（Queue q2117-i01）。worktree `/home/awe/zedBSD-worktrees/p2-ws328p003`、基点 main 1bf2cbe5e3、header の基点 57bf94e62f。

実装: `userland/desktop/libbrowser/vector/vec-flatten.c`（header の全関数: vec_lines_init・release・reset・begin・add・close・check、vec_flatten、vec_lines_transform・bounds）。試験: `plan/ws328/tests/unit/test-flatten.c`（13 の群）。

実行した command と最後の行:

- `sh plan/ws328/tests/run-unit.sh flatten`: `test-flatten: PASS (93774 checks)`、`run-unit: PASS`。
- `sh plan/ws328/tests/run-unit.sh all`: base 204220・flatten 93774・harness 1127・path 348081 checks、`run-unit: PASS`。`CC=clang` の `all` も `run-unit: PASS`。ASan・UBSan 付き（run-unit.sh の既定）。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: `check-skeleton: PASS`（check 6 は自分の build/ の `build/llvm/bin/clang` と `build/x86_64/sysroot`。`llvm-toolchain-cached` と `sysroot` を `make -j4 CONFIGMK=config/config.x86_64.mk` で自分の build/ に入れた）。
- `git diff --check`: 出力なし。

試験の中身（期待の値は仕様の式と手の計算）:

- 分割数: 二次 (0,0)(5,10)(10,0)・tol 0.1 → D=20、N=8。三次 (0,0)(0,10)(10,10)(10,0)・tol 0.1 → D=14.142、N=11（tol 1 → 4、tol 100 → 1）。式が整数ちょうどになる例（二次 (0,0)(0,0)(16,0)・tol 0.25 → N=4、5 でない）。二階差分の片方だけが大きい三次 2 通り（N=5）。D=0 は 1 線分。全点が同じ曲線は長さ 0 の 1 線分を残す。
- 頂点が t = i/N の Bernstein 形（double）の評価と 1e-5 で一致、内側は印なし、終点は制御点そのもの（誤差 0）。
- 曲線から折れ線への最大の距離を 2000 標本で測り tolerance 以下（二次・三次・張り出しの大きい三次 × tol 2・0.5・0.1・0.01）。
- VEC_FLATTEN_SEGMENTS_MAX（巨大な曲線、VEC_TOLERANCE_MIN の小さい曲線、座標の上限の二次）で 1024 線分。tolerance 1e-6 を受け、その半分・0・負・NaN・±inf は EINVAL、3e38 は 1 線分。
- 失敗の巻き戻し: 壊れた view（先頭が move でない・点の数の食い違い・NaN）、NaN の行列、行列で範囲の外へ出る点（途中の頂点を足した後）、頂点の上限の途中で ENOBUFS。どれも out の contour・頂点の数が元に戻る。
- 行列: 単位行列と NULL の一致、scale 10 で N=33（D=141.42）、回転 90 度・平行移動の位置、scale 0.1 で N=4。
- vec_lines_check（first + count の overflow・count 0・配列の外・NaN・上限・配列なし）、begin・add・close・reset・release、頂点・contour の上限の ENOBUFS（各 2^21・2^20 まで作って確かめた）、transform（全部を確かめてから書く）、bounds。
- 乱数: builder の乱数 path 600 本（contour の数 = move の数、vec_lines_check が 0）と乱数の view 20000 本（0 か EINVAL、失敗で巻き戻し）。

壊す確かめ（git の外で vec-flatten.c を書き換え、試験を流し、元に戻した。最後に `git diff` が空）: 21 通りで全て FAIL（見逃し 0）。ceil → floor、8 → 4、n(n-1) → n、二階差分を 1 つだけにする、tolerance の下限の `<` → `<=`、分割数の上限を外す、頂点・contour の巻き戻しをそれぞれ外す、check の overflow の式を first + count に、内側の頂点に CORNER、終点の CORNER を外す、close で頂点を足す、transform を検査なしで書く、t の分母をずらす、行列を無視、bounds が空で成功、close の後の描画で新しい contour を作らない、contour の上限を外す（ASan の heap overflow）、終点を評価で置く、長さ 0 の線分を落とす、count 0 の contour を許す。最初の流しで 3 通りが -Werror の build の失敗で FAIL だったので、build が通る形で書き直して流し直し、全て試験の FAIL であることを確かめた。見逃し 0。試験で検出できない書き換え: 曲線の評価結果を制御点の箱へ clamp する処理（丸めで箱の外へ出ない保証で、float では箱の外に出る入力を作れていない）。

自分で決めた点（header が黙っている所）:

1. vec_lines_add: 最後の contour が頂点配列の末尾で終わっていない構造（`first + count != vertex_count`）は EINVAL（他の contour の中へ頂点を差し込まない）。contour が無い時は begin と同じ。
2. vec_lines_bounds: 頂点配列の全頂点が対象（どの contour にも属さない頂点も含む）。頂点が 0 なら EINVAL。
3. vec_flatten: 空の view（verb 0 個）は成功して何も足さない。view の検査は vec_path_view_check。
4. close の直後に move なしで描く命令がある view（手で作った view）: SVG 2 §9.3.4 のとおり閉じた subpath の最初の点（行列の後）から新しい contour（CORNER）を始める。builder が作る path は常に move を挟む。
5. 曲線の内側の頂点は de Casteljau で float の評価をし、制御点の箱へ clamp する（丸めで VEC_COORD_MAX の外へ出さない）。終点は評価でなく制御点そのもの。
6. 行列を掛けた後の点（制御点も）が vec_point_valid でなければ EINVAL（path の入力の検査は行列の前）。
7. 巨大な tolerance で 8 * tolerance が float で inf になる時は 1 線分。分割数は float の段階で上限と比べてから整数にする。
8. vec_lines_check は件数の上限も検査する（頂点 > VEC_LINES_VERTICES_MAX、contour > VEC_LINES_CONTOURS_MAX も EINVAL）。

header の疑問（直していない）: `VEC_LINES_VERTICES_MAX` の comment「8 MiB of vertices at the most」は、sizeof(struct vec_vertex) = 12 バイトで 2^21 個なので 24 MiB（contour も 24 バイト × 2^20 = 24 MiB）。数字の誤りで、契約の動作には影響しない。Q1 が header の再凍結をする時に合わせて直す案。

未実施: QEMU・実機の試験（この Phase の範囲外）。規約の全文の見直しは p023。
