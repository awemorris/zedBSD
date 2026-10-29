<!-- awesome-plan project=zedbsd record=ws326-p004 -->

# ws326-p004 slice header

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-30 Q1 の判定。実装と試験は Sonnet 5.5 high、3523197bd）。
- 依存: p003（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-slice.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 7.3.3・7.4.3（表 7-6）、7.3.3.1・7.4.3.1（表 7-7）、7.3.3.2・7.4.3.2、7.3.3.3・7.4.3.3（表 7-8・7-9）、7.4.1.2.4。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-slice.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-slice.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- I・P・B の slice header を bitwriter で組み（PPS と SPS は h264sw_ps_read_* で先に入れる）、全 field と data_position を確かめる。IDR と非 IDR、POC type 0・1 の field の有無。
- num_ref_idx_active_override、list の modification（idc 0・1・2 と終わりの 3）、weight の表（flag 0 の時の推定の値 2^denom と 0）、marking（IDR の 2 flag、adaptive の op 1〜6 と 0 で終わり、has_mmco5）。
- 誤り: PPS が無い（EINVAL）、first_mb_in_slice が PicSizeInMbs 以上、slice_type 10、IDR の P slice、SliceQPY 52、filter offset 7、cabac_init_idc 3、modification の個数超え、SP slice（ENOTSUP）。
- h264sw_slice_new_picture: 7.4.1.2.4 の各条件を 1 つずつ変えた組で 1、同じ組で 0、previous が NULL で 1。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh slice` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
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
- 実装: `userland/desktop/libmedia/h264sw-slice.c`（header は変えていない）。`h264sw_slice_read`（identity・order・ref count・modification・weight・marking・cabac_init・qp・deblocking の補助関数に分けた）と `h264sw_slice_new_picture`。
  試験: `plan/ws326/tests/unit/test-slice.c`（141694 checks）。手で符号化した IDR の 3 byte（17 bit、7.3.3 と 9.1 から紙で計算）、bitwriter で組んだ I・P・B（POC type 0・1・2、override、modification の idc 0〜2、weight の表と推定の値 2^denom、
  marking の IDR の 2 flag と op 1〜6、CABAC、deblocking）、誤りの全項目（PPS 無し、first_mb、slice_type 10、IDR の P、qp 52 と -1、filter offset 7、cabac_init_idc 3、modification 34 個、SP/SI の ENOTSUP、data partition の ENOTSUP ほか）、
  巨大な ue（0xFFFFFFFE）、全 prefix の切断、固定の乱数の bit 反転 20000 回と乱数 byte 列 20000 回（成功した時は範囲の不変条件を検査）、7.4.1.2.4 の条件を 1 つずつ変えた組。
- 実行した command と結果:
  - `timeout 600 sh plan/ws326/tests/run-unit.sh slice` → `test-slice: PASS (141694 checks)`、`run-unit: PASS`（ASan・UBSan の報告 0）。
  - `timeout 900 sh plan/ws326/tests/run-unit.sh all` → 全 9 本 PASS、最後の行 `run-unit: PASS`。
  - `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - zedBSD amd64 の `build/amd64/dynamic/libmedia.so` の make → exit 0、出力に warning 0。
  - `git diff --check` → 問題なし。
- 判断（header が黙っている所・読み方が二つある所）:
  - nal の type: 2〜4 は ENOTSUP（header）、1・5 以外の残り（0・6 以上・20・21 など）は呼び手の誤りとして EINVAL。
  - 検査の順: slice_type の範囲 → IDR は I か SI だけ（IDR の P・B・SP は EINVAL、IDR の SI は ENOTSUP）→ PPS と SPS の有無（EINVAL）→ SP・SI の ENOTSUP → first_mb（PicSizeInMbs 以上は EINVAL）。したがって PPS の無い SP slice は EINVAL。
  - num_ref_idx_active は header どおり 1..32 を受ける（7.4.3 は frame で 0..15 と言うが、header の `H264SW_MAX_LIST` と誤りの一覧は 32）。
  - modification: 個数が `H264SW_MAX_MODIFICATIONS`（33）を超えたら EINVAL（num_ref_idx_active との比較はしない）。最初の idc が 3（仕様は禁止）も空の list として受ける。abs_diff_pic_num_minus1（idc 0・1）は MaxPicNum（= MaxFrameNum）未満を要求して EINVAL にする（7.4.3.1、reflist の算術を守る）。
    long_term_pic_num は検査しない（DPB を要るため）。
  - marking: 終わりの op 0 は mmco 配列に保存せず mmco_count に数えない（modification の idc 3 と同じ扱い）。op 4 の max_long_term_frame_idx_plus1 は SPS の max_num_ref_frames 以下（7.4.3.3）を要求して EINVAL。
    difference_of_pic_nums_minus1・long_term_pic_num・long_term_frame_idx は検査せず uint32 のまま（DPB の側で int64 などで扱うこと）。IDR で nal_ref_idc が 0 でも検査しない（marking を読まない）。
  - idr_pic_id は 0..65535（7.4.3）、redundant_pic_cnt は 0..127 で EINVAL にする。frame_num は IDR で 0 でなければ EINVAL。
  - max_num_ref_frames が 0 の SPS の P・B slice（7.4.3 は禁止）は検査せず受ける。
  - 重み: luma_log2_weight_denom・chroma_log2_weight_denom は 0..7。weight・offset は -128..127（各値を読む時に検査）。flag 0 の時の推定の値は header のとおり（weight 1 << denom、offset 0、present 0）。weight の table の ref_idx が num_ref_idx_active 以上の entry は 0 のまま。
  - slice_qp_delta は int64 で足してから 0..51 を検査（int32 の溢れが無い）。filter offset は検査してから 2 倍にする。
  - `h264sw_slice_new_picture`: previous が NULL か current が NULL なら 1。sps が NULL の時は type 0 と type 1 の両方の order count の項目を比べる（pps_id が同じなら sps も同じなので、type が違う項目は両方 0 で一致する）。
  - 引数のどれかが NULL の `h264sw_slice_read` は EINVAL。header は先に memset 0 する（誤りで返った時の中身は未定）。
- 所見（直していない）: `h264sw.h` の `H264SW_MAX_LIST` の comment は frame slice の最長を 32 と書くが 7.4.3 は frame で 15（minus1）。header の誤りの一覧と整合させて 32 とした。
- 未実施: QEMU・実機の試験（この Phase の範囲外）、decoder 全体との結合。
