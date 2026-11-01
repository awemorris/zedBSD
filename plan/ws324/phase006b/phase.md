<!-- awesome-plan project=zedbsd record=ws324-p006b -->

# ws324-p006b H.265: h265sw-nal・h265sw-scaling・h265sw-vui の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006a 段 2 の基点の後に起動）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D4・D12・§5・§7・§8）。
- 基点 BASE: `<p006a 段 2 の SHA>`（段 2 が書き込む）。

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
BASE=<p006a 段 2 の SHA> sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

（この module 群は libmedia の `sources.mk` に入らない。target の build の代わりに check-skeleton が cc と clang の両方で警告 0 を確かめる。）

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。
- 規約の checklist（coding-style §14）で自分の diff を見直した。表の値の block ごとに「Values referenced: ITU-T H.265 (01/2026) Table …」の comment。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <path>...`。push しない。rm しない。
- 結果の節: 日付（host の `date`）、command と最後の行、check の件数、自分で決めた点、未実施、表の値の照合の方法。ws.md の行を「実装済み・試験 PASS」。

## 結果

（実装の担当が書く）
