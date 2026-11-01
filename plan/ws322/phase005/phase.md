<!-- awesome-plan project=zedbsd record=ws322-p005 -->

# ws322-p005 libvulkan の H.265 decode の転送

- 親: [WS322](../ws.md)。設計: [design.md](../design.md) §8.2・§12・§15 の p005、WS324 の [design.md](../../ws324/design.md) §10・§11 と [proposed/gpu-op-video-encode.md](../../ws324/proposed/gpu-op-video-encode.md) §2（capset の bit）。
- Status: in-progress（2026-11-02 06:14 P2、q1911-i01、worktree p2-ws322、branch agent/p2-ws322、基点 main 4eae22020）
- 依存: Vulkan の H.265 decode の header（下の「header」、この Phase の最初の commit）。WS318 p011a・WS324 p002 の段 1 と同じ物。

## 目的

libvulkan（`userland/desktop/libvulkan/`）が `VK_KHR_video_decode_h265` を、native の renderer が capset で H.265 decode を約束した時だけ名乗り、H.265 の profile・capabilities・session parameters（VPS・SPS・PPS）・decode の記録を、H.264 と同じ video の wire（`video.c` の自己記述の形）で renderer に送る。executor の側の reader（PowerVR は p006、nvrtx は WS324 p005）はこの wire を読む。

## 担当の割り振りの記録

- ws.md の「WS324 との揃え」（Q324-3）は libvulkan の H.265 の転送を WS324 p004 に置いたが、2026-11-02 Q1 の依頼（q1911-i01）は Q322-4（先に着手する方が持つ）により WS322 p005 が持つとした。WS324 p004 はまだ始まっていない。WS324 が読めるよう、wire は WS324 design §10 の形（VUI・HRD・sub-layer を含む全部の field、幅のある flag は 1 語ずつ）、capset の bit は WS324 の案（bit 2 = H.265 decode）に揃える。ws.md・WS324 の記録の直しは Q1。

## H.264（p004 までの道）から H.265 への拡げ方

H.264 の転送は WS083・WS243 で作られ、p004 はそれを PowerVR の executor の reader で読んだ。libvulkan の側は codec ごとに次の所だけが H.264 に固定されている。H.265 はそれぞれに codec の分岐を足す（新しい opcode・protocol の版は要らない。構造は sType で自己記述なので、同じ 14 の opcode の中で sType が増えるだけ）。

| 所 | 今（H.264） | H.265 で足す物 |
| --- | --- | --- |
| `context.c` capset | byte 172 の bit 0 → `context->video_h264` | bit 2 → `context->video_h265`。video の門は「どちらかの decode の bit」 |
| `instance.c` format・queue の門 | `video_h264` が無ければ video の format feature・queue flag を隠す | decode の bit のどれも無ければ隠す |
| `instance.c` `physical_load_video` | family の codec の操作を DECODE_H264 だけ残す | renderer の答え ∧（bit 0 なら DECODE_H264、bit 2 なら DECODE_H265）。H.265 の family があれば `VULKAN_DEVICE_VIDEO_DECODE_H265` |
| `instance.c` 拡張の表 | video_queue・decode_queue・decode_h264（`available[14]`） | decode_h265（表を 15 に） |
| `device.c` | 名前と依存（decode_h264 → decode_queue） | decode_h265 → decode_queue |
| `internal.h` | `VULKAN_DEVICE_VIDEO_DECODE_H264 = 2048` | `VULKAN_DEVICE_VIDEO_DECODE_H265 = 16384` |
| `video.c` profile の検べ | H.264 decode の profile だけ | H.265: 拡張が有効な device の時、chain に `VkVideoDecodeH265ProfileInfoKHR`、`stdProfileIdc` は MAIN（D9、Main だけ）、8 bit 4:2:0 |
| `video.c` 知っている入力 | H.264 の 4 つの sType | H.265 の profile・session parameters create・add・picture・DPB slot の 5 つ |
| `video.c` 出力 | `VkVideoDecodeH264CapabilitiesKHR` | `VkVideoDecodeH265CapabilitiesKHR`（maxLevelIdc） |
| `video.c` parameters の鍵 | SPS 32・(SPS, PPS) 32×256 | VPS 16、(VPS, SPS) 16×16、(VPS, SPS, PPS) 16×16×64 |
| `external-properties.c`・`vkGetPhysicalDeviceVideoFormatPropertiesKHR` | `vulkan_video_profile_list_check` | 同じ関数が H.265 を通す（変えない） |

## wire（H.265 の構造、H.264 と同じ規則）

規則は `video.c` の先頭の comment（構造は sType・chain・field の宣言の順、chain の link は 64 bit の有無の語、pointer は有無の語と中身、配列は 64 bit の数と要素、1 bit の flag は名前の順に bit 0 から 1 語へ）。H.265 で決める事:

- 1 bit の flag の struct（`StdVideoH265SpsFlags`・`PpsFlags`・`VpsFlags`・`ProfileTierLevelFlags`・`SpsVuiFlags`・`ShortTermRefPicSetFlags`・decode の `PictureInfoFlags`・`ReferenceInfoFlags`）は宣言の順に bit 0 から 1 語。
- 幅のある flag（`StdVideoH265HrdFlags` の 1 bit 4 つと 8 bit 3 つ）: 1 bit の 4 つを 1 語、8 bit の `fixed_pic_rate_general_flag`・`fixed_pic_rate_within_cvs_flag`・`low_delay_hrd_flag` をそれぞれ 1 語（sub-layer ごとの bit mask）。
- 数の field は u32 に広げる（符号付きは符号を保って広げる）。固定の大きさの配列は 64 bit の要素数と要素（u8 の配列は bytes、u16・u32 は要素ごとに u32）。
- pointer は全部送る: SPS の PTL・DecPicBufMgr・ScalingLists・ShortTermRefPicSet（`num_short_term_ref_pic_sets` 個の配列）・LongTermRefPicsSps・VUI（と HRD と sub-layer の NAL・VCL の配列、`cpb_cnt_minus1` は送るが配列は sub-layer の数 `sps_max_sub_layers_minus1 + 1` 個）・PredictorPaletteEntries、VPS の DecPicBufMgr・HRD・PTL（sub-layer の数は `vps_max_sub_layers_minus1 + 1`）、PPS の ScalingLists・PredictorPaletteEntries。H.264 は VUI を送らない（decode に要らない）が、H.265 は WS324 design §10 に揃えて送る（NVDEC・Wave5 の reader が読み捨ててよい。encode の取り出しでも同じ構造を使う）。
  - sub-layer の HRD の配列の数: Vulkan の規定は「pointer は sub-layer ごとの要素の配列」（`vps_max_sub_layers_minus1 + 1`・`sps_max_sub_layers_minus1 + 1` 個）。HRD の flag の present が 0 なら pointer は NULL でよい（libmedia の h265-std はそうする）。
- decode の `VkVideoDecodeH265PictureInfoKHR`: `pStdPictureInfo`（`RefPicSetStCurrBefore`・`StCurrAfter`・`LtCurr` は 8 byte ずつの bytes）、`sliceSegmentCount` と offset の配列。
- `VkVideoDecodeH265DpbSlotInfoKHR`: `pStdReferenceInfo`（flag の語と `PicOrderCntVal`）。

## header（2026-11-02 Q1 が branch に入れることを許した。別の最初の commit）

- Q1 の判断（2026-11-02、q1911-i01 の返事）: 担当の branch に、別の最初の commit として header を入れてよい。toolchain・sysroot の build はしない（shared の sysroot の写しの作り直しが要るなら Q1 に知らせる）。
- 入れた物（commit bafa50879）は下の案の通り。

当時の tree の `include/libc/vulkan/` には H.265 の std と `VK_KHR_video_decode_h265` の宣言が無い（`vulkan_core.h` 1.3.269 は H.265 decode の sType 1000187000〜5 と `VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR` だけを持つ）。WS324 design §11 の段 1（p002）と同じ物を案にする:

1. `include/libc/vulkan/vk_video/` に Debian `libvulkan-dev 1.4.309.0-1`（Khronos Vulkan-Headers 1.4.309、Apache-2.0、Copyright 2015-2025 The Khronos Group Inc.、tree の `LICENSE-API` と同じ）から変えずに写す:
   - `/usr/include/vk_video/vulkan_video_codec_h265std.h` SHA-256 `a5d0b2e7b47843db610d2769c7b0202c90cd7db82108b60fb2631d1c7976a395`
   - `/usr/include/vk_video/vulkan_video_codec_h265std_decode.h` SHA-256 `944646ced5bf9f49b79144c18369f0b932900c375fd81c262a3c30e9c60de8c6`
   - （`vulkan_video_codecs_common.h` は既にある、同じ SHA-256 `7afc98e8…`）
2. `userland/desktop/libvulkan/tools/maintain-video.noct`: 選ぶ拡張に `VK_KHR_video_decode_h265` を足し、照合の `blocks != 4` を 5 に（command の数 19 は変わらない、H.265 decode に command は無い）、preamble の題を H.264・H.265 decode に。block の中に `#include "vk_video/vulkan_video_codec_h265std_decode.h"` があるので `vulkan_video.h` の先頭の include は変えなくてよい。そのうえで `vulkan_video.h` を作り直す（入力は今と同じ `/usr/include/vulkan/vulkan_core.h` 1.4.309、SHA-256 `39f358bb…`）。
3. `API-PROVENANCE.md` の「Vulkan Video and synchronization2 declarations」に `VK_KHR_video_decode_h265`（revision 8）と 2 つの file の SHA-256 を足す。dispatch の表（`maintain-dispatch.noct`）は command が増えないので変わらない見込み（作り直して確かめる）。
- `maintain-video.noct` の preamble は、tree の `vulkan_video.h` の今の先頭の comment（生成の後に手で直されていた）をそのまま書く形に直した（作り直しの差が H.265 の block と題の 1 行だけになる）。
- `maintain-dispatch.noct` を `build/tmp` に流した: 記録の数は今の表と同じ（H.265 decode に command は無い）。今の表との差は前からある 2 行（video の 2 つの physical の問いが生成では ENTRY_INSTANCE、表では手で直した ENTRY_PHYSICAL）だけで、この変更と関係が無いので触っていない。
- shared の sysroot（`toolchain/llvm/sysroot.mk` が `include/libc` を全部 `usr/include` に写す）は作り直していない（Q1）。libvulkan・libmedia は `-isystem include/libc`・`-Iinclude/libc` で tree の header を読むので要らない。

## 範囲

1. 上の表の libvulkan の変更（`video.c`・`context.c`・`instance.c`・`device.c`・`internal.h`、README の Vulkan Video の節）。
2. host 試験 `plan/ws322/tests/p005/`: libvulkan の encoder（`video.c` を include）で H.265 の parameters create（VPS・SPS・PPS、全部の pointer を埋めた物と NULL の物）・update・decode（picture info と DPB slot）・begin・profile を書き、試験の中の reader で全部の field と byte の数を読み直す。鍵の検べ（範囲の外の id、重なり、template の上書き）。capset の bit と family の codec の操作の門（H.264 だけ・H.265 だけ・両方・無し）。H.264 の wire が変わらないこと（p004 の `wire-host-test` をそのまま流す）。
3. build: amd64・riscv64 の libvulkan。
- 範囲の外: PowerVR の reader と Wave5 の HEVC（p006）、nvrtx の reader（WS324 p005）、encode（p010）、header を tree に入れる事（Q1）、libmedia の H.265 の Vulkan の道（WS318）。

## UAPI・HAL

- HAL は変えない。`include/uapi/gpu-op.h` は変えない（opcode は H.264 と同じ、capset の bit は libvulkan の `context.c` の約束で、renderer の側は p006 で立てる）。toolchain は触らない。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host の wire | `plan/ws322/tests/p005/run.sh` が 0 failed（H.265 の往復、鍵、門） |
| H.264 の非退行 | `plan/ws322/tests/p004/run.sh` の wire（33）と `plan/tools/rtx-video/wire-host-test.sh` が前と同じ |
| build | amd64・riscv64 の libvulkan が warning 0 |

## 結果（2026-11-02 P2、q1911-i01）

### commit（基点 main 4eae22020）

- bafa50879: header だけ（`vk_video/vulkan_video_codec_h265std.h`・`_h265std_decode.h` を変えずに写す、`maintain-video.noct`、作り直した `vulkan_video.h`、`API-PROVENANCE.md`）。
- 次の commit: libvulkan の H.265 decode の転送（`video.c`・`context.c`・`instance.c`・`device.c`・`internal.h`・`external-properties.c` の comment・README）と host 試験 `plan/ws322/tests/p005/`（`run.sh`・`h265-wire-host-test.c`・`video-gate-host-test.c`）、この phase.md・ws.md。SHA は Q1 への報告に。

### 確かめた事（host）

| 項目 | 結果 |
| --- | --- |
| H.265 の wire | `sh plan/ws322/tests/p005/run.sh build/tmp/p005-run2`: h265-wire 65375 passed・0 failed（parameters create 20 回と update、decode 50 回、profile、capabilities の要求と返事、鍵、profile の検べ。構造は擬似乱数で埋め、flag の語は bit field の生の bit と比べる。全部の byte を読み切る）、video-gate 26 passed・0 failed（bit の組 6 通り、family の flag、拡張の表 15 個、ASan・UBSan） |
| 試験の効き | video.c を 1 か所ずつ壊す 10 通りのうち、元の文が無かった 2 通りを除く 8 通りが全部試験の失敗で落ちた（flag の bit の位置、HRD の flag、PPS の id の範囲、num_positive_pics、reserved3、cr の offset、VPS の id の境、sub-layer の数） |
| H.264 の非退行 | `plan/tools/rtx-video/wire-encode.c` の 3 つ（decode・begin・parameters create）の byte が基点 4eae22020 の video.c と同じ（2931 文字の hex が一致）。`sh plan/ws322/tests/p004/run.sh build/tmp/p005-p004`: ps-slots 4094・engine 56・wire 33・nv12 state 69・executor 505・nv12 752、全部 0 failed。`sh plan/tools/rtx-video/wire-host-test.sh`: 32 checks、0 failed |
| compile | host の gcc（`-Wall -Wextra -Werror`）で video.c・instance.c・context.c・device.c。zedBSD の clang（`build/llvm/bin/clang`、x86_64・aarch64・riscv64、`-std=gnu99 -Wall -Wextra -Werror -fsyntax-only`、`-isystem include/libc`）で同じ 4 つが warning 0 |
| WS324 の骨格 | `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` → `check-skeleton: PASS`、h265-std の「skipped」の行が消えた（zedBSD の clang で h265-std.c も syntax を通る） |
| whitespace | `git diff --check` 0 |

### 判断（範囲の中の技術）

- H.265 の wire は WS324 design §10 の形（VUI・HRD・sub-layer を含む全部の field、HRD の 8 bit の flag 3 つは 1 語ずつ）。H.264 の SPS が VUI を落とすのは変えない（byte が今と同じであることを確かめた）。
- 配列の数は app の field の通りに送る（sub-layer の HRD は `max_sub_layers_minus1 + 1`、short-term の set は `num_short_term_ref_pic_sets`）。範囲の外の値（app の誤り）の検べは renderer に任せる（H.264 の `num_ref_frames_in_pic_order_cnt_cycle` と同じ扱い）。
- 鍵は codec ごとの別の型を作らず、両方の大きさを持つ 1 つの `struct vulkan_video_keys`（2084 byte）。parameters の object は 1 つの session ＝ 1 つの codec の物なので混ざらない。
- profile の検べは H.265 Main だけ（D9）。Main 10・Main Still Picture は断る。
- 拡張の表の配列を 14 から 15 にした（足さないと全部の拡張を持つ device で溢れる、gate の試験の最後の項目が ASan で見る）。

## 未実施

- `make` での libvulkan（amd64・riscv64）の build: 自分の worktree の `build/riscv64/sysroot` の作り直しを伴うので、Q1 の返事を待っている（上の zedBSD の clang の syntax の確かめで代えている）。
- PowerVR の reader（p006）・nvrtx の reader（WS324 p005）との往復。capset の bit 2 を立てる renderer はまだ無いので、実機・QEMU での確かめは無い（QEMU の Venus では bit が無く今と同じ道）。
