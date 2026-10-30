<!-- awesome-plan project=zedbsd record=ws326-p018 -->

# ws326-p018 slice data と macroblock の syntax

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-30、Q1 の受け入れ待ち。p001 で作成）。
- 依存: p002、p008、p009、p012（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-macroblock.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.4・7.4.4、7.3.5・7.3.5.1・7.3.5.2・7.3.5.3・7.3.5.3.1・7.4.5〜7.4.5.3.3（式 7-36・7-37）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-macroblock.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-macroblock.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- CAVLC: bitwriter で組んだ I slice（I_4x4・I_16x16・I_PCM の各 1 macroblock）、P slice（mb_skip_run、P_L0_16x16、P_8x8 の sub 4 種、P_8x8ref0）、B slice（B_Direct_16x16、B_8x8 の direct sub）を h264sw_macroblock_parse で読み、ctx->syntax と mb の field が期待どおり。
- transform_size_8x8_flag の読む条件（I_NxN、inter の cbp の後、direct_8x8_inference の条件）と CAVLC の 8x8 の 4 つの 4x4 の交互の並び（7.3.5.3.2 の lumaList）。
- CABAC: macroblock 全体の符号化を test に書くのは大きすぎるので、ここでは I_PCM の macroblock 1 つと end_of_slice_flag だけを `cabacwriter.h` で組んで読む。CABAC の macroblock の syntax の全体は p021 の stream の試験（CABAC の stream）と p022 の適合で確かめる。
- QP: mb_qp_delta の wrap（51 + 1 → 0、0 - 1 → 51）、範囲外 26 は EINVAL。I_PCM の qp 0。
- h264sw_slice_data は recon が骨格（ENOSYS）の間は p019 の後に全体で試す。この Phase では parse だけ（slice_data の loop は recon を stub にできないので、p018 の試験に回す）。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- h264sw_slice_data は h264sw_recon_mb を呼ぶ。recon が未実装の間は ENOSYS で止まるので、loop の試験は p019 の受け入れに含める。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh macroblock` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p012 から引き継ぐ事（2026-10-30 Q1、p012 の実装の担当の報告）

CABAC の syntax element の文脈（ctxIdxInc）は今の macroblock の状態を読むので、macroblock の解析はそれぞれを、それを使う次の要素を読む前に書く:
`kind`、`coded_flags`（16 = luma DC、17・18 = Cb・Cr の DC、`19 + 4*(component-1) + chroma4x4BlkIdx` = chroma AC）、`mvd`、`motion.ref_idx[list][quadrant]`、`direct`（ref_idx と mvd の前）、I_16X16 の `cbp`（mb_type から）。

## 結果

- 日付: 2026-10-30（host の `date`）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- 作った物: `userland/desktop/libmedia/h264sw-macroblock.c`（`h264sw_slice_data` と `h264sw_macroblock_parse`、CAVLC と CABAC の両方）、単体試験 `plan/ws326/tests/unit/test-macroblock.c`。header・他の module・Makefile・試験の道具は変えていない。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh macroblock` → `test-macroblock: PASS (1107981 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 600 sh plan/ws326/tests/run-unit.sh all` → 全 test PASS（test-cabac_se 168241・test-cavlc 87868・test-intra 593・test-macroblock 1107981・test-mb 1938・test-mvpred 83・test-nal 95・test-ps 41887・test-slice 141694・test-tables 846・test-transform 33158 など）、`run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so` → exit 0、出力の warning 0（h264sw-macroblock.o を clang の `-Wall -Wextra -Werror` で build した行を確認）。
  - `git diff --check` → 問題なし。cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で .c を単独 compile して OK。
- 試験の内容: CAVLC の I_4x4・I_16x16（DC・AC・chroma DC）・I_PCM（整列と 384 sample）・P slice の I_NxN（transform 8x8 で Intra_8x8、4 つの 4x4 の交互の並び）、P の 16x16・16x8・8x16・P_8x8（sub 4 種の mvd の位置）・P_8x8ref0、B の B_Direct_16x16・B_L0_Bi_16x8・B_8x8（direct・L1・Bi・L0 4x4 の sub）、skip（P_Skip・B_Skip・I で EINVAL）、transform_size_8x8_flag を読む・読まない全部の条件（I_NxN、inter の cbp 後、noSubMbPartSizeLessThan8x8Flag、direct_8x8_inference の 2 通り、chroma のみの cbp、PPS 無効）、QP の wrap（51+1→0、0-1→51 ほか）と範囲外 26・-27 の EINVAL、nC の列の切り替え（隣の total_coeff が 16・3 と 4・Cb の 8・Cr の 5・block 0 だけの 2）、誤りの入力（mb_type・sub_mb_type・ref_idx・chroma mode・cbp・mvd ±32768 の外・se(v) の最大・32 個の 0 の ue、mvd の範囲の端 32767・-32768 は成功で entry の絶対値は 255 で打ち切り）、途中で切れた入力（全 byte 長）、ランダムの入力（CAVLC 20000 本、CABAC 20000 本と slice_data の 6000 本。EINVAL か成功のみで成功時の entry の値が範囲内）。CABAC は仕様どおり I_PCM 2 つ（2 つ目の mb_type の context が 1 つ目の kind に依る）・end_of_slice_flag・P slice の skip 2 つ + P の中の I_PCM と、P_L0_L0_16x8 と P_L0_16x16 の 2 macroblock（ref_idx・mvd・cbp の context を手で導き、encoder と decoder の最後の context 変数が一致することで読む順と書く順の正しさを確かめる。第 2 partition と次の macroblock が前の値を読む）。
- slice_data の試験: 本物の `h264sw_recon_mb` は ENOSYS の骨格なので、試験は `h264sw-macroblock.c` をもう 1 度 `#include` し（public 関数は macro で名を替え、recon を stub にする。本物は link して parse の試験に使う）、loop を stub の recon で通す（skip run の途中・端・0、mb_skip_run が picture の外、重なる slice で entry を触らない、recon の失敗・parse の失敗で entry を slice -1 に戻す、picture の外へ続く slice、SP・SI は ENOTSUP）。この試験は p019 の後も変わらず通る（stub は試験の中）。
- 試験の確かめ（一時的な突然変異、git の外）: 実装に 29 通りの誤りを入れ、27 通りで test-macroblock が FAIL（I_16x16 の cbp 未記録、ref_idx の entry への書き忘れ、total_coeff の書き忘れ、chroma AC の nC の block、QP の wrap 無し・範囲 check 無し、8x8 flag の noSub・direct inference 条件の欠落、4x4 の交互の並びの誤り、skip run の reset 忘れ・上限無し、mvd の cap・範囲・書き込み位置、direct bit 未設定、PCM の整列、kind の記録の遅れ、cbp の列の取り違え、重なりの check、失敗した entry の reset、I_NxN の flag 無視、P_8x8ref0 で ref_idx を読む、prev_qp_delta_nonzero 未更新）。PASS のまま残ったのは 2 つ: I_PCM の entry の qp を 0 にする行を消す（reset がもう qp 0 にするので等価）と、CABAC の 8x8 transform の coded_flags の 4 bit を立てない（CABAC の residual は試験の範囲外、下の「未実施」）。chroma AC の nC は最初の突然変異で通ったので試験（Cb 8・Cr 5 の隣）を足して検出を確かめた。
- header が決めていない点で担当が決めた物（Q1 の確認用）:
  - 各 parse の最初に `ctx->syntax` の type・sub・mvd・ref_idx（全部 -1）・cbp・qp_delta・全 level 配列を既定値（0）にする。残差の無い block・skip・cbp 0 の macroblock でも level 配列は全部 0 になる（recon が cbp に頼らなくてよい）。I_PCM では level 配列を触らず pcm を使う。
  - `syntax.type.kind` も entry の kind も、I_NxN は I_4X4 か I_8X8 に解決した値で終わる（`syntax.type` の parts などは mb_type の表のまま）。
  - direct の sub-macroblock・B_Direct_16x16・B_Skip の `syntax.ref_idx` と entry の `motion.ref_idx` は -1 のまま（recon が埋める）。P_Skip も entry の ref_idx は -1 のまま。
  - ref_idx が送られず使われる list（active が 1、P_8x8ref0）は 0 を syntax と entry に書く。
  - mvd の entry の絶対値は partition が覆う全部の 4x4 block に入れる。
  - `h264sw_slice_data` は最初に prev_mb_addr -1、prev_qp_delta_nonzero 0、prev_skipped 0、skip_run -1 に自分で初期化する（decoder が先にしても害は無い）。SP・SI の slice_type は ENOTSUP（slice header が拒むので到達しない）。
  - CAVLC の I_PCM の pcm_alignment_zero_bit は読み飛ばして値を検査しない（CABAC の h264sw_cabac_pcm と同じ）。
  - `prev_qp_delta_nonzero` は parse が macroblock ごとの終わりで更新する（mb_qp_delta を読んだ後、skip と I_PCM では 0）。QPY が範囲 0..51 の外なら EINVAL。
  - CAVLC の mb_skip_run が `PicSizeInMbs - CurrMbAddr` を超えれば読んだ macroblock で EINVAL。run が使い切られた後に more_rbsp_data が真なら次の macroblock は run 無しで符号化された macroblock（7.3.4 の順）。CABAC の end_of_slice_flag 後に cabac.error が立てば EINVAL（その macroblock は decode 済みのまま）。
  - phase.md と仕様の食い違いは無し。header の不備・不足も無し（止める物は無かった）。
- 未実施: CABAC の residual（coded_block_flag・coded_flags の記録・8x8 の 4 bit・mb_qp_delta の context・I_16x16・I_4x4 の CABAC の流れ）の試験は phase.md の指定どおりこの Phase に含めず、p021 の stream の試験と p022 の適合で確かめる。recon を含む slice_data の全体は p019 の後。QEMU・実機の試験。規約の全文の見直しの Phase（WS の最後）。
