<!-- awesome-plan project=zedbsd record=ws324-p006f -->

# ws324-p006f H.265: h265-std の実装（StdVideoH265 の構造体へ。標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006a 段 2 の基点の後に起動。p006b・p006e と並べてよい）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§2.2・§4 D2・D7〜D9・D15・§5.3・§6・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。この module は他の module の実装を呼ばない（入力の構造体の型だけを使う）。試験の入力は手で埋める。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§2.2（Vulkan の H.265 の std の意味）・§4（D7〜D9・D15）・§5.3・§6・§8。
- 実装する header: `userland/desktop/libmedia/h265-std.h`（driver との約束を冒頭の comment に書いた）。使う型: `h265sw-ps.h`・`h265sw-vui.h`・`h265sw-rps.h`・`h265sw-scaling.h`・`h265-dpb.h`・`h265-stream.h`。host の `/usr/include/vk_video/vulkan_video_codec_h265std.h`・`_decode.h`（Khronos、1.4.309）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - 7.4.4（profile の compatibility、PDF 107〜110）、Table A.8 の level（PDF 282〜283）、7.4.8（explicit の形の delta、7-63〜7-71、PDF 121〜122）、7.3.2.2.2（range extension の 9 つの flag の順、PDF 55）。

## 2. 作る物

- `userland/desktop/libmedia/h265-std.c`。
- `plan/ws324/tests/unit/test-std.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

入力の `h265sw_*`・`h265_picture`・`h265_dpb_plan` の構造体は試験の中で手で埋める。

- VPS・SPS・PPS の全 field の写し（flag の各 bit、minus・diff の形への直し、ids、PCM の有無、conformance window、DecPicBufMgr の 7 entry と上の 0、range extension の 9 つの flag の bit n → 名前の flag の全 9 通りと sps・pps の range_extension_flag、SCC の field が 0）。
- VUI: sar_width・sar_height が signalled の値（EXTENDED_SAR の時だけ非 0）、HRD の flag の 8 bit の mask（fixed_pic_rate_general など、sub-layer i が bit i）、sub-layer の配列。
- pointer: 各 pointer が出力の構造体の中を指し、条件で NULL（VUI 無し、HRD 無し、NAL・VCL の present 0、short-term 0 個、long-term 無し、scaling の無効・PPS の list 無し）。scaling が有効で data 無しの SPS は既定の list を指す。
- RPS の 2 つの形: explicit の set はそのまま、予測の set は syntax の値と、導いた set を explicit に直した値（delta_poc_s0_minus1[0] = −DeltaPocS0[0] − 1、[i] = DeltaPocS0[i−1] − DeltaPocS0[i] − 1、s1 も同様、used の mask）を手の計算と照合。差が 65 536 を越える set の EINVAL。
- profile: 1・2・3・4・9 の写し、0 と 5 で compatibility の flag 1・2・3 の最小、どれも無い ENOTSUP。level: 30・60・63・90・93・120・123・150・153・156・180・183・186 の 13 値、間の値（31・100・185）の次の上の level、0 の 1.0、255 の 6.2。
- picture info: IRAP・IDR の flag、IsReference 1、sps の flag と NumBitsForSTRefPicSetInSlice・NumDeltaPocsOfRefRpsIdx の 3 通り（SPS の set、header の explicit、header の予測）、ids、POC、Curr の list の slot と 0xFF（欠けと残り）、9 個以上の ENOTSUP。references と setup の flag と POC。
- わざと壊す確かめ（例: explicit の直しの符号、level の写しの 1 値、range extension の bit の位置、NumDeltaPocsOfRefRpsIdx の条件）。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- この file だけが `<vulkan/vulkan.h>` を include する。host の試験は host の header で build される（run-unit.sh）。
- 出力の std の構造体は自分の中を指すので、試験で写した後に pointer を読まない。
- NumDeltaPocsOfRefRpsIdx の約束は Vulkan の本文の字句と違う（design D8、header の comment）。header に従う。
- Vulkan の bit field（`uint32_t x : 1`）への代入は 0 か 1 にしてから（幅 8 の mask の field も同様に範囲の中で）。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh std
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

（実装の担当が書く）
