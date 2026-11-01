<!-- awesome-plan project=zedbsd record=ws324-p006f -->

# ws324-p006f H.265: h265-std・h265-stream の実装（StdVideoH265 と、Annex B から picture へ。標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006d と p006e の cleared の後に起動）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§2.2・§4 D2・D3・D7〜D12・D14・D15・§5.3・§6・§7・§8）。
- 基点 BASE: `<p006a 段 2 の SHA>`。前提: p006b〜e が main にある。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§2.2（Vulkan の H.265 の std の意味）・§4・§5.3・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265-std.h`、`h265-stream.h`。使う物: `h265sw-nal.h`・`h265sw-ps.h`・`h265sw-slice.h`・`h265sw-poc.h`・`h265-dpb.h`（実装済み）、`vcodec-nal.h`（`vcodec_nal_find`）、host の `/usr/include/vk_video/vulkan_video_codec_h265std.h`・`_decode.h`（Khronos、1.4.309）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - h265-stream: 7.4.2.4.2〜7.4.2.4.5（AU と順、PDF 87〜90）、7.4.3.2.1 の SPS の活性（PDF 94〜95）、8.1.3（NoRaslOutputFlag・HandleCraAsBlaFlag・PicOutputFlag、PDF 135〜137）、B.2・B.3（PDF 291〜292）、C.5.2.2 の NoOutputOfPriorPicsFlag（PDF 306）。
  - h265-std: 7.4.4（profile の compatibility、PDF 107〜110）、Table A.8 の level（PDF 282〜283）、7.4.8（explicit の形の delta、7-67〜7-71）。Vulkan の意味は設計 §2.2 と `h265-std.h` の comment（driver との約束）。

## 2. 作る物

- `userland/desktop/libmedia/h265-std.c`、`h265-stream.c`。
- `plan/ws324/tests/unit/test-std.c`、`test-stream.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

- test-std（入力の `h265sw_*` の構造体は試験の中で手で埋める）:
  - VPS・SPS・PPS の全 field の写し（flag の各 bit、minus・diff の形への直し、ids、PCM の有無、conformance window、DecPicBufMgr の 7 entry と上の 0）。
  - pointer: 各 pointer が出力の構造体の中を指し、条件で NULL（VUI 無し、HRD 無し、NAL・VCL の present 0、short-term 0 個、long-term 無し、scaling の無効・PPS の list 無し）。
  - RPS の 2 つの形: explicit の set はそのまま、予測の set は syntax の値と、導いた set を explicit に直した値（delta_poc_s0_minus1[0] = −DeltaPocS0[0] − 1、[i] = DeltaPocS0[i−1] − DeltaPocS0[i] − 1、s1 も同様、used の mask）を手の計算と照合。差が 65 536 を越える set の EINVAL。
  - profile: 1・2・3・4・9 の写し、0 と 5 で compatibility の flag 1・2・3 の最小、どれも無い ENOTSUP。level: 30・60・63・…・186 の 13 値と、31・255 の ENOTSUP。
  - picture info: IRAP・IDR の flag、IsReference 1、sps の flag と NumBitsForSTRefPicSetInSlice・NumDeltaPocsOfRefRpsIdx の 3 通り（SPS の set、header の explicit、header の予測）、ids、POC、Curr の list の slot と 0xFF（欠けと残り）、9 個以上の ENOTSUP。references と setup の flag と POC。
- test-stream（`h265write.h` で組んだ Annex B の stream）:
  - IDR から始まる IPPP、CRA から始まり RASL を捨て RADL を返す、途中から（IRAP の前の絵を before_irap に数える）、EOS の後の CRA（no_rasl_output 1、after_eos 1）、reset の後の CRA（BLA として、RASL を捨てる、first 1）、IRAP の no_output_of_prior_pics の導出（CRA は 1、IDR は flag の値、first の絵では 0）。
  - parameter set: AU の途中の再送で generation が変わらない、違う中身で変わる、cursor が次の AU の parameter set の前で止まる（次の picture の前に store が変わらない）、`h265_stream_parameters`。
  - SPS の活性: CVS の途中で別の SPS id を名指す絵は壊れた絵、同じ id で中身が変わった SPS（IRAP 以外で）も壊れた絵、次の IRAP で立ち直る。
  - slice segment: 600 個で返り 601 個で壊れた絵、VPS の欠けで壊れない、dependent、PPS の不一致・nal_unit_type の不一致の壊れた絵、first_slice_segment_in_pic_flag 0 から始まる絵の飛ばし、offset と size が NAL の bytes（start code の後）を指す、access_unit の範囲。
  - AU の境: AUD・prefix SEI・suffix SEI・EOS・EOB・reserved の 41〜44 と 48〜55、他の層の NAL（other_layer）、reserved の VCL の型（reserved_type）。
  - 長い slice header（entry point を多くして 64 KiB を越える）を全体の写しで読む、1 MiB を越える物で壊れた絵。
  - `h265_stream_input` を AU ごとに分けて与えても、全体を 1 回で与えた時と同じ picture の列。
  - 全ての切り口で切った stream と乱数の byte 列で落ちない（ASan・UBSan）。
- わざと壊す確かめ（例: RASL の捨ての条件、EOS の後の NoRaslOutputFlag、explicit の直しの符号、level の写しの 1 値、cursor の止め位置）。

## 4. 注意

- `h265-std.c` だけが `<vulkan/vulkan.h>` を include する。host の試験は host の header で build される（run-unit.sh）。
- 出力の std の構造体は自分の中を指すので、試験で写した後に pointer を読まない。
- `h265_stream_next` は次の AU を始める NAL の前で止まる（header の約束）。parameter set を先に読んで store を変えない。
- 捨てる絵は POC の状態を変えない。返す絵だけ `h265sw_poc_finish`。
- `slice_rbsp` の malloc の失敗の道（ENOMEM）は code の読みで確かめ、結果に書く。
- NumDeltaPocsOfRefRpsIdx の約束は Vulkan の本文の字句と違う（design D8、header の comment）。header に従う。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh std stream
sh plan/ws324/tests/run-unit.sh all
BASE=<p006a 段 2 の SHA> sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

（実装の担当が書く）
