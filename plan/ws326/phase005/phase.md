<!-- awesome-plan project=zedbsd record=ws326-p005 -->

# ws326-p005 frame と POC

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30。cleared は Q1 が受け入れを見て決める）。
- 依存: p003、p004（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-frame.h`、`userland/desktop/libmedia/h264sw-poc.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 8.2.1・8.2.1.1・8.2.1.2・8.2.1.3（式 8-1〜8-12）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-frame.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- `userland/desktop/libmedia/h264sw-poc.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-frame.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- 単体試験 `plan/ws326/tests/unit/test-poc.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- frame: pool を作り get・ref・unref・再利用（id は毎回増える）、limit で NULL、範囲外の大きさで EINVAL、destroy の後も持っている frame は有効で最後の unref で解放（ASan の leak が無い）、fill_grey。
- poc type 0: lsb の wrap（MaxPicOrderCntLsb 16 で 14 → 2 の時 msb +16、2 → 14 で -16）、bottom の delta、非参照の picture は prev を変えない、IDR で 0 から。
- poc type 1: 手で計算した cycle（offset_for_ref_frame 2 個、offset_for_non_ref_pic -1）の 6 枚、num_ref_frames_in_pic_order_cnt_cycle 0 の場合、frame_num の wrap で FrameNumOffset が MaxFrameNum 増える、int64 を溢れさせる値で EINVAL。
- poc type 2: 参照 2*n、非参照 2*n - 1、wrap。
- mmco5 の後: poc_finish で top・bottom が tempPicOrderCnt を引いた値になり、次の picture の prev が 0 から。int32 を越える結果は EINVAL。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh frame poc` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-30（host の `date`）。
- 実装: `userland/desktop/libmedia/h264sw-frame.c`、`userland/desktop/libmedia/h264sw-poc.c`（header は変えていない）。
  試験: `plan/ws326/tests/unit/test-frame.c`（284 checks）、`plan/ws326/tests/unit/test-poc.c`（382 checks）。期待の値は 8.2.1 の式 8-3〜8-13 を紙で追った値
  （type 0 は MaxPicOrderCntLsb 16 の wrap と半分ちょうどの境、type 1 は offset_for_ref_frame { 4, 2 }・offset_for_non_ref_pic -1・offset_for_top_to_bottom_field 1 の 8 枚、frame_num の wrap 2 回、cycle 0 と 1 と 255 個、type 2 は 2n と 2n - 1 と wrap、
  mmco5 は type 0・1・2 の reset）。誤りは int64 の積と和の溢れ、int32 を越える結果、FrameNumOffset の範囲、巨大な poc_cycle_length・frame_num・poc_lsb・delta。固定の乱数 20000 組で EINVAL か成功だけを返すこと。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh frame poc` → `test-frame: PASS (284 checks)`、`test-poc: PASS (382 checks)`、`run-unit: PASS`（ASan・UBSan・leak の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 14 本 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `build/amd64/dynamic/libmedia.so` の make → exit 0、出力に warning 0。
  - `git diff --check` → 問題なし。
  - 変異試験（git の外で実装を壊して戻した）: wrap の >= と >、type 0 の msb の符号、absFrameNum の -1、type 2 の -1、prevFrameNumOffset の mmco5 の 0、mmco5 後の prev_lsb、非参照で prev_ref_frame_num を動かす、int64 の積の検査の削除、mmco5 後の int32 検査の削除、
    finish の clamp を cast に、id の増分、limit の境、灰の値、dead の pool の解放、二重 unref の保護、plane の配置、のどれも試験が FAIL した。捕まらなかった変異は motion の memset を 1 byte 短くする物だけ（最後の record の padding で、観測できない）。
- 判断（header が黙っている所・読み方が二つある所）:
  - frame pool: crop の幅・高さが 0 は EINVAL（SPS が空の window を拒む）。`pool` が NULL も EINVAL。id の counter は 0xFFFFFFFF の次を 1 にする（0 は使わない）。
    get は dead の pool と NULL の pool で NULL。refs が 0 の frame への unref は無視（二重 unref で free list に二重に載せない）。get のたびに crop を pool から入れ直す。`fill_grey` は non_existing を触らない（呼び手が立てる）。
    plane は 1 回の calloc（Y の後に Cb、Cr）、stride は幅と幅/2。
  - poc compute: sps の poc_type が 3 以上、log2_max_poc_lsb が 4..16 の外、poc_lsb が MaxPicOrderCntLsb 以上、frame_num が max_frame_num 以上、poc_cycle_length が 255 より大きい、は EINVAL。引数が NULL も EINVAL。
    EINVAL の時は `*poc` を書かない。PicOrderCntMsb・FrameNumOffset（0..2^31-1）・top・bottom が int32 の外は EINVAL（8.2.1）。
    mmco5 の picture は、finish が int32 の中に収まるよう compute の時点で `top - min` と `bottom - min` が 2^31-1 を越えれば EINVAL にした（finish は void で誤りを返せないため。phase.md の「int32 を越える結果は EINVAL」はこの形で満たす）。finish は念のため clamp も持つ。
    type 1 の ExpectedDeltaPerPicOrderCntCycle は SPS の `expected_delta_per_poc_cycle` をそのまま使い（offset から再計算しない）、cycleCnt * それ は検査付きの int64 の積。途中の値は int32 に収めず、最後の top・bottom だけを検査する。
    prevFrameNumOffset は `prev_mmco5` が立てば 0（finish も prev_frame_num_offset を 0 にするので二重に守る）。
  - poc finish: type 0 の PicOrderCntMsb は header の構造体に無いので `top - pic_order_cnt_lsb` から戻す（compute が msb を int32 に検査済みなので正確）。type 1・2 では prev_msb・prev_lsb に触らない。
    mmco5 の picture は nal_ref_idc を見ずに reset する（mmco5 は参照 picture にだけ在る）。
- 所見（直していない）: frame_num の gap で作る non-existing frame を decoder が `h264sw_poc_finish` に通すと、type 0 では prev_lsb・prev_msb が合成の header の poc_lsb（0）に変わってしまう（8.2.1.1 は non-existing frame を前の参照 picture にしない）。
  gap の frame の処理（後の DPB の Phase）は type 1・2 の prev_frame_num と prev_frame_num_offset だけを進める（finish を type 0 では呼ばない、または呼ぶ前後で prev_msb・prev_lsb を保存して戻す）必要がある。header は変えていない。
- 未実施: QEMU・実機の試験（この Phase の範囲外）、decoder 全体との結合、frame_num の gap の処理（DPB の Phase）。
