<!-- awesome-plan project=zedbsd record=ws326-p003 -->

# ws326-p003 共有の表と parameter set

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-30 p001 で作成）。
- 依存: p002（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high の subagent を 1 回起動（ws.md のユーザーの決定 2）。header は p001 の Opus の設計で、変えない。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 実装する header: `userland/desktop/libmedia/h264sw-tables.h`、`userland/desktop/libmedia/h264sw-ps.h`（各関数の comment が仕様。struct の field の意味と単位も header に）。使う側の header（`#include` の先）も読む。
6. 仕様: ITU-T H.264 (08/2021) の 表 7-2・7-3・7-4、表 8-13・8-14、表 8-15、式 8-313〜8-318、7.3.2.1.1・7.3.2.1.1.1・7.4.2.1.1、7.3.2.2・7.4.2.2、E.1.1・E.1.2・E.2.1（表 E-1）、A.3（表 A-1 の MaxDpbMbs）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- `userland/desktop/libmedia/h264sw-tables.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- `userland/desktop/libmedia/h264sw-ps.c`: 骨格（各関数の `ENOSYS` など）を header の仕様どおりの実装に置き換える。static の補助関数は自由（規約どおり前方宣言）。
- 単体試験 `plan/ws326/tests/unit/test-tables.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- 単体試験 `plan/ws326/tests/unit/test-ps.c`（`check.h`・`bitwriter.h` を使う。期待の値は仕様の表か、手で計算した値。他の decoder の source・出力を写さない）。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- tables: zig-zag の 4x4 の idx 0..15 と 8x8 の 0..63 が表 8-13・8-14 どおり（全要素を表の値で比べる）、どちらも 0..N-1 の置換であること。既定の list（表 7-3・7-4）の全値、QPC（表 8-15、qPI 29 → 29、30 → 29、51 → 39）、v（式 8-315・8-318）。
- ps: bitwriter で組んだ Baseline・Main・High の SPS（scaling list あり・なし、POC type 0・1・2、crop、VUI の aspect_ratio_idc 1・255、bitstream_restriction）を読み、全 field と派生の値（max_frame_num、size_mbs、crop の sample、dpb_frames）を確かめる。
- ps: 範囲外（log2_max_frame_num_minus4 13、pic_order_cnt_type 3、num_ref_frames_in_pic_order_cnt_cycle 256、crop で窓が空になる値）は EINVAL、幅 1056 macroblock 以上は ENOTSUP、profile 88・110、chroma_format_idc 0・2、bit_depth 9、frame_mbs_only 0 は ENOTSUP、どちらも store を変えない。途中で切れた SPS は EINVAL。
- ps: PPS の基本・transform_8x8_mode と second_chroma_qp_index_offset の有無（more_rbsp_data）・num_slice_groups 1 以上は ENOTSUP・weighted_bipred_idc 3 は EINVAL。
- ps: h264sw_ps_scaling の 4 通り（どちらも無し → 16、SPS だけ、PPS だけ（rule A）、両方（rule B））を手で組んだ list で確かめる。useDefaultScalingMatrixFlag（delta_scale で nextScale 0 を最初に作る）も。
- ps: dpb_frames の例: level 3.0（MaxDpbMbs 8100）の 720x576（1620 MB）→ 5、level 4.0 の 1920x1088 → 4、level_idc 9 と 11 + constraint_set3（1b）、表に無い level_idc → 16、VUI の max_dec_frame_buffering がある時はそれ。VUI の壊れ（途中で切れる・aspect_ratio_idc 17〜254）で SPS は残り vui_present 0。profile 88 の constraint_set0 1 は受け、0 は ENOTSUP、110 の 8 bit 4:2:0 は受ける。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- 表の値は PDF の頁の画で確かめる（pdftotext は v の列の順が崩れる）。block ごとに `Values referenced: ITU-T H.264 (08/2021) Table 8-13` の形の複数行の comment。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `timeout 600 sh plan/ws326/tests/run-unit.sh tables ps` → 最後の行 `run-unit: PASS`。前の Phase の試験も `sh plan/ws326/tests/run-unit.sh all` で PASS のまま。
- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。
