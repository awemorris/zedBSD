<!-- awesome-plan project=zedbsd record=ws324-p006f -->

# ws324-p006f H.265: h265-std の実装（StdVideoH265 の構造体へ。標準廉価実装手順、Sonnet 5.5 high）

Status: in-progress（2026-11-02 Sonnet q1901-i01 が実装済み・試験 PASS。cleared は Q1 の受け入れ後）。前: planned（2026-11-01 P1 が p006a で作成。p006a 段 2 の基点の後に起動。p006b・p006e と並べてよい）

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

実装の担当（Sonnet 5.5、q1901-i01、worktree `s-ws324-p006f`）、2026-11-02（host の `date` で 03:14 JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265-std.c`（骨格を実装で置き換え。外から見える名前は `h265_std_*` の 7 つだけ）。
- `plan/ws324/tests/unit/test-std.c`。header・道具・他の module は変えていない（check-skeleton の BASE 照合 PASS）。

### 確かめた command と最後の行

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh std` | `test-std: PASS (1680 checks)`、`run-unit: PASS` |
| `sh plan/ws324/tests/run-unit.sh all` | h265write 51・nal 816099・ps 1447981・rps 1891521・scaling 5499・std 1680（最後の足しの前は 1632）・vui 2432070 checks、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（host の cc・clang。zedBSD の clang は h265-std を飛ばした: tree に Vulkan の H.265 の header が無い） |
| zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header）で h265-std.c を `-std=c89 -Wall -Wextra -Werror -Wpedantic -fsyntax-only` | 3 target とも出力なし。ただし tree の `include/libc/vulkan/vk_video/` には H.264 の header しか無いので、host の `/usr/include/vulkan`・`vk_video` を scratchpad に複写して `-isystem` で先に置いた（tree は変えていない）。tree に H.265 の header が入った時の確かめは未実施 |
| `git diff --check` | 出力なし（終了 0） |

試験は ASan・UBSan 付き（`-std=c89 -Wall -Wextra -Werror`）。

### 試験の中身（要点）

- 入力の構造体（`h265sw_vps`・`h265sw_sps`・`h265sw_pps`・`h265_picture`・`h265_dpb_plan`）は全部この file の中で手で埋め、他の module は使わない。期待の値は syntax の minus・diff の形、7.4.8（7-63〜7-71）を手で解いた値、level_idc = 30 × level（7.4.4、Table A.8）から書いた。
- level 44 値（13 値・各値の前後・0・186 超・2^32 - 1）、profile（1・2・3・4・9 の写しと、0・5・10・255 の compatibility の全 bit の組、ENOTSUP で out が変わらない）、PTL の flag 5 つを 1 つずつと全体の memcmp。
- VPS（全 field、7 sub-layer、HRD の有無と 4 通りの NAL・VCL、各 flag の単独）、HRD（flag の mask の 3 本を sub-layer ごとの手の値、7×32 の CPB 値、pointer）。
- SPS（全 field の minus・diff の形、PCM の有無、conformance の 4 辺、DecPicBufMgr の 7 entry、long-term の mask、別 heap での pointer が自分を指す確かめ）、flag 30 個（base 16 と range extension 9 と SCC 5）を 1 つずつ、range_extension の 10 bit 目以上が flag にならない、9 bit 全部。
- VUI（signalled の SAR と導いた SAR の区別、flag 18 個を 1 つずつ、全 field）、scaling（4 つの size と 6 つの matrix、32x32 は matrixId 0 と 3、DC、有効で data 無しの SPS は parse の入れた list、無効は NULL、PPS の有無）。
- 短期 RPS: explicit の set、予測の set（7-61 を手で解いた 3 つの例: 負だけ・正だけ・混合と used の mask）、explicit の直しの境（差 65536 まで、65537・非単調・0 の EINVAL）、15・16 entry、17 の EINVAL、64 set の順と 65 の EINVAL。
- PPS（key の 3 id、minus の形、init_qp の全範囲 -48..51、tile の個数と非一様の sizes と一様・無しの 0、20 列・22 行の境と超過の EINVAL、flag 24 個を 1 つずつ、SPS の NULL・id 違いの EINVAL）。
- picture info（IDR・CRA・非 IRAP、D8 の 3 通り: SPS の set は 0・0、header の explicit は bits と 0、header の予測は bits と NumDeltaPocs、Curr の list の slot と 0xFF、8 個ちょうどと 9 個・16 個の ENOTSUP で出力が変わらない、slot 254 と負と 255・256 の EINVAL）、references（POC・long の flag・count が配列を越える時の切り詰め）、setup。
- 乱数: 3000 通りの乱数の構造体（SPS の個数・RPS の個数・tile の個数・list の個数・slot を境の近くに寄せる）で、返り値が文書の物だけで落ちない（ASan）。

### 表の値の照合の方法

- level の表（30・60・63・90・93・120・123・150・153・156・180・183・186）: 方法 1 は 7.4.4 の「general_level_idc は level 番号の 30 倍」（PDF 頁 280 の本文）と level の一覧（Table A.8 に当たる表、この版では Table A.9、PDF 頁 282〜283 の text: 1・2・2.1・3・3.1・4・4.1・5・5.1・5.2・6・6.1・6.2）から手で 30 倍にした値、方法 2 は Vulkan の enum（`vulkan_video_codec_h265std.h` の `StdVideoH265LevelIdc`、1.0 が 0 から 6.2 が 12 の並び）との対応を header の text で確かめた。頁の画での照合は未実施（text だけ。表に 6.3（189）も載っているが header の約束どおり 186 を越える値は 6.2 に写す）。
- range extension の 9 flag の並び: 7.3.2.2.2（PDF 頁 55）の text の順と `StdVideoH265SpsFlags` の順が同じことを text で確かめた（画は未実施）。

### わざと壊す確かめ（git の外で、script は scratchpad の `mutate.py`）

- 220 通り（level の表の値と enum の並び・比較、profile の case と compatibility の走査（範囲・向き・index・shift）、PTL の flag、VPS・SPS・PPS・VUI・HRD の flag と field の取り違え、DecPicBufMgr の loop・index、mask の shift と数、sub-layer の配列の loop・pointer・NAL と VCL の取り違え、range extension の bit の位置 4 か所、PCM の minus・diff と有無、conformance の辺、long-term の clamp・mask・条件、scaling の size・matrix 添字と 32x32 の ×3、RPS の符号・off-by-one・上限の `>=`・check 抜き・used の添字と shift・同じ index 抜け、PPS の minus・tile の loop・上限・一様、picture の IsReference・flag・D8 の条件と値・Curr の上限と slot の境・EINVAL の抜き、references の切り詰め、setup、`std_bit`・memset 抜け・error の握りつぶし）を、各 mutant を別 directory に置いて build し、ASan・UBSan 付きで流した。各 build・実行に RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s・壁時計 600 s・RSS 6 GB の上限、同時 4 本。
- 最初の 219 通りで 16 が生き残った。等価と判断した 2 つ（`std_level` の最後の表の値 186→185 は「表の上は最後の level」で結果が同じ、init_qp の `(uint8_t)` 経由は同じ値）は mutant から外し、1 つ（slot の `< -1`）は等価のため `< -10` と check 抜きに置き換え、残りは試験を足して塞いだ: HRD の low_delay の bit 6、sub_pic の 2 flag の独立、VPS の temporal_id_nesting の単独、RPS の 16 entry の境（derived 16・explicit 16・8+8・9+8）、予測の set の delta_rps_sign・abs_delta_rps_minus1・delta_idx_minus1・used・use_delta、long-term の 33 個以上の clamp、CRA（IRAP で IDR でない）、after・lt の list の slot の EINVAL、PTL の出力全体の memcmp（memset 抜け）。
- 足した後の再実施: 220 通り全部が試験の失敗（`FAIL` で `FAIL (build)` でない）で落ちた（結果は下の最終行を参照）。

### 自分で決めた点（header が黙っている所）

- `h265_std_ptl`・`h265_std_vps`・`h265_std_sps`: ENOTSUP は out を書く前に返す（out は変わらない。試験で確認）。`h265_std_sps` が EINVAL（短期 set）を返す時は out が途中まで書かれていて使えない。`num_short_term_ref_pic_sets` が 64 を超える時も EINVAL（配列の大きさ）。
- `pLongTermRefPicsSps` は `long_term_present`（long_term_ref_pics_present_flag）が 1 なら NULL でない（`num_lt_sps` が 0 でも。その時 mask と lsb は 0）。header の「long-term 無し」を flag の 0 と読んだ。`num_lt_sps` が 32 を超える時は 32 に切って写す（`num_long_term_ref_pics_sps` はそのまま uint8 に写す）。
- 短期 set: `inter_rps_pred` が 0 の set は syntax の explicit の field をそのまま写す（導いた set は使わない）。1 の set は予測の syntax と、導いた set から作った explicit の形の両方。explicit の形の個数が 16 を超える・derived の合計が 16 を超える set は EINVAL。
- `h265_std_pps`: `tile_columns` が 20 を、`tile_rows` が 22 を超えると EINVAL（Vulkan の 19・21 の配列に signalled の entry が入らない）。tiles が 0 なら tile の個数は 0（`tile_columns` の値は見ない）。column_width・row_height は uniform_spacing 0 の時だけ最初の `tile_columns - 1`・`tile_rows - 1` 個を写す（最後の列・行は残りなので写さない）。
- `h265_std_picture`: Curr の slot が 255 以上は EINVAL（`STD_VIDEO_H265_NO_REFERENCE_PICTURE` と区別できない）、負の slot は `H265_DPB_NO_SLOT` と同じ「参照なし」。ENOTSUP・EINVAL の時は info を書かない（一時の構造体で作ってから最後に写す）。`NumBitsForSTRefPicSetInSlice`・`NumDeltaPocsOfRefRpsIdx` は header の comment（D8）どおり、`short_term_ref_pic_set_sps_flag` が 1 の picture では syntax の中身が残っていても 0。
- `h265_std_references`: `reference_count` が配列（16）を超える時は 16 に切る（info は count 個ぶんの領域という約束なので、これを越えて読まないため）。
- chroma_format_idc は 0〜3 以外でも検べずに enum へ cast する（parse が 0〜3 を保証。header は検べを約束していない）。`aspect_ratio_idc` も cast。
- HRD の flag の mask は sub-layer 0〜6 の 7 つ（`H265SW_MAX_SUB_LAYERS`）。`fixed_pic_rate_*` が 2 以上でも 1 として数える。
- SPS の range extension の 9 flag は `range_extension_present` に関わらず `range_extension` の bit 0〜8 から写す（parse は absent で 0 にする）。

### 未実施・持ち越し

- zedBSD の clang の 3 target での h265-std の確かめは、tree に `include/libc/vulkan/vk_video/vulkan_video_codec_h265std.h`・`vulkan_video_codec_h265std_decode.h` が入るまで check-skeleton では飛ばされる（上の scratchpad 複写での確かめは済み）。
- level・range extension の表の頁の画での照合。
- header の comment の持ち越しは無し（header の不備は見つからなかった）。

### 消す物（Q1 へ）

- 試験の出力 `build/tmp/h265-unit.run.*`（worktree の中）。mutant の build は scratchpad の `mut/`（`/tmp/claude-1000/-home-awe-zedBSD-claude1/b57eaf0e-d1bd-5084-9fc6-6409b527934c/scratchpad/mut/`）。
