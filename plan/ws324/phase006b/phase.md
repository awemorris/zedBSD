<!-- awesome-plan project=zedbsd record=ws324-p006b -->

# ws324-p006b H.265: h265sw-nal・h265sw-scaling・h265sw-vui の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: cleared（2026-11-02 01:03 Q1）。前: in-progress（2026-11-02 Sonnet q1879-i01 が実装済み・試験 PASS。cleared は Q1 の受け入れ後）。2026-11-01 P1 が p006a で作成

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D4・D12・§5・§7・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）。
- `plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」と「開発の管理情報」。
- `docs/agent/spec-module-delegation.md` の §5（実装の担当が守ること）と §7（出典と license）。
- 設計の §1・§4（D1・D4・D12）・§5.1〜§5.2・§6〜§8。
- 実装する header（仕様の正本）: `userland/desktop/libmedia/h265sw.h`、`h265sw-nal.h`、`h265sw-scaling.h`、`h265sw-vui.h`。使う物: `vcodec.h`・`vcodec-bitreader.h`・`vcodec-nal.h`（`vcodec_nal_unescape`）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`）。**印刷の頁 N = PDF の頁 N + 18**。`pdftotext -layout -f N -l M`、表は Read の tool で PDF の頁の画も見る。
  - h265sw-nal: 7.3.1.1・7.3.1.2（PDF 52）、7.4.2.1〜7.4.2.2 と Table 7-1（PDF 82〜86）、7.4.2.4.4（PDF 88〜89）。
  - h265sw-scaling: 7.3.4（PDF 63）、7.4.5 と Table 7-5・7-6（PDF 110〜112）。
  - h265sw-vui: E.2.1〜E.2.3（PDF 445〜448）、E.3.1（Table E.1 を含む、PDF 448〜466）、E.3.2・E.3.3（PDF 466〜469）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-nal.c`、`h265sw-scaling.c`、`h265sw-vui.c`（骨格を実装で置き換える）。
- `plan/ws324/tests/unit/test-nal.c`、`test-scaling.c`、`test-vui.c`。
- この phase.md の「結果」の節、`plan/ws324/ws.md` の Phase の表のこの行（「実装済み・試験 PASS」。cleared は Q1）。

## 3. 試験の項目

期待の値は仕様の表・式と手の計算だけから作る（他の実装の出力を使わない）。入力は `plan/ws324/tests/unit/h265write.h`（NAL の header・emulation prevention、VUI・HRD・scaling list の書き手）と `plan/tools/h264sw/unit/bitwriter.h` で作る。

- test-nal:
  - header の各 field（type 0〜63、layer 0〜63、temporal_id_plus1 1〜7）の読み。forbidden_zero_bit 1、temporal_id_plus1 0、IRAP（16〜23）で TemporalId が 0 でない、TSA（2・3）で TemporalId 0 の各 EINVAL。size 0・1 の EINVAL。
  - 分類の関数 9 つを type 0〜63 の全値で、Table 7-1 から手で書いた表と照合。
  - RBSP の写し: emulation prevention の 0x03 の除去（先頭・途中・末尾・連続）、末尾の 0x000003 の扱い、`vcodec_nal_unescape` が拒む並び（0x000000・0x000001・0x000002・0x000003 の後に 3 より大きい byte）の EINVAL、capacity の不足の ENOBUFS。
  - `h265sw_nal_parse_prefix`: limit の全ての値（1〜payload の長さ + 1）で、complete と rbsp_size が `h265sw_nal_parse` の結果の先頭と一致（0x03 を跨ぐ切り口を含む）。limit 0 の EINVAL。
  - `h265sw_nal_starts_access_unit` を 0〜63 の全値で 7.4.2.4.4 の一覧と照合。
  - 乱数の byte 列（10 万通り）で落ちない。
- test-scaling:
  - `h265sw_scaling_default` の全値を、Table 7-5・7-6 から 2 つの独立した方法（`pdftotext -layout` の text と、頁の画の目視）で取った表と照合。使わない entry（sizeId 3 の matrixId 1・2・4・5、sizeId 0 の i ≥ 16）が 0。
  - 往復: 全 sizeId・matrixId で、pred_mode 0 の delta 0（既定の写し、DC 16）・delta 1 以上（参照の写し、DC も、sizeId 3 の refMatrixId は 3 刻み）、pred_mode 1（DC あり・無し、nextCoef の 256 の巡回の上下）を書いて読み、手で計算した list と DC に一致。
  - 範囲の端: pred_matrix_id_delta が matrixId（sizeId 3 は matrixId / 3）で可・+1 で EINVAL、dc_coef_minus8 の -7・247 で可・-8・248 で EINVAL、delta_coef の -128・127 で可・±1 外で EINVAL、nextCoef が 0 になる entry の EINVAL。
  - 途中で切れた RBSP の全ての切り口で EINVAL。
- test-vui:
  - E.2.1 の全分岐（各 present の flag の 0・1、EXTENDED_SAR、colour あり・無し、chroma_loc、default display window、timing と poc_proportional、HRD、bitstream_restriction）の往復で全 field が一致。無い時の推定の値（video_format 5、colour 2、motion_vectors_over_pic_boundaries 1、max_bytes_per_pic_denom 2、max_bits_per_min_cu_denom 1、log2_max_mv_length 15）。
  - 導いた sar_width・sar_height を Table E.1 の 1〜16 の全値、255、0、17〜254、255 で 0 の項の各場合で手の表と照合。
  - HRD: common_present 1・0（previous の写し）、previous NULL の EINVAL、sub_pic あり・無し、NAL・VCL の組 4 通り、max_sub_layers_minus1 0〜6、cpb_cnt_minus1 0・31・32（EINVAL）、fixed_pic_rate の 3 つの組と推定、elemental_duration 2047・2048（EINVAL）、bit_rate_value_minus1 の 2^32 − 2・ue の上限、cbr の bit。
  - 範囲の外: chroma_sample_loc 6、min_spatial_segmentation_idc 4096、max_bytes_per_pic_denom 17、log2_max_mv 16、num_units_in_tick 0、time_scale 0 の EINVAL。
  - 途中で切れた RBSP の全ての切り口で EINVAL、乱数の入力で落ちない。
- 試験が通った後、実装を git の外で数通り壊し（例: Table 7-6 の 1 値、7-42 の 3 刻み、HRD の common の写し、IRAP の TemporalId の検べ）、FAIL を確かめて戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- `vcodec_bitreader_*` の誤りは sticky。値を index・回数に使う前に `error` を確かめる。
- Table 7-6 は 2 つの行（matrixId 0〜2 と 3〜5）が 16 列ずつ 4 段に折り返す。`pdftotext` は列の値を落とすことがある（WS326 の教訓）ので、画でも照合し、どちらで取ったかを結果に書く。
- scaling list の配列は spec の ScalingList[ sizeId ][ matrixId ][ i ] の i の順（対角の走査の順）のまま。行列（ScalingFactor）にはしない（D5、この module の外）。
- VUI の `def_disp_win_*` は chroma の単位のまま保つ（変換しない）。
- HRD の `struct h265sw_hrd` は約 7 KiB。stack に置いてよいのは試験だけ。
- `h265sw_nal_parse_prefix` の切り口で 0x00 0x00 の後に 0x03 が切られる場合と、切り口の直前が 0x03 の場合を両方扱う。

## 5. 変えてはいけない物

全ての header（`h265sw*.h`・`h265-*.h`・`vcodec*.h`）、他の module の .c、Makefile・`sources.mk`、試験の道具（`run-unit.sh`・`check-skeleton.sh`・`unit/h265write.h`・`plan/tools/h264sw/unit/`）、HAL・toolchain・共有の build。header の不備を見つけたら直さずに止め、どの header のどの関数で何が足りないかを報告する。header が黙っている所は自分で決め、結果の節に書く。phase.md が仕様と食い違えば仕様に従い、報告する。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh nal scaling vui
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

（この module 群は libmedia の `sources.mk` に入らない。target の build の代わりに check-skeleton が cc と clang の両方で警告 0 を確かめる。）

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。
- 規約の checklist（coding-style §14）で自分の diff を見直した。表の値の block ごとに「Values referenced: ITU-T H.265 (01/2026) Table …」の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 結果の節: 日付（host の `date`）、command と最後の行、check の件数、自分で決めた点、未実施、表の値の照合の方法。ws.md の行を「実装済み・試験 PASS」。

## 結果

実装の担当（Sonnet 5.5、q1879-i01、worktree `s-ws324-p006b`）、2026-11-02（host の `date` で 00:50 JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265sw-nal.c`・`h265sw-scaling.c`・`h265sw-vui.c`（骨格を実装で置き換え）。header・道具は変えていない（check-skeleton の BASE 照合 PASS）。
- `plan/ws324/tests/unit/test-nal.c`・`test-scaling.c`・`test-vui.c`。

### 確かめた command と最後の行（2026-11-02 00:50）

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh nal scaling vui` | `test-nal: PASS (816099 checks)`、`test-scaling: PASS (5499 checks)`、`test-vui: PASS (2432070 checks)`、`run-unit: PASS` |
| `sh plan/ws324/tests/run-unit.sh all` | h265write 51 checks を含め全部 PASS、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（zedBSD の clang 3 target を含む。h265-std は Vulkan の header が tree に無いため飛ばし） |
| `git diff --check` | 出力なし（終了 0） |

試験は ASan・UBSan 付き（`-std=c89 -Wall -Wextra -Werror`）。

### 試験の中身（要点）

- test-nal: header の全組（type 64 × layer 64 × temporal_id_plus1 1〜7、禁止の組の EINVAL）、forbidden_zero_bit・plus1 0・size 0/1・NULL・上限超の EINVAL、分類 9 関数を type 0〜69 で手で書いた集合と照合（`starts_access_unit` は 7.4.2.4.4 の一覧）、unescape の手の例 8 通りと拒む並び 8 通り、capacity の比較（size - 2、ENOBUFS が escape の数に依らない）、`parse_prefix` を 400 本の乱数の NAL（0 を多く含む）と手の 2 本で limit 1〜payload+2 の全部、本体の写しの先頭との一致と、この file の別の簡単な除去との一致、limit 0 の EINVAL、乱数 10 万通り（ちょうどの大きさの heap で ASan）。
- test-scaling: 既定の表を Table 7-5・7-6 と照合、往復（signalled の乱数・極値の list 300 回、既定・参照・signalled の混合 400 回で期待を手の規則（7-42・7-43、DC の推定）で作る）、手の値（+127・-127 の巡回、DC 255 + 2、DC 1 - 128）、範囲の端（pred_matrix_id_delta・dc・delta_coef・entry 0）、全ての切り口。
- test-vui: 全て absent の VUI（10 bit と推定の値）、aspect_ratio_idc 0〜255 の SAR を Table E.1 の手の表と照合（255 の 0 の項を含む）、乱数の完全な VUI 4000 回と present flag の単独、HRD の全組（NAL・VCL・sub_pic 8 通り × sub-layer 0〜6）、共通部の引き継ぎ 300 回、previous NULL の EINVAL、cpb_cnt_minus1 0・31・32・UE_MAX、elemental_duration 2047・2048、2^32 - 2 と 2^32 - 1、cbr bit、範囲の外の各 field、全ての切り口、乱数 10 万通り。

### 表の値の照合の方法

- Table 7-6（既定の list）: 方法 1 は `pdftotext -layout -f 112 -l 112` の text から script で行を取り（4 行 × 16 = 64 値、intra と inter）、試験の表にした。方法 2 は頁の画（`pdftoppm -r 130` の PDF 頁 112）を目視で読み、実装の表と照合した。2 つは一致。実装の表は方法 1 の text を見て手で書き、方法 2 の画で確かめた物。
- Table E.1（SAR）: `pdftotext` の text（PDF 頁 449）から 16 行を読み、試験にも実装にも書いた（頁の画での照合は未実施。値は 1:1・12:11・10:11・16:11・40:33・24:11・20:11・32:11・80:33・18:11・15:11・64:33・160:99・4:3・3:2・2:1）。
- Table 7-5 は 16 個の 16。

### わざと壊す確かめ（git の外で数通り、戻して `git diff --quiet` で戻りを確認）

- 37 通り（nal 10、scaling 11、vui 16。再実施を含め 40 回流した。script は `build/tmp/p/mutate.py`・`mutate2.py`）: IRAP の TemporalId の検べ（`> 8U` に）、TSA の検べ、layer_id の shift、IRAP 上限、SLNR 上限、起動 NAL の上限、decodable の範囲、ENOBUFS の比較、`complete` の境、PPS の AU 開始、Table 7-6 の 2 値、参照の 3 刻み（7-42）、nextCoef の 256、DC の上限、entry 0 の検べ、sizeId 3 の delta 上限、既定 DC、参照の DC の写し、delta の下限、SAR の 160:99、HRD の長さ推定 23、within_cvs の推定 1、共通の写し 2 か所、elemental の上限、cpb_cnt の上限、cbr bit、reserved の SAR 上限、min_spatial の上限、chroma_loc の上限、max_bits の推定、time_scale の 0 の検べ、def_disp_win の上限、motion_vectors の推定、VUI の最後の error の検べ。
- 全部が build の誤りでなく試験の失敗（`FAIL` で `FAIL (build)` でない）で落ちた。ただし次の 3 点は最初の実行で例外だった（1 つ目は build の誤り、2 つ目は見逃し、3 つ目は壊しでなく実装の誤り）:
  - IRAP の検べを `0 &&` にした壊しは `-Werror` の build の誤り（試験の失敗でない）→ `> 8U` に変えて再実施、試験の失敗で落ちた。
  - `SCALING_DC_MINUS8_MAX` を 248 にした壊しを見逃した（DC 248 の後の entry が 0 になって別の検べで拒まれていた）。test-scaling に「DC -8・248 の後に最初の delta +1」の検べ（5467 → 5499 checks）を足し、DC の上限・下限の両方の壊しを試験の失敗で捕まえた。
  - 実装の誤りを試験が捕まえた 1 件: VUI の最後の flag（bitstream_restriction_flag）の読みが切れても返り値が 0 になる誤りを、切り口の試験が見つけた。実装に最後の `bits->error` の検べを足して直した（壊しの確かめでその検べを抜くと落ちる）。

### 自分で決めた点（header が黙っている所）

- `h265sw_nal_parse`・`_prefix`: `out` が NULL は EINVAL。NAL の size が H265SW_MAX_NAL_BYTES 超は、ポインタを読む前に EINVAL。`nal` が NULL で size が 2 以上も EINVAL。header の検べ（EINVAL）を capacity の検べ（ENOBUFS）より先にした。`out` は成功の時だけ書く。STSA の TemporalId 0 は仕様の「nuh_layer_id 0 の時に 0 でない」（7.4.2.2）だが header の comment が IRAP と TSA だけを挙げるので検べない。
- `h265sw_nal_parse_prefix`: 切り口が 0x000003 で終わる時の 0x03 は、`vcodec_nal_unescape` の既存の規則（最後の 0x03 を落とす）に任せ、結果が常に全体の RBSP の先頭になることを試験で確かめた。payload が limit ちょうどの時は complete 1。
- `h265sw_scaling_read`: 構造体全体を 0 で埋めてから読む。`h265sw_scaling_default` も全体を 0 で埋める。
- `h265sw_vui_read_hrd`: `hrd == previous` も EINVAL にした（header は「同じ物にしない」とだけ書く）。common_present 0 の時は previous の共通部（presence flag・sub_pic の fields・scale・長さ 3 つ）を全部写す。HRD の全 field の検べは値を使う前に `bits->error` を見る。
- `h265sw_vui_read`: `bits` と `vui` が NULL、max_sub_layers_minus1 が 6 超は EINVAL。aspect_ratio_idc 17〜254 は signalled として保ち、sar_width・sar_height は 0（header どおり）。video_format 6・7 や colour の予約値は signalled のまま保つ（変換しない）。
- phase.md の「試験の項目」の cbr: 「cbr の bit」は cbr_flags の bit j として試験した。

### 未実施

- 適合の stream（HEVC_v1）の読み（p006h の範囲）。QEMU・実機（この Phase の範囲外）。Table E.1 の頁の画での照合。
- 規約の全文の見直し（WS324 p015）。coding-style の checklist で自分の diff は見直した（関数を条件に使わない、関数の先頭の宣言、段落の comment、forward 宣言、Success の return）。試験の .c は C89・host の試験で、`CHECK` の中の式に一部の条件演算子を使っている。
