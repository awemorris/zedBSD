<!-- awesome-plan project=zedbsd record=ws328-p002 -->

# ws328-p002 vec-base と vec-path（行列・path の builder・弧・tight な bbox）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（2026-11-15 Q1、merge 4360704e39）
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

実装の担当（P4、Sonnet 5.5、Queue q2104-i01）。日付は host の `date`（2026-11-15）。

### 実装

- `vec-base.c`: 全関数（vec_finite・vec_point_valid・行列の identity/valid/multiply/invert/apply/apply_vector/scale_max・translate/scale/rotate/skew_x/skew_y・vec_rect_include/empty・vec_atan2・vec_exp）。
- `vec-path.c`: 全関数（init/release/reset/error、move/line/quad/cubic/arc/close、rect/rounded_rect/ellipse、transform/copy、view_of/view_check/bounds）。builder の命令は scratch の領域に集めてから 1 回で path へ足す（失敗は error 以外を変えない）。
- 試験: `plan/ws328/tests/unit/test-base.c`（204220 checks）・`test-path.c`（348081 checks）。期待の値は仕様の式と手の計算（半円の中点 (5, ±5)・円 r=10 の中心 (5, ±8.660254)・四半円の handle 4/3·tan(π/8)・二次の極値 5・三次の極値 7.5 と ±5√3・シア行列の特異値 (1+√5)/2・回転した楕円の bbox √((25+9)/2) など）。弧は乱数の円（3000 個）で「全ての sample が 1 つの円の上・sweep の符号が flag・large が半周超」を独立の幾何で確かめた。

### 確かめ（出力を読んだ）

- `sh plan/ws328/tests/run-unit.sh base path`: `test-base: PASS (204220 checks)`、`test-path: PASS (348081 checks)`、最後の行 `run-unit: PASS`。
- `sh plan/ws328/tests/run-unit.sh all`（cc）と `CC=clang sh plan/ws328/tests/run-unit.sh all`: どちらも test-base・test-harness (1127)・test-path が PASS、最後の行 `run-unit: PASS`（ASan・UBSan・-Wall -Wextra -Werror）。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: 最後の行 `check-skeleton: PASS`（check 6 の zedBSD の clang・x86_64 の sysroot の compile を含む。自分の worktree の build/ に `llvm-toolchain-cached` と `sysroot` を入れて使った）。
- `git diff --check`: 出力なし。

### わざと壊す確かめ（git の外、30 通り、script は担当の scratchpad）

28 通りで試験が FAIL（行列の合成の順・e の項・逆行列の e の符号・行列式の下限・scale_max の符号・rotate の c の符号・atan2 の反射と係数・exp の下限・rect_include の下端・範囲の検査・弧の中心の符号・sweep の補正・handle の長さ・分割の余裕・close の後の暗黙の move・三次の第 2 根・二次の根・rrect の handle の向き・楕円の向き・上限の境界・view の点数の検査・copy の closed・move の sticky・close の current・弧の端点の一致・半径の絶対値・変換の start）。毎回元に戻し、`git diff` が空に戻った事を確かめた。
残った 2 通りは等価の変異: (1) skew の `reduced == 90` の削除は、90° の tanf が −2.2e7 になり `VEC_COORD_MAX` の検査で同じく EINVAL になるため結果が同じ（二重の防御）。(2) path_curve_at の hull への clamp の上限の変異は、float の丸めで hull を超える入力を作れず試験で踏めない（防御）。

### 自分で決めた点（header が黙っている所）

1. `vec_path_view_check`: 動詞も点も無い view は well formed（0）。`vec_path_bounds` は空の view を EINVAL（header どおり）。件数が `VEC_PATH_*_MAX` を超える view も EINVAL。
2. `vec_rect_include`: 無効な点（NaN・範囲の外）は無視。`bottom < top` の矩形も `right < left` と同じく空の始まりとして扱う。
3. `vec_atan2`: 無限大が入っても 0（NaN と同じ扱い）。
4. `vec_matrix_rotate`: 角度を fmodf で [0, 360) に畳み、0・90・180・270 度は cos・sin を厳密な 0・±1 にする（90° で (1,0)→(0,1) が厳密）。skew は 180° 周期で [−90, 90] に畳み、ちょうど ±90（450°・270° を含む）は EINVAL。
5. 弧: 半径は [1e-6, 1e9] に収める（二乗を作らず、`x1'/rx` を先に割る形で計算し、float の範囲に収まるため）。Λ が 1e-20 未満（弦が半径の 1e-10 未満）は直線を足す。角度は libm の atan2f（`vec_atan2` の誤差 1e-5 が大きな半径で端点を動かすため使わない）。曲線の数は ceil(|Δθ|/(π/2) − 1e-4) を 1〜4 に収める（半円の丸めで 3 本にならない）。最後の曲線の終点は引数の (x, y) をそのまま使う。曲線の点が座標の範囲を超えれば（極端に細長い楕円）EINVAL（sticky）。
6. 弧で端点が現在の点と等しい（float の完全一致）時は、close の直後でも暗黙の move を足さず何もしない。現在の点が無い時の EINVAL は端点の比較より先。
7. `vec_path_append_rounded_rect`: 半径はどちらかが 0 の角を両方 0（直角）にする。辺に沿った半径の和が辺を 1/1024 の余裕の中で超える時は、全ての半径を同じ比（最も混む辺が丁度満ちる）で縮める（header は余裕の外を EINVAL としか書いていない。CSS Backgrounds 3 §4.5 の縮め方）。直角の角は命令を足さない（辺の line が角で終わる）ので、全ての角が直角なら `vec_path_append_rect` と同じ M L L L Z。曲がる角が n 個（1〜4）の時は move 1・辺の line 4・曲線 n・close 1 で 6 + n verb、5 + 3n 点（n = 4 で 10 verb・17 点）。開始は左上の角の曲線の終点（直角なら角そのもの）。
8. `vec_path_copy`: source が error なら EINVAL で destination は触らない。destination == source は何もしない。確保に失敗したら destination を release して error ENOMEM。copy の capacity は件数ちょうど。
9. `vec_path_transform`: 失敗は sticky な EINVAL（header どおり）。点の他に start・current も写す（検査してから書く 2 回の走査）。
10. bbox: 極値の点は de Casteljau で求め、制御点の箱に clamp する（丸めで座標の範囲を出ないように）。move だけの subpath の点も含む。三次の導関数の根は安定な二次の解の公式（a = 0 は一次）。
11. `vec_matrix_invert` は float のまま（行列式も float）。逆行列が `vec_matrix_valid` を満たさない（成分が 2^24 を超える）時は EINVAL。

### header の疑問（直さず報告）

- `vec_path_view_check` の comment は空の view の扱いを書いていない（上の 1 で決めた）。
- `vec_path_append_rounded_rect` の comment は「1/1024 の余裕の中の超過」をどうするかを書いていない（上の 7 で縮める事にした）。
- 契約の誤りは見つからなかった（header は変えていない）。

### 未実施

- zedBSD の実機・QEMU での実行（この Phase の範囲外。zedBSD の clang での compile だけ check 6 で確かめた）。
- realloc の失敗（ENOMEM）の経路の試験（phase.md が不要とした）。
- 規約の全文の見直しは p023。自分の diff は coding-style §14 の checklist で読んだ（関数の中の関数呼び出しの条件・三項演算子・for の宣言は grep で 0 件を確かめた）。
