<!-- awesome-plan project=zedbsd record=ws328-p006 -->

# ws328-p006 vec-gradient（色の補間・ramp・t の計算）

- WS: [WS328](../ws.md)。設計: [design.md](../design.md)。
- Status: cleared（Q1 の受け入れ待ち）。実装済み・試験 PASS（2026-11-15 P5 q2126-i01）。2026-11-14 p001 が作成
- 担当: 実装の担当（Sonnet 5.5 high、phase-runner-low、1 回の起動）。
- 依存: p002（依存の Phase が main に merge され cleared の後に始める）。
- 基点の SHA（header の凍結）: ws.md の Resume point に Q1 が書いた SHA。`BASE=<それ>`。

## 1. 読む物

- [AGENTS.md](../../../AGENTS.md) §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）。
- [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに全文を当てる）。[plan/guardrail.md](../../guardrail.md) の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」。
- [design.md](../design.md) の §1・§3・§6・§17 と、下に挙げた節。
- 仕様（git の外）: `build/sources/specs/ws328/` の HTML と `txt/` の text（design.md §16 の URL と SHA-256。自分の worktree に無ければ同じ URL からパラメータの無い GET で取り、`sha256sum -c SHA256SUMS` で照合する。合わなければ Q1 に報告）。読み方: `txt/` を grep し、式・表は HTML の該当の節も見る。
- 実装する header: `userland/desktop/libbrowser/vector/vec-gradient.h`（仕様。comment を全部読む）。
- 仕様の該当: design.md §5・§17 の D6。CSS Images 3 §3.1（線形）・§3.2（放射）・§3.3（repeating）・§3.4.2（色の補間、premultiplied）・§3.4.3（fixup）、CSS Images 4 §3.3（conic）・§3.5.2（hint の式）・§3.5.3、CSS Color 4 §12.1（legacy の sRGB は sRGB で補間）・§12.3（alpha と premultiplied）、SVG 2 §14.2.1（straight の rgba の補間の式）・§14.2.2.2（同じ 2 点）・§14.2.3.2（焦点の円が外の時の円錐、重なる円）・§14.2.4.3（stop の順）、HTML §4.12.5.1.10（createRadialGradient の ω の手順）。
- 依存の module の header（include される物）。

## 2. 作る物

- `userland/desktop/libbrowser/vector/vec-gradient.c`（骨格を実装で置き換える）
- `plan/ws328/tests/unit/test-gradient.c`（`#include "check.h"`、必要なら `raster-reference.h`・`svg-test-tree.h`。最後に `return check_finish("gradient");` の形で 1 行。test が 2 つの時はそれぞれ）
- この phase.md の「結果」の節、ws.md の Phase の表のこの行。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る。他の実装の出力を使わない。
- fixup（CSS）: CSS Images 3 §3.4.3 の例（offset の無い stop の均等な配置、前より小さい offset の繰り上げ、最初 0・最後 1）。fixup（SVG）: [0,1] への clamp と繰り上げ。
- repeat_span: 範囲の写しと start・length、長さ 0 で ENOTSUP。
- color_at: 2 stop の中間（premultiplied: 赤不透明と透明の中間が CSS Images 3 §3.4.2 の例の値、straight: rgba の平均）、hint（H = 0.25 で位置 0.25 が中間の色、式 P^(log 0.5 / log H)）、範囲の外の clamp、NaN が 0。同じ offset の 2 stop（硬い stop）の両側。
- average: 2 stop の平均、硬い stop を含む物。
- ramp_width の clamp、ramp の texel i が (i + 0.5)/width の色。
- parameter: 線形（start・end を通る直線の上の t、垂線の上で一定）、同じ 2 点で t = 1、放射（焦点 = 中心・半径 0 で t = 距離 / 半径）、焦点が中心と違う物（HTML の手順の手の計算: 円の上の点で t = 1）、焦点の円が外（円錐の外で painted 0）、2 円が同じで painted 0、円錐（上が 0、右が 0.25、start_angle の分のずれ）。to_gradient の行列を通す。
- spread: pad・reflect（1.5 → 0.5、-0.25 → 0.25）・repeat（1.25 → 0.25、-0.25 → 0.75）、NaN が 0。
- sample: ramp からの lookup の index（t = 1 で width - 1）、painted 0 で 0。
- 乱数の stop と点で ASan・UBSan が落ちない。

（2026-11-15 design-reviewer の反映で足した項目。上の項目と食い違う所はこちらが正）
- hint は entry（`is_hint`）で渡す: `red 50%, 20%, blue` で hint が 50% に繰り上がり、硬い遷移の扱い（H の端）。H = 0 で P > 0 の全てが第 2 色、H = 1 で P < 1 の全てが第 1 色、H = 0.5 で線形と同じ。fixup の後の count が stop の数。文法に反する hint（先頭・末尾・連続）で EINVAL。
- `vec_gradient_extend_span`: −50%〜150% の stop の写しと start・length、[0, 1] の中の stop で何もしない。
- `vec_gradient_average`: CSS Images 3 §3.3 の例（red・white・blue が全て 0px → rgb(75%, 50%, 75%)）。
- radial: 安定な根の式で、焦点が中心の近く・円がほぼ同じ・r(ω) が 0 になる点（描かない）の手の計算。円錐: 中心で t = 0、`vec_atan2` を使う事（真上・真右・真下・真左で 0・0.25・0.5・0.75）。
- linear の start = end: REPEAT・REFLECT でも最後の stop の色（`vec_gradient_sample`）。
- `vec_gradient_line_length`: 線形・放射・円錐の手の計算（行列 scale 2 で 2 倍）。

## 4. 注意

- premultiplied の color_at は premultiplied のまま 8 bit に丸める（各 color の channel ≤ alpha）。straight（SVG）の補間の結果も最後に premultiplied にして返す（header）。
- 放射の二次方程式は header の係数で、先頭の係数が 1e-6 未満は一次として解く。
- 骨格の関数の「Not implemented yet.」の comment は実装の comment に置き換える（規約の §10）。
- 未定義の動作をしない: 負の値の shift、符号付きの overflow、範囲の外の float → int の変換、NULL の算術。信用できない入力で範囲の外を読み書きしない。

## 5. 変えてはいけない物

- 全ての header（`userland/desktop/libbrowser/vector/*.h`・`userland/desktop/libbrowser/svg/*.h`）、他の module の .c、試験の道具（`plan/ws328/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/raster-reference.h`・`unit/svg-test-tree.h`・`unit/test-harness.c`）、Makefile・`mk/`・`sources.mk`、HAL・toolchain・共有の build。
- header の不備（足りない・矛盾・仕様と違う）を見つけたら header を直さず止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に列挙する。phase.md が仕様と食い違えば仕様に従い、食い違いを報告する。
- 他の実装（Skia・cairo・pixman・Blink・WebKit・Gecko・resvg・librsvg・nanosvg・FreeType・Mesa など）の code・comment・構造・名前を写さない。仕様と数学から書く。

## 6. 確かめの command

- `sh plan/ws328/tests/run-unit.sh gradient` と `sh plan/ws328/tests/run-unit.sh all`（最後の行 `run-unit: PASS`）。`CC=clang` でも同じ。
- `BASE=<基点の SHA> sh plan/ws328/tests/check-skeleton.sh`（最後の行 `check-skeleton: PASS`。基点の SHA は ws.md の Resume point）。check 6（zedBSD の clang と x86_64 の sysroot での compile）は自分の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を使う。無ければ、Q1 が起動の依頼文で許した時に `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` の後 `timeout 3600 make -j4 CONFIGMK=config/config.x86_64.mk sysroot` で自分の build/ に入れる（共有の toolchain と main の build/ は使わない・書かない。許しが無ければ Q1 に頼む）。最後の行が `check-skeleton: INCOMPLETE` の時は PASS ではない。
- `git diff --check`。

## 7. 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊し（git の外で: 例えば符号・境界の比較・定数を変える）、試験が FAIL する事を確かめて元に戻す（`git diff` が空に戻った事を確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block には出典の comment（「Values referenced: <仕様と版> <節・表>」、複数行の comment）。
- commit: `git add` の後 `git commit -m WIP -- <作った・変えた path>`。push しない。
- この phase.md の「結果」の節に: 日付（host の `date`）、実行した command と最後の行・件数、自分で決めた点、未実施。ws.md の Phase の表のこの行を「実装済み・試験 PASS」に（cleared は Q1 が決める）。

## 結果

2026-11-15 02:58（host の date）P5（q2126-i01）。worktree `/home/awe/zedBSD-worktrees/p5-ws328p006`（基点 main 6aeb76a864）。作った物は `userland/desktop/libbrowser/vector/vec-gradient.c`（骨格を実装に置き換え）と `plan/ws328/tests/unit/test-gradient.c`。header・他の module・試験の道具は変えていない。

実行した command と結果:

- `sh plan/ws328/tests/run-unit.sh gradient`: `test-gradient: PASS (277264 checks)`、最後の行 `run-unit: PASS`（cc）。`CC=clang` も同じ。
- `sh plan/ws328/tests/run-unit.sh all`（cc・clang とも）: test-base 204220、test-flatten 93774、test-gradient 277264、test-harness 1127、test-path 348081 checks、最後の行 `run-unit: PASS`。ASan・UBSan 付き（run-unit.sh が付ける）。
- `BASE=57bf94e62f sh plan/ws328/tests/check-skeleton.sh`: 最後の行 `check-skeleton: PASS`。この worktree の build/ に toolchain が無いので、check 6 は `WS328_ZEDBSD_CC` と `WS328_ZEDBSD_SYSROOT` で p2-ws328p004 の worktree の `build/llvm/bin/clang` と `build/x86_64/sysroot` を読むだけで使った（書いていない。自分の build/ への toolchain の build はしていない）。
- `git diff --check`: 出力なし。
- 壊す確かめ（git の外、script で 42 通り: 符号・境界の比較・定数・式の取り違え。fixup の各段、hard stop の比較、hint の式と両端、丸め、premultiplied の切り替え、average の pad と平均、ramp の中心、放射の根の選び方・判別式・同じ円・二次式から一次式、円錐の符号・floor、spread の各式、index の clamp、line length の行列）: 40 通りが FAIL。初回は 6 通りが PASS（丸めの切り捨て、同じ円の判定、判別式 0、a が小さい時の一次式、color_at の分母の摂動、q の符号）で、試験を足して（丸め・狭い span を許容 0 で、判別式 0 の点 (4,3)、a が 1e-5 の点、同じ円の判定の点）塞いだ。残った PASS は 2 通りで等価: (1) `gradient_pack` の「color チャンネルが alpha を超えない」clamp（丸めが単調で premultiplied の値は alpha 以下なので届かない防御）、(2) 放射の根の安定形で `b` の符号を逆にする（q = b - sqrt(D) も数学上は同じ根で、single precision の安定さだけが違う）。元に戻した事は script が元の file との全文の一致で確かめた。

自分で決めた点（header が黙っている所）:

1. 計算は double で行い、渡す所で float に丸める。float に入らない値は ±1e30 に止める（範囲外の double→float の変換は未定義）。
2. `vec_gradient_color_at`: 位置 ≤ p となる最後の stop を取る。同じ offset の stop の位置ちょうどでは後の色（硬い遷移）。最初の stop より前は最初の色、最後の stop 以後は最後の色。offset が NaN や順序の乱れた stop でも範囲外を読まず、重みは [0,1] に止める。
3. hint の重み: P ≤ 0 は 0、P ≥ 1 は 1、H = 0.5 は P、H ≤ 0 は 1、H ≥ 1 は 0、その他は pow(P, ln 0.5 / ln H)。数でない hint は hint なしとして扱う。
4. `vec_gradient_average`: offset を [0,1] に止めて使い、最初の stop より前と最後の stop より後の [0,1] の部分も端の色で数える（SVG の focal が円の上の repeat の時の 1 周期の平均）。止めた後の最後 ≤ 最初なら i/(count-1) に均等に配る（hint は比のまま残す）。hint のある組は重みの平均 1/(k+1)（k = ln 0.5 / ln H）で厳密に積分し、straight（SVG）の組は premultiply が alpha との積で二次になるので平均二乗 1/(2k+1) も使って厳密に積分する。
5. `vec_gradient_fixup_css`: 順序は header のとおり（先頭・末尾 → 繰り上げ → 均等 → hint の除去）。hint の H は [0,1] に止める。除去した後の stop は has_hint・hint を hint が付く物だけ 1・値にし、他は 0 にする。`vec_gradient_fixup_svg` は has_offset を 1 にする。
6. `vec_gradient_extend_span`・`vec_gradient_repeat_span` は count が 1 でも動く（repeat_span は長さ 0 で ENOTSUP なので 1 つは ENOTSUP）。
7. `vec_gradient_parameter`: painted は NULL でもよい。to_gradient が valid でない、幾何が有限でない、写した点が有限でない時は painted 0。半径が負の放射は painted 0。幾何の絶対値の上限は課さない（有限なら使う）。線形で start と end の距離が 1e-6 未満は t = 1・painted 1（`vec_gradient_sample` は同じ判定で spread を使わない）。放射の根はすべて double で計算する。
8. `vec_gradient_line_length`: 円錐で area が NULL の時は 0。area の角が有限でない時も 0。線形・放射で area は使わない。
9. `vec_gradient_sample`: 幅が範囲外・ramp が NULL・gradient が NULL は 0。index は t * width の切り捨てで、0 .. width-1 に止める。

header への所見（直さず報告。header は固定）:

- 放射の gradient で焦点 = 中心・焦点の半径 0 の CSS の形（CSS Images 3 §3.2）は、header の「r(ω) > 0 の最大の w」の規則どおり、中心の点そのもの（r(0) = 0 の 1 点）を描かない。1×1 px の box の中心の pixel の中心 (0.5, 0.5) は丁度その点で、その pixel は透明になる。HTML の canvas の規則では円周だけを描くので中心は未定義だが、CSS の radial-gradient() は中心を最初の stop の色で塗る。呼ぶ側（paint の統合）が CSS の放射で `painted == 0` の時に t = 0 として塗るか、header の規則に focal_radius = 0 の中心の例外を足すかを、p018 以後で決める事を勧める。試験は header のとおり「描かない」を検証している。
- `vec_gradient_color_at` の comment は premultiplied を引数 `premultiplied` に関わらず返すと読めるが、straight（SVG）の補間を premultiplied に直して返す事で、header の文のとおりに実装した。header の誤りではない。

未実施: 実機・QEMU の確認（この Phase は host の単体試験のみ）、zedBSD の clang での link（check 6 は compile のみ）、GPU の shader との値の比べ（p018 以後）。
