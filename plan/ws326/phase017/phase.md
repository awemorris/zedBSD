<!-- awesome-plan project=zedbsd record=ws326-p017 -->

# ws326-p017 B の direct

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-31 Q1、2134a8efe。実装と試験 S-p017）。
- 依存: p007、p008、p016（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-direct.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.4.1.2・8.4.1.2.1（表 8-6、8-8、frame の行）・8.4.1.2.2（式 8-184〜8-190）・8.4.1.2.3（式 8-191〜8-202）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-direct.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-direct.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- temporal: 手で決めた POC（今 4、list0[0] 0、list1[0] 8）と co-located の mv (16, -8) から mvL0 (8, -4)・mvL1 (-8, 4)、list0 の long-term・td 0 の時 mvL0 = mvCol、co-located が intra で 0、co-located の ref の frame が list0 の 2 番目にある時 refIdxL0 1。
- spatial: 隣 A・B・C の refIdx の MinPositive、両方 -1 で 0・0 と mv 0、colZeroFlag（list1[0] short、refIdxCol 0、mvCol ±1 以内）で ref 0 の list の mv 0、long-term の list1[0] で colZero 無し。
- direct_8x8_inference 1 で角の 4x4（0・5・10・15）が quadrant 全体に、0 で 4x4 ごと。
- direct の bit が mb の direct に立つ。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh direct` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 実装済み・試験 PASS（2026-10-31 S-p017、Sonnet 5.5 high。cleared は Q1 が受け入れを見て決める）。
- 作った物: `userland/desktop/libmedia/h264sw-direct.c`（header は無変更）、`plan/ws326/tests/unit/test-direct.c`。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh direct` → `test-direct: PASS (259 checks)`、`run-unit: PASS`（ASan・UBSan 黙る）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 21 試験 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の `warning` は 0 件。
  - `git diff --check` → `check-skeleton: PASS`。
  - 試験の検出力: 角の block の表（0、5、10、15）と floor の右 shift を一時的に壊すと 7 件が FAIL になることを確かめ、元に戻した（commit していない）。
- 試験の中身: DistScaleFactor（128、85、-256、512、clip の 1023 と 256、極値の POC、long-term と td 0 の long_term0、list 1 の long-term、list 1 が空）、
  temporal（4 つの quadrant の mvL0・mvL1・refIdxL0・ref_id、co-located が list 1 だけ、intra の co-located、list 0 に無い frame で refIdxL0 0 と concealed、同じ frame が 2 つの時の最小の index、
  long-term と td 0 で mvL0 = mvCol、direct_8x8_inference 1 と 0、-1024 の係数と極値の vector の clamp）、
  spatial（MinPositive、単独一致、list を使わない・intra・利用不可の隣、両 list が -1 での 0・0 と directZero、C の代わりの D、colZeroFlag の各条件、long-term の list1[0]、inference 1 と 0、macroblock の 1 回の導出と spatial_ready の再導出）、
  direct の bit、誤りの入力（NULL、quadrant 4 以上、context の欠け、co-located frame の欠け・motion の欠け・address が範囲外、空の list、frame の無い entry、list の外の refIdx、巨大な refIdx）。
- 判断（header が書かない所）:
  - 床の右 shift は `direct_floor_shift`（負の値の `>>` を使わない）。式 8-198 と 8-199 の `>>` は仕様では算術 shift。
  - 誤りは書く前に全部検査し、EINVAL の時は ctx->n.current の motion と direct の bit を変えない（spatial の準備の ctx->direct の欄は更新されうる）。
  - spatial の refIdx が list の count 以上、または entry に frame が無い時は EINVAL（隣は同じ slice の物なので壊れた stream の時だけ）。
  - temporal の refIdxL0 は quadrant の最初の 4x4 block の co-located で決める（同じ quadrant の block は同じ refIdxCol を持つ）。list 0 に frame が無い時は ctx->concealed を quadrant ごとに 1 増やす（macroblock ごとではない）。
  - co-located の block が intra でなく両 list とも未使用の時は refIdxCol -1、mvCol 0 とする。
- 仕様と header の食い違い: 無し。
