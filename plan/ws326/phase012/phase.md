<!-- awesome-plan project=zedbsd record=ws326-p012 -->

# ws326-p012 CABAC の syntax element

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、6115b8dcd）。
- 依存: p008、p011（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-cabac-se.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.5.3.3、9.3.2（表 9-34〜9-38）、9.3.3.1・9.3.3.1.1.1〜9.3.3.1.1.10・9.3.3.1.2（表 9-39）・9.3.3.1.3（表 9-40〜9-43）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-cabac-se.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-cabac_se.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 各 element を p011 の `cabacwriter.h`（9.3.4 の符号化 engine）で、決めた neighbour の状態（struct h264sw_mb を test で埋める）の下に、仕様の 2 値化と ctxIdx で符号化し、復号で値が戻ること。ctxIdxInc の各条件（隣が無い・skip・intra・I_PCM・direct・cbp の bit）を 1 つずつ変える。
- mb_type: I の全 26 値、P と B の全値（intra の 5+・23+ を含む）。sub_mb_type 全値。cbp の全 48 組（隣の cbp で ctxIdxInc が変わる組を含む）。mb_qp_delta の -26〜25 と範囲外。ref_idx の 0〜上限と上限超え。mvd の 0・±1・±8・±9・±100・±8191、範囲外。
- residual: cat 0〜5 の各々で、決めた係数の列（0 だけ、最後だけ、全部、大きな level 2^15-1）を往復。cat 5 は coded_block_flag 無し。significant・last の ctxIdxInc（cat 5 は表 9-43 の frame の列）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 試験の 2 値化は test の中に仕様の 9.3.2 から書く（ctxIdxInc は test が決めた neighbour の状態から手で決めた値を使う）。decoder の側の code を流用して符号化を書かない（同じ誤りで往復が通ってしまう）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh cabac_se` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

2026-10-30、worktree `/home/awe/zedBSD-worktrees/s-ws326-p012`（branch `agent/s-ws326-p012`）。

- 実装: `h264sw-cabac-se.c` に header の全関数（`h264sw_cabac_mb_skip`・`_mb_type`・`_sub_mb_type`・`_transform_8x8`・`_intra_mode`・`_chroma_mode`・`_ref_idx`・`_mvd`・`_cbp`・`_qp_delta`・`_end_of_slice`・`_residual_block`）を入れた。header は変えていない。
  - 2 値化は仕様の 9.3.2（表 9-36〜9-38、UEGk の式 9-6）、ctxIdx は表 9-34・9-39・9-41、ctxIdxInc は 9.3.3.1.1.1〜9.3.3.1.1.10 と式 9-7〜9-24、residual は 7.3.5.3.3 と表 9-40・9-42・9-43（frame の列、ctxBlockCat 5 は表 9-43 の significant の列と last の列）。表 9-40 と表 9-43 は block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の comment を付けた。
  - 表 9-43 の 63 行 x 2 列は、PDF の頁の画（PDF 291・292 頁）と `pdftotext -layout` の出力から scratch の script（`python3 -I`、commit していない）で抜き出した値とを照合した。試験側にも別に写しを持つ。
  - ref_idx と mvd の neighbour は `h264sw_blk4x4_at` と `h264sw_mb_neighbour_4x4`、cbp の luma は `h264sw_mb_neighbour_8x8`、coded_block_flag の chroma AC は `h264sw_mb_neighbour_chroma` を使う。
- 判断（header が書いていない所）:
  - `h264sw_cabac_mb_skip` は P・B 以外の slice 型で `ctx->cabac.error` を立てて 0 を返し、読まない。`h264sw_cabac_mb_type` は I・P・B 以外で EINVAL（*mb_type 0）、`h264sw_cabac_sub_mb_type` は P・B 以外で EINVAL。P・B の mb_type と sub_mb_type は 2 値化の表にある全ての bin 列が値を持つので、表に無い bin 列の EINVAL は起こらない。
  - `h264sw_cabac_ref_idx` と `h264sw_cabac_mvd` は、list・component が 1 より大、x・y が 16 以上、num_ref_idx_active[list] が 0（ref_idx）の時、読まずに EINVAL（出力は 0）。ref_idx は 1 の bin を読んで値が num_ref_idx_active に達した時点で EINVAL。
  - `h264sw_cabac_residual_block` は、cat が 5 より大、cat ごとの blk・component・max_coeff（16・15・16・4・15・64、cat 3・4 の component は 1 か 2、他は 0）が合わない、start が end より大、end が max_coeff 以上の時、読まずに EINVAL（levels に触れず *coded は 0）。level の範囲外は EINVAL で、levels は読んだ所まで（残りは 0）。範囲は -32768〜32767（正の 32768 は EINVAL、負の 32768 は可）。sign の bypass を読んだ後で検査する。
  - mvd の範囲は -32768〜32767（量子化 1/4 sample）。正の 32768 と負の 32769 以下は EINVAL。EG の unary 部が 24 より多い 1（25 個目の 1）で EINVAL、24 個までは読む。
  - mb_qp_delta の mapped 値 51（+26）は EINVAL、52（-26）は可、53 以上は 1 を 53 個目まで読んだ所で EINVAL。ctxIdxInc は `ctx->prev_mb_addr`（-1 か picture の size_mbs 以上は「前の macroblock 無し」で 0）の entry を `ctx->picture->mbs` から読み、skip・I_PCM・「Intra_16x16 でなく cbp が 0」を 0、他は `ctx->prev_qp_delta_nonzero` に従う。
  - neighbour の判定は、header が書く entry の値に加えて kind も直接見る（skip は kind が P_SKIP か B_SKIP、intra は kind が H264SW_MB_P_SKIP 未満、B_Direct_16x16 は kind、direct 化した 8x8 は `direct` の bit）。coded_block_flag の I_PCM の neighbour は kind から 1、skip は 0、unavailable は current の kind が intra なら 1、inter なら 0。それ以外は `coded_flags` の bit（cat 0 は 16、cat 3 は 17 と 18、cat 4 は 19 + 4 * (component - 1) + chroma4x4BlkIdx、cat 1・2 は luma4x4BlkIdx）。
  - 後の macroblock の phase への条件（header にある物の確認）: 同じ macroblock の中の先の block の `coded_flags`・`mvd`・`motion.ref_idx[list][quadrant]`・`direct` と current の `kind` は、次の element を読む前に parser が書く。I_16X16 の `cbp` は mb_type から導いた値（luma 0 か 15、chroma 0〜2）が入っている。
  - ctxBlockCat 3 の coeff_abs_level_minus1 の ctxIdxInc の `4 - 1`（式 9-24）は、DC が 4 係数なので numDecodAbsLevelGt1 が 3 を超えず、式の通りに書いたが 4 の場合と区別できない（突然変異で確かめて同値と分かった）。
- 試験 `plan/ws326/tests/unit/test-cabac_se.c`（`check.h`・`bitwriter.h`・`cabacwriter.h` を使う）:
  - 2 値化は test の中に、仕様の表 9-36・9-37・9-38 の bin 列の文字列と式 9-6 の疑似 code から書いた。ctxIdxInc は neighbour の状態（struct h264sw_mb を 3 x 3 の picture に置く）ごとに手で決めた値で、表 9-39・9-41 の ctxIdx も test 側に別に持つ。decoder の code を使わない。
  - context variable は乱数の状態から始め（初期化表に頼らない）、encoder と decoder に同じ初期状態を与える。各 stream の最後に decoder と encoder の全 context variable（460 個）が一致することを検査する（別の context を使えば状態がずれる）。
  - mb_skip（P・B）、mb_type（I の 26 値・P の 0〜3 と 5〜30・B の 0〜48 を、neighbour の状態 9〜10 通りの各々で。I_PCM は terminate bin）、sub_mb_type（P 0〜3・B 0〜12）、transform_8x8（可用性 x flag の全組）、intra mode（rem 0〜7）、chroma mode（隣が無い・intra・I_PCM・inter）、ref_idx（隣が無い・skip・intra・I_PCM・direct の quadrant・ref 0・-1・list 1・current の中の neighbour、上限 2〜32 の各々で 0〜上限 -1 と上限）、mvd（0・±1・±2・±3・±7〜±13・±100・±8191・±16383・32767・-32768 など 41 値を 14 の neighbour の状態と 2 成分・2 list で、手で決めた neighbour の和 0・2・3・32・33・255）、cbp（48 値 x 11 x 11 の neighbour の状態、手で計算した ctxIdx 3 例）、qp_delta（-26〜25 を 13 の前の macroblock の状態で）、end_of_slice、residual（cat 0〜5 の各々で 0 だけ・最後だけ・最初だけ・真ん中・全部・全部 ±1・大きさの列・32767 と -32768・乱数 60 通り、start・end が内側の block、1 位置だけの block、coded_block_flag の ctxIdxInc 48 例を手で（各例で block に level がある場合と無い場合））。
  - 誤り: 範囲外の arguments（読まない）、ref_idx が上限（EINVAL）・200 個の 1、mvd の ±32768 近辺・巨大な値・EG の unary 25 個以上、qp_delta の 51・53・54・100・5000 個の 1、residual の level の範囲外・EG の unary が長いこと（6 つの cat）、3 byte に切った stream（error が立つ）、乱数の byte と乱数の neighbour 1500 本で各 element を 3 回ずつ（値の範囲・戻り値・ASan・UBSan）。
  - mutation の確認（decoder を一時的に変えて test が FAIL になること。元に戻した）: ref_idx の bin 1 の ctxIdx・表 9-43 の last の 1 値・mb_qp_delta の「Intra_16x16 でなく」の条件・mvd の prefix の ctxIdx・coded_block_flag の skip の neighbour・B の mb_type の bin 2 の ctxIdx・P の mb_type の bin 2 の条件・mb_qp_delta の +26・chroma の cbp の bin 1 の条件・B の sub_mb_type の bin 2・mvd の和の 2 の境・rem_intra_mode の bin の順・I の mb_type の pred の ctxIdx・direct の quadrant の判定、の 13 件が全て FAIL（上の chroma DC の `4 - 1` だけは同値で FAIL にならない）。
- 確かめた command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh cabac_se` → `test-cabac_se: PASS (168241 checks)`、`run-unit: PASS`。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → test-bits PASS (169)・test-bitwriter PASS (14)・test-cabac PASS (195654)・test-cabac_init PASS (4159)・test-cabac_se PASS (168241)・test-mb PASS (1938)・test-nal PASS (95)、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - amd64 の `build/amd64/dynamic/libmedia.so`（Phase の command どおり）→ exit 0、warning 0。
  - `git diff --check` → 出力なし（commit 前の追跡済みの変更）。clang でも test-cabac_se.c は `-std=c89 -Wall -Wextra -Werror -Wpedantic` で通る。
- 未実施: 実行時の QEMU・実機の試験（この Phase の範囲外）、macroblock の syntax との結合（p018）、conformance の stream との照合、速さの測定（bin ごとに関数を呼ぶ形で、速さの Phase で見直す）。
