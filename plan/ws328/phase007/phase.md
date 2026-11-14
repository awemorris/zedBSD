<!-- awesome-plan project=zedbsd record=ws328-p007 -->

# ws328-p007 vec-rrect（角丸・ring・辺の分け前・影）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（Q1 の受け入れ待ち。2026-11-15 P1 q2125-i01 が実装、試験 PASS）
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
- coverage: 角丸の無い矩形で今の矩形の規則と同じ（端数の重なり。display list は辺を整数にして渡す）、円の角の中心の pixel が 1、角の外の遠い pixel が 0、曲線の上の pixel が約 0.5（中心が曲線上の時ちょうど 0.5）、楕円の角（rx ≠ ry）。
- ring: 外 − 内、border の中の pixel が 1、padding の中が 0。
- side_share: 4 辺の和が 1（多数の pixel で）、辺の帯の中で 1、角の対角線の上で 0.5、片方の幅 0 で他方が全部。
- erf: 0 → 0、大きい x → 1、奇関数、表の値（erf(0.5) = 0.5204999、erf(1) = 0.8427008）を 5e-7 で（A&S の誤差 1.5e-7 と float の丸め）。
- shadow: σ < VEC_SIGMA_MIN で coverage と同じ、角丸の無い矩形で erf の積の厳密な値（手の式）と 1e-4、遠い所で 0、中で 1、辺の上で 0.5、角丸の矩形は下の「足した項目」の照合（許容 0.015）。
- 乱数の矩形・半径・σ で範囲 [0,1] と ASan・UBSan。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- normalize: 1/256 px 未満の半径が 0。
- side_share（新しい定義）: 中央線で分けた 4 象限、象限の角の線の上で 0.5、象限の外の 2 辺は 0、4 辺の和が 1（乱数の pixel・幅・半径で）、片方の幅 0 で他方が 1、両方 0 で水平の辺が 1。
- shadow（新しい積分）: header の算法と独立の密な数値積分（試験の中で書く: 例えば y を 4000 等分した中点則で、各行の左右を楕円の式から、erf は倍精度の `erf`）を比べ、σ 0.5〜3・角 8〜100 px・楕円の角（rx ≠ ry、例 92×30）で全 pixel の差 ≤ 0.015。σ 10 と 30 でも同じ。半分の行が cy ± 4σ の外の時に飛ばしても値が変わらない事。

## 4. 注意

- 角の箱の判定は pixel の中心で（header）。d の式の平方根の引数 0 の場合は header の値。
- shadow は右と左の辺ごと、直線部分は厳密、角の曲線は 45°（`vec_atan2(ry, rx)`）で二つに分け、急な半分は y の、緩い半分は部分積分で x の中点則 VEC_SHADOW_NODES 点（header の式のとおり）。範囲は cy ± 4σ・cx ± 4σ との交わり。GPU の shader が同じ順で計算するので、式の順を変えない。指数は `vec_exp`。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh rrect` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。check 6（zedBSD の clang と x86_64 の sysroot での compile）は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を使う。無ければ、Q1 が起動の依頼文で許した時に `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk sysroot` で自分の build/ に入れる（共有の toolchain と main の build/ は使わない・書かない。許しが無ければ Q1 に頼む）。最後の行が `check-skeleton: INCOMPLETE` の時は PASS ではない。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

2026-11-15 03:13（host の date）P1（q2125-i01、worktree p1-ws328p007、基点 main 6aeb76a864）。

作った物: `userland/desktop/libbrowser/vector/vec-rrect.c`（骨格を置き換え）、`plan/ws328/tests/unit/test-rrect.c`。header・他の module・試験の道具・Makefile は変えていない。

確かめ（実行した command と結果）:
- `sh plan/ws328/tests/run-unit.sh rrect`（cc、ASan・UBSan）: `test-rrect: PASS (419610 checks)`、`run-unit: PASS`。`CC=clang` でも同じ件数で PASS。
- `sh plan/ws328/tests/run-unit.sh all`: base 204220・flatten 93774・harness 1127・path 348081・rrect 419610 checks、`run-unit: PASS`。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: 検査 1〜5（header 単独・.c の cc と clang・engine の include 無し・管理情報無し・header の不変）は通った。検査 6（zedBSD の clang と x86_64 の sysroot）は自分の worktree の build/ に build/llvm と build/x86_64/sysroot が無く、起動の依頼文に toolchain の build の許しが無かったので未実施。最後の行は `check-skeleton: INCOMPLETE`（PASS ではない）。
- `git diff --check`: 出力なし。
- 影の密な数値積分（試験の中、y を 4000 等分の中点則・倍精度の erf）との最大差: 円の角 8 px・σ 0.5〜3 で 0.0023〜0.0088、角 20・24 px で 0.0079〜0.0088、楕円の角 92×30・30×92 と角ごとに違う半径で 0.0008〜0.0066、角 100 px で最大 0.0087、σ 10 と 30 で 0.0060 と 0.0010（全て許容 0.015 の内。header の「0.009 以内」とも合う）。
- わざと壊す確かめ: 34 通り（符号・境界の比較・定数・式の入れ替え・積分の端の取り違えなど）を git の外で適用して流し、30 通りは FAIL。残った 4 通り（radius の下限を `< 0` にする、片方 0 の角を `&&` にする、象限の境の `>=` を `>` にする 2 通り）は試験の見逃しだったので試験を足した（片方が 0 や 1/512 の角が辺の和の係数 f に入らない事、中央線の上の pixel が後の象限に属する事）。この 4 通りが全て FAIL になる事を確かめ、元に戻して `git diff` に vec-rrect.c の差が無い事を確かめた。

自分で決めた点（header が黙っている所）:
- 外の辺の幅・spread の上限: 幅と spread は VEC_COORD_MAX で頭打ち（それより広い箱は全て空になるため結果が同じ）。非有限・負の幅は 0、非有限の spread は 0。
- 縮小で辺が交差した時（inset の幅が箱より大きい、spread が負で大きい）は、交差した 2 辺を中点の 1 つに潰す（幅 0 の矩形）。
- normalize: 辺の和は double で取り、f を掛けた後で 1/256 px 未満になった半径の角も正方形にする（二つ目の下限の検査）。矩形の辺が非有限でも空でも全ての半径を 0 にする。
- 角の箱の境: 左側の角は left <= cx < ex、右側の角は ex <= cx < right（上下も同様）。どの角の箱にも入らない pixel は overlap だけ。
- 辺の分け前: 中央線の pixel は後の象限（cx >= xm、cy >= ym）。位置が非有限の pixel と範囲外の side は 0。
- 影: 1/√π と √2 は定数で持つ。窓は cy ± 4σ・cx ± 4σ。steep 半分の行・flat 半分の列は窓と交わらせて VEC_SHADOW_NODES 点。flat 半分は行が窓の外なら 0 で飛ばし、列が窓の外ならば境界の項だけ。θ45 の点は sinf・cosf（θ45 から）で出し、steep と flat が同じ点を共有する。A&S 7.1.26 の erf(0) は 1e-9 台で、0 に特別扱いしていない（試験は 2e-7 の許容）。
- 位置が非有限の pixel は coverage・shadow とも 0。NaN・無限の σ は 0（sharp）。σ が非常に大きい（FLT_MAX）時も 0〜1 の値で止まる。

phase.md が仕様と食い違った所: なし。header の不備: 見つけていない。

未実施: check-skeleton の検査 6（上記）。QEMU・実機の試験（この Phase の範囲外）。

Q1 への注: 試験の実行の script の結果の場所 build/tmp/ws328-unit は worktree の build/ の中。使い捨ての scratch として /tmp/p1-ws328p007-scratch/mut.py（壊す確かめの script）を作った（共有の scratchpad では他の担当の file と名前が衝突して上書きされたため）。消す物の path はこれ。
