<!-- awesome-plan project=zedbsd record=ws324-p006d -->

# ws324-p006d H.265: h265sw-slice・h265sw-poc の実装（標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006c の cleared の後に起動）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D1・D4・D12・§5・§7・§8）。
- 基点 BASE: `<p006a 段 2 の SHA>`。前提: p006b・p006c が main にある。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§4（D1・D4・D12）・§5・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265sw-slice.h`、`h265sw-poc.h`。使う物: `h265sw-nal.h`・`h265sw-ps.h`・`h265sw-rps.h`（実装済み）、`vcodec-bitreader.h`。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - h265sw-slice: 7.3.2.12 byte_alignment（PDF 59）、7.3.6.1〜7.3.6.3（PDF 64〜67）、7.4.7.1〜7.4.7.3（7-52〜7-58、PDF 113〜120）、Table 7-7（slice_type）。
  - h265sw-poc: 8.3.1（8-1〜8-4、PDF 137〜138）、8.3.2 の 8-5（PDF 138〜139）、prevTid0Pic の定義（8.3.1）、Table 7-1 の SLNR（PDF 84〜85）。

## 2. 作る物

- `userland/desktop/libmedia/h265sw-slice.c`、`h265sw-poc.c`。
- `plan/ws324/tests/unit/test-slice.c`、`test-poc.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

期待の値は仕様と手の計算から。入力は `h265write.h`（slice segment header の書き手）と、試験の中で組んだ SPS・PPS（`h265sw_ps_read_*` で store に入れる）。

- test-slice:
  - 全分岐の往復: IDR・CRA・BLA・TRAIL・RASL・RADL の各型、first と first でない（address の Ceil(Log2(PicSizeInCtbsY)) bit を PicSizeInCtbsY 1・2・3・2^k・2^k + 1 で）、dependent（PPS の dependent_slices 0 では flag を読まない、1 で写し）、reserved の flag 0〜7 本、output_flag の有無、colour_plane_id（separate_colour_plane 1 の SPS）、short-term の SPS の set（idx の bit 長を num_st_rps 1・2・3・64 で）と header の set（予測あり・無し、st_syntax の bits と ref_num_delta_pocs）、long-term（num_lt_sps 0・1・2・32 で lt_idx_sps の bit 長、poc_lsb_lt、msb の cycle の累積）、temporal MVP、SAO の luma と chroma（ChromaArrayType 0 で chroma を読まない）、P と B の num_ref_idx の override、list の modification（NumPicTotalCurr 1 では読まない、2・3・5 の bit 長）、mvd_l1_zero、cabac_init、collocated（from_l0 の推定、ref_idx の読む条件）、重み（P・B、luma と chroma の flag、負の重みの 7-58 の floor）、max_merge_cand、qp と chroma の offset、deblocking の override、loop filter across slices の読む条件、entry point（WPP だけ・tile だけ・両方の上限の境、offset_len 1・32）、extension の長さ 0・256・257（EINVAL）、byte_alignment（1 の bit が 0 の誤り、0 でない詰め物の誤り）、data_offset。
  - 導いた値: slice_addr_rs、num_pic_total_curr、qp（26 + init_qp_minus26 + slice_qp_delta の境）。
  - 範囲の端の EINVAL: address の PicSizeInCtbsY、slice_type 3、IRAP の P・B、idx、lt_idx_sps、num_long_term の和、DeltaPocMsbCycleLt の上限（各値と和）、collocated_ref_idx、five_minus_max_num_merge_cand 5、SliceQpY 52 と −QpBdOffsetY − 1、cb・cr の offset ±13、beta・tc ±7、offset_len_minus1 32、pps の id 64。
  - dependent の segment で independent NULL の EINVAL。
  - 途中で切れた RBSP の全ての切り口で EINVAL（短い写しの場合を含む）、乱数の入力で落ちない。
- test-poc:
  - 8-1 の 3 つの枝を、MaxPicOrderCntLsb 16 と 65536 で境（差が Max/2 ちょうど・± 1）を含む手の例。IRAP の NoRaslOutputFlag 1 で Msb 0。IDR の POC 0。
  - finish: TemporalId 0 の TRAIL_R は記録、RASL・RADL・SLNR（TRAIL_N など）・TemporalId 1 は記録しない。
  - rps: IDR で全部空。short-term の used で Before・After・Foll への分け（8-5 の順）。long-term の msb の有無、負の PicOrderCntVal での `&` の floor の算術（例: POC −1、Max 16 で 15）、DeltaPocMsbCycleLt の大きな値。int32 を越える和で EINVAL。
- わざと壊す確かめ（例: 8-1 の >= と > の取り違え、dependent の写しの 1 field、list_entry の bit 長、7-58 の shift）。

## 4. 注意

- byte_alignment は `vcodec-bitreader.h` に無い。この module の中で読む（`vcodec*.h` を変えない、design D4）。
- 負の値の右 shift（7-58 の `( 128 * ChromaWeight ) >> denom`）は floor の除算で。負の値の `&`（8-5）は `x − Floor( x / m ) * m` を int64_t で（`h265sw.h` の約束）。
- slice header の short-term の set は `h265sw_rps_read( bits, num_st_rps, num_st_rps, sps->st_rps, max, &st_rps, &st_syntax )`。SPS の set を使う時は `st_syntax` を 0 のままに。
- 7.4.7.1 の entry point の上限は tile と WPP の組で 3 通り（header の comment）。
- 重みの presence の条件の PicOrderCnt の比較は常に真として読む（header の約束）。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh slice poc
sh plan/ws324/tests/run-unit.sh all
BASE=<p006a 段 2 の SHA> sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

（実装の担当が書く）
