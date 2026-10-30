<!-- awesome-plan project=zedbsd record=ws326-p021 -->

# ws326-p021 decoder の全体の流れと最初の全体の試験

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-31 O-p021。cleared は Q1 が決める）。
- 依存: p002〜p020（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-decoder.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.4.1.2.3・7.4.1.2.4、8.1、8.2（全体）、C.4、design.md §2 の誤りと開始の方針。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-decoder.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 全体の試験の program `plan/ws326/tests/stream/decode-stream.c`（unit の外に置く: run-unit.sh の all は引数なしで流すため）: 引数の Annex B の file を h264sw_nal_find で分けて h264sw_decoder_nal に与え（EBUSY の前に出力を取る）、最後に flush、出力の frame を crop して I420 で frame ごとの MD5 を出す。
- 試料: plan/ws083/tests/streams/*.h264（6 本）、plan/ws236/tests/*.h264（2 本）、host の ffmpeg（libx264）で作る 352x288 の短い stream（CAVLC baseline、CABAC main の B、high の 8x8 と cqm、複数 slice、weighted の P）。
- 期待: host の `ffmpeg -i <file> -f rawvideo -pix_fmt yuv420p` の frame ごとの MD5（副の oracle、design.md §7）。全 frame が一致。
- 誤り: stream を任意の位置で切る・byte を壊す（固定の seed で 100 通り）で落ちない（ASan・UBSan）、次の IDR から正しく戻る。
- 開始: IDR の前の P を捨てる、reset の後。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 最初の全体の試験なので module の間の誤りがここで出る。この Phase に限り、他の module の h264sw-*.c の修正をしてよい（header は不可）。直した file・関数・理由を phase.md の結果に 1 つずつ書く。Q1 は Opus の担当（phase-runner）を推す。
- この試験の runner は `plan/ws326/tests/run-stream.sh`（新しく作る、decode-stream.c を ASan・UBSan で build、出力は build/tmp の fresh の directory、最後の行 `run-stream: PASS`）。ffmpeg が無い時は skip と書く。確かめの command に `timeout 1800 sh plan/ws326/tests/run-stream.sh` を足す。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。この Phase の h264sw-reflist.h の直しの後は `BASE=8ddb130a9`（結果の節）。
- `timeout 1800 sh plan/ws326/tests/run-stream.sh` → `run-stream: PASS`。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## p020 から引き継ぐ事（2026-10-30 Q1）

deblocking は chroma の QP に `picture->chroma_qp_offset[0]`・`[1]` を全 macroblock で使う（context に slice ごとの PPS の offset が無い）。1 つの picture の slice が chroma_qp_index_offset の違う PPS を指す stream があり得るか（7.4.1.2.1 などの制約）を統合で確かめ、あり得るなら macroblock ごとの値を持たせる直しを Q1 に返す。

## p006 から引き継ぐ事（2026-10-30 Q1）

- memory_management_control_operation 5 の picture は、`h264sw_dpb_store` を呼ぶ前に `frame->frame_num = 0` にする（7.4.3。DPB の項目の frame_num は `current->frame_num` から写る。decoder.h の手順 6 は POC の写しだけを書いている）。
- gap の非存在の frame の POC（type 0）は DPB の中の最も新しい frame の POC か 0（p006 の判断、phase006/phase.md の結果）。適合の試験の gap の stream（MR9_BT_B など）で出力の順を確かめる。

## p018 から引き継ぐ事（2026-10-30 Q1）

- CABAC の residual（coded_block_flag、8x8 の coded_flags の bit、mb_qp_delta の文脈、CABAC の I_16x16・I_4x4）は p018 の単体試験に無い。統合と適合（CABAC の stream）で確かめる。
- SP・SI の slice は ENOTSUP（範囲の外の profile）。

## p007 から引き継ぐ事（2026-10-30 Q1）

- **header と仕様の食い違い（直す事）**: h264sw-reflist.h は「非存在の frame も他と同じく list に入る」と書くが、8.2.4.2.3 は B の slice で pic_order_cnt_type 0 の時に非存在の frame を両方の list から除く。`h264sw_reflist_build` は POC type を知らないので、p007 は header に従った。仕様に合わせるには header が POC type（か「非存在を除く」の印）を渡す必要がある。Q1（か Opus）が p021 の前に header を直し、reflist の実装と test-reflist を合わせる（BASE の更新を記録する）。
- 1 の list の list 0 との比べと入れ替えは num_ref_idx_active での切り詰めの前（8.2.4.2 の順）。
- `h264sw_reflist_find` は欠けた位置を飛ばすので、代わりの entry だけが持つ frame は -1（direct は隠しとして数える）。

## 結果

- 日付: 2026-10-31（host の `date`）。担当 O-p021（Opus、Queue q1755-i01）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- commit: 8ddb130a9（header の直し、新しい BASE）、その後の commit（decoder と試験、Q1 への報告に SHA）。

### header の直し（Q1 の許可、この Phase だけ）

- `h264sw-reflist.h` の `h264sw_reflist_build` に引数 `unsigned poc_type`（active な SPS の pic_order_cnt_type）を足し、B の slice で type 0 の時に非存在の frame を両方の初期の list から除く（8.2.4.2.3）と comment に書いた。P の list と type 1・2 は今まで通り。
- `h264sw-reflist.c`: `reflist_collect`・`reflist_initial_b` に `skip_non_existing` を足した。`plan/ws326/tests/unit/test-reflist.c`: env に `poc_type`（既定 2 で従来の試験の意味を保つ）、新しい `test_b_non_existing`（type 0 で両 list から除く・長い list は missing で埋まる・type 1 と 2 は POC で並ぶ・P は type 0 でも含む）、乱数の試験でも `poc_type` を振る。
- 確かめ: `run-unit.sh reflist` → `test-reflist: PASS (281194 checks)`。除外を外す突然変異で `FAIL (5 of 281655)`。commit 8ddb130a9 の後 `BASE=8ddb130a9 check-skeleton.sh` → PASS、`git diff --stat fd1d6f88c 8ddb130a9 -- 'userland/desktop/libmedia/h264sw*.h'` → `h264sw-reflist.h | 11 +++++++----`（1 file、7 行足し 4 行消し）だけ。
- 他の header の不備は見つからなかった。

### 作った物

- `userland/desktop/libmedia/h264sw-decoder.c`: header の流れ（手順 1〜6）の実装。static の補助は rbsp の確保、AU の境の判定、parameter set、slice、SPS の活性化、pool と配列の確保、picture の開始、slice の decode、picture の終わり、concealment、picture の破棄、decode_order_times の時刻の入れ替え。
- `plan/ws326/tests/stream/decode-stream.c`（全体の試験の program。MD5 は RFC 1321 を自分で書き、既知の 2 例で自己検査）、`plan/ws326/tests/run-stream.sh`（ASan・UBSan で build、出力は `build/tmp/ws326-stream` の fresh の directory、ffmpeg が無い時は skip と書く）。

### 実装の判断（header が書かない所）

- 待ちの間に落とした picture は、最後に落とした slice の header を `previous` に置き（`waiting` 2 の印）、7.4.1.2.4 で同じ picture の slice を 1 度だけ数える。
- SPS の活性化: 別の SPS（id か世代が違う）の時は、古い frame を活性化の時点で出す（IDR で no_output_of_prior_pics_flag 1 なら捨てる）。IDR では C.4.4 が新しい picture の前に古い frame を全部出すので順は同じ。その上で `h264sw_dpb_init`。大きさ・crop・dpb_frames が変わる時だけ pool と配列を作り直す。よって `h264sw_dpb_remove` の `changed` は常に 0。
- 開始の picture（待ちの後の非 IDR の I）は frame_num の gap を作らない（比べる前の frame_num が無い）。
- leading の判定（POC が開始の picture より小さい）は operation 5 が POC を変える前の値で行う。operation 5 の picture は `h264sw_dpb_remove` の後・`h264sw_dpb_store` の前に `frame->frame_num = 0`（p006 から引き継ぐ事）。
- slice の中の誤りは `h264sw_decoder_nal` が EINVAL（slice_data の ENOTSUP はそのまま）を返し、slice_errors を数える。残りは picture の終わりに concealment。
- `h264sw_decoder_flush` も、出力の queue が空でない時は EBUSY（DPB の全部を入れる余地を保つため）。flush と reset は POC の状態と long-term の上限を初期化し、開始を待つ。
- NAL の type 11（end of stream）・12・13・19 は AU の境にしない（header の手順 2 の列挙に従う）。

### 試験（実行した command と結果、すべて host）

- `timeout 1800 sh plan/ws326/tests/run-stream.sh` → `run-stream: PASS`:
  - 22 本・408 frame が host の ffmpeg（7.1.5、`-f framemd5 -pix_fmt yuv420p -fps_mode passthrough`）の frame ごとの MD5 と全部一致。全 stream で concealed 0・slice_errors 0・refused 0。
    内訳: plan/ws083/tests/streams の 6 本（i-baseline-64 3、i-high-352-cqm 3、i-main-352-slices 3、p-baseline-64 10、pb-high-352-pyramid 15、pb-main-352 15）、plan/ws236/tests の 2 本（pb-high-1080 8、pb-high-352-pyramid 15）、
    ffmpeg（libx264）で作った 14 本（各 24 frame）: cavlc-baseline、cabac-main-b（temporal direct、b-pyramid、implicit の weighted bipred）、cabac-main-spatial、high-8x8-cqm（cqm=jvt）、high-cavlc-8x8、main-slices（4 slice）、main-qp45、high-qp8、open-gop、repeat-headers、repeat-headers-cavlc、weighted（explicit の weighted P）、weighted-cavlc、crop-360x200（frame cropping）。
    ffmpeg の trace_headers で、weighted_pred_flag 1・weighted_bipred_idc 2・direct_spatial_mv_pred_flag 0 と 1・memory_management_control_operation 1・transform_8x8_mode_flag 1・pic_scaling_matrix_present_flag 1 が実際に stream にある事を確かめた。
  - 誤り: `--fuzz`（seed 20261031、100 通り × 2 本: repeat-headers（CABAC）と repeat-headers-cavlc）。byte を変える・範囲を切り取る・末尾を切る。ASan・UBSan の報告 0、次の IDR の後の出力が元の decode と一致（CABAC 52 通り・CAVLC 55 通りで次の IDR があり全て一致）。
  - 開始: `--start`（各 picture から decode を始める、SPS・PPS は先に与える。repeat-headers 24・open-gop 24・pb-main-352 15 通り）。IDR からは全 frame 一致・dropped 0、非 IDR の I からは leading の picture（計 3）を落として残りが一致、P・B からは dropped > 0 で次の IDR から一致。reset の試験（前半を decode、reset、後半の picture から）も次の IDR から一致。
- `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 24 試験全部 PASS、`run-unit: PASS`。
- `BASE=8ddb130a9 timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so`（確かめの command のとおり）→ exit 0、出力の `warning` 0 件。
- `git diff --check` → 問題なし。

### 直した他の module

- `h264sw-reflist.c` の `h264sw_reflist_build`・`reflist_collect`・`reflist_initial_b`（上の header の直しに合わせて。理由は 8.2.4.2.3）。他の module は直していない（全体の試験で module の間の誤りは出なかった）。

### 引き継ぎの項目への答え

- p020（chroma の QP の offset）: 1 つの picture の slice が別の PPS を指すことは無い。7.4.1.2.4 は pic_parameter_set_id の違いを新しい picture の最初の VCL NAL の条件にしており、decoder はそれで picture を終える。PPS の中身も、PPS の NAL が AU の境（7.4.1.2.3）で picture を終えてから置き換わる。よって picture ごとの `chroma_qp_offset` で足り、macroblock ごとの値は要らない。
- p006: operation 5 の `frame_num = 0` は実装した（上）。gap の stream（MR9_BT_B など）での出力の順は未実施（cache に無い、p022 の適合で）。
- p018: CABAC の residual（coded_block_flag、8x8 の coded_flags、mb_qp_delta、I_16x16・I_4x4）は CABAC の stream 9 本（cabac-main-b、cabac-main-spatial、high-8x8-cqm、main-slices、main-qp45、high-qp8、open-gop、repeat-headers、weighted と ws083・ws236 の pb-* 4 本、どれも entropy_coding_mode_flag 1）が ffmpeg と一致したことで確かめた。SP・SI は ENOTSUP のまま。
- p007: header の直しは上。list 1 と list 0 の比べは切り詰めの前（reflist.c は既にそう）。

### 未実施・残り

- JVT の適合の bitstream（gap・long-term・ASO・PCM・MMCO の多く）は p022。cache の 3 本も流していない。
- 速さ（1080p の実時間）は測っていない（p024）。zedBSD の上（QEMU・実機）での decode は未実施（p023 の backend の後）。
- 要らなくなった出力: `build/tmp/ws326-stream.run.*`・`build/tmp/ws326-unit.run.*`・`build/tmp/ws326-skeleton.run.*` の古い run（この worktree の中、2026-10-31 03:40 に合わせて約 95 MB。最新の run は残す）。
