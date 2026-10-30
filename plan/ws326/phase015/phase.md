<!-- awesome-plan project=zedbsd record=ws326-p015 -->

# ws326-p015 補間と重み

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-31 S-p015。cleared は Q1 が受け入れを見て決める）。
- 依存: p007（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-interp.h`、`userland/desktop/libmedia/h264sw-weight.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.4.2.2.1（式 8-223〜8-261、表 8-12、図 8-4）、8.4.2.2.2（式 8-227〜8-230・8-262〜8-270）、8.4.2.3・8.4.2.3.1・8.4.2.3.2（式 8-271〜8-276）、8.4.3（式 8-277 以下）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-interp.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- `userland/desktop/libmedia/h264sw-weight.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-interp.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- 単体試験 `plan/ws326/tests/unit/test-weight.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- luma: 16 の分数位置 (0..3, 0..3) の各々で、決めた 9x9 の参照の小 plane から手で計算した 1 sample（b、h、j、a、e、r など）を比べる。整数位置は複写。plane の外を指す MV（-1000、+1000）で端の sample の繰り返しになり読み越しが無い（ASan）。
- chroma: 8x8 の分数位置の 4 隅（0,0・7,0・0,7・7,7）と中（4,4）の手で計算した値、clamp。
- weight: default の単・双、explicit の logWD 0 と 5、負の重み、offset、clip。implicit: POC で手で計算した DistScaleFactor から w0・w1、long-term・同じ POC・範囲外で 32/32。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh interp weight` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果（2026-10-31）

- 実装: `userland/desktop/libmedia/h264sw-interp.c`、`userland/desktop/libmedia/h264sw-weight.c`（骨格を置き換え。header は変えていない）。単体試験: `plan/ws326/tests/unit/test-interp.c`、`plan/ws326/tests/unit/test-weight.c`。
- 実行した command と結果（worktree の root）:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh interp weight` → `test-interp: PASS (23460 checks)`、`test-weight: PASS (159 checks)`、最後の行 `run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 22 本全部 PASS（test-bits 169、test-bitwriter 14、test-cabac 195654、test-cabac_init 4159、test-cabac_se 168241、test-cavlc 87868、test-deblock 3313、test-dpb 129447、test-frame 284、test-interp 23460、test-intra 593、test-macroblock 1107981、test-mb 1938、test-mvpred 83、test-nal 95、test-poc 382、test-ps 41887、test-reflist 278758、test-slice 141694、test-tables 846、test-transform 33158、test-weight 159）、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so`（Phase の command のとおり）→ exit 0、log の `warning` は 0 行（h264sw-interp.c・h264sw-weight.c は `-Wall -Wextra -Werror` で compile され、link まで通った）。
  - `git diff --check` → 出力なし（exit 0）。
- 試験の作り方（期待の値は仕様の式と手の計算だけ。他の decoder の source・出力は使っていない）:
  - luma: 9x9 の 0 の plane に 1 つの impulse（100）を置き、6 tap の各 tap（20・1・-5）に当たる 4 つの位置（G・H・E・A）の 16 位置 x 4 表を手で計算（b1 = 20 x 100 = 2000 → b = 63、j1 = 40000 → j = 39 など）。255 の clip、負の値の clip、j と b の丸めの境（impulse 32 の j1 = 12800 で (j1 + 512) >> 10 = 13、impulse 4 の b1 = 80 で (80 + 16) >> 5 = 3）。
    一次の ramp（8x + 4y）は filter が厳密に再現するので、8x8 と 16x16 の block の全 16 位置が G + 2 xFrac + yFrac。
    plane の外を指す MV（-1000・1000・INT_MIN・INT_MAX）は端の sample の繰り返しになる（左右では全 xFrac が同じ、上下では全 yFrac が同じ、隅では隅の sample）。端をまたぐ block は、端を繰り返しで埋めた plane（border 24）での予測と全位置で一致（2x2・4x4・8x8）。plane は malloc の丁度の大きさで ASan が読み越しを見る。
  - chroma: 8x8 の plane（3x + 17y + 5）で xFrac 3・yFrac 5 の 4 隅と中（17・37・125・145・97）を式 8-270 で手計算、64 の分数位置の全部を ramp の式（A + ((24 xFrac + 136 yFrac + 32) >> 6)）と比較、端の clamp（左右上下の遠い値）。
  - weight: default の単・双（丸め）、explicit の logWD 0・1・5・7、負の重み、負の積の floor の shift、offset（正・負・平均の floor）、clip、list 1 だけ、双予測（8-276）、実装の block 全体（stride、destination が source、block の外は不変）。モードの選択（P・SP・B の idc 0..3・I・table が無い時）。
    explicit の factors（luma・Cb・Cr、list 0 だけ・list 1 だけ・両方、index 31）。implicit は poc の組で tb・td・tx・DistScaleFactor を手で計算（中間・外挿・境界 128 と 136・下の外れ・逆順・奇数の距離・同じ POC・long-term・複数の組・範囲外の組が 32）。
  - 誤りの入力: NULL・0 の大きさ・16 を超える block・0 の plane（何も書かない）、分数位置の mask、巨大な ref_idx・component・logWD・weight・offset・order count（INT32_MIN・INT32_MAX）・list の count（UBSan・ASan の報告 0）。乱数の plane・位置・分数・block の大きさ 3000 回（luma は落ちない、chroma は plane の最小〜最大の中）。
- わざと壊した確かめ（git の外で一時的に変え、試験が FAIL することを確かめ、`cmp` で元に戻ったことを確かめた）: 6 tap の係数、j の丸め 512 → 511、g の m の列、clamp の size - 1、chroma の重み b の入れ替え、chroma の +32、平均の +1、p の s の行、floor の shift を切り捨てに、offset の平均の +1、DistScaleFactor の +32、上限 128 を >=、chroma の添字、long-term の判定、単の丸め項、双の丸め項、implicit の組の転置、tx の符号。
  最初の 18 のうち 17 が FAIL、j の丸め 511 だけが通ったので、丸めの境の impulse（上の 32 と 4）を足し、j の 511 と b・h の 15 も FAIL になることを確かめた。
- 自分で決めた点（header が黙っている所）:
  - 3 関数の誤りの入力は戻り値が無い（void）ので、何も書かない: interp は plane・out が NULL、plane の幅か高さが 0、block の幅か高さが 0 か 16 超（chroma も同じ上限）。weights_apply は factors・out が NULL、block が空、p0 も p1 も NULL。
    分数位置は luma を & 3、chroma を & 7 にする（定義済みの動作）。x・y は int の全範囲を受け、座標は int64 で clamp する。stride は plane_width 以上と仮定する（読む位置は clamp 後で plane の範囲）。
  - 負の値の右 shift は使わない: filter の中間値は「負なら 0」に clip（(v + add) < 0 なら clip で 0 になる）、weight の shift は floor の補助関数（負の値は大きさを切り上げて符号を戻す）。tx の除算は大きさで割って符号を戻す（8-197 の「0 へ切り捨て」）。weight の積は int64。
  - weights_apply の logWD が 7 を超える時は 7 として扱う（構文の上限）。w・o は int の全範囲で未定義の動作にならない。
  - weights_setup: explicit は header->weighted が 1 の時だけ（table を読んでいない header は default）。B の weighted_bipred_idc が 3 の時は default。SP は P と同じ。I は default。lists が NULL の implicit は mode だけ implicit で全部の組が 32。
    list の count が 32 を超える時は 32 で止める。implicit_w0 は全 entry を 32 で初期化してから、使う組だけ 8-280〜8-283 で上書きする。reflist の `missing` の entry（stand-in の frame）は、その entry の poc・long_term をそのまま使う。
  - weights_of: 使う list が無い・index が 32 以上・component が 2 超・weights が NULL は default の factors（weighted 0、他は 0）。implicit は両方の list の時だけ（片方は default）。explicit は使わない list の w・o を 0 にする。
  - 8.4.3 の MBAFF の場合（8-284、8-285 の >> 1）と field の場合は範囲の外（frame だけ）なので実装していない。header と同じ。
- 観測できない物: なし（全分岐を試験で通した。j の丸めと b の丸めは境の impulse で観測できる）。
- 未実施: QEMU・実機の試験（この Phase は host の単体試験と build まで）。recon との結合（p019）は後の Phase。規約の全文の見直しはこの WS の最後の Phase。
- header の不備: 見つけていない（header は変えていない）。
- 注: worktree の `.git` の file が worktree 作成の時に commit-msg の hook の内容で上書きされていた（`gitdir:` の行が無く git が動かなかった）。`gitdir: /home/awe/zedBSD-claude1/.git/worktrees/s-ws326-p015` の 1 行に書き直した（worktree の中の file の修復。main の checkout には触れていない）。他の worktree でも同じ事が起きていないか Q1 に確認を頼む。
