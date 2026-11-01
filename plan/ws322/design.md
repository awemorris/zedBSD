<!-- awesome-plan project=zedbsd record=ws322-design -->

# WS322 設計: VisionFive 2（JH7110）の Wave511（H.264・H.265 decode）と Wave420L（H.265 encode）を PowerVR の Vulkan の executor の Vulkan Video として

Parent: [WS322](ws.md)。Phase: [ws322-p001](phase001/phase.md)（この文書を書いた Phase）。
版: 第 2 版（2026-11-01、P2、q1845）。第 1 版に design-reviewer の review 1（[design-review.md](design-review.md)）の指摘を反映した。実機・QEMU の観測は無い。値と手順は全て source と公開の文書を読んだだけの物（§5 の出典）。

## 0. 要約

- 見せ方（ユーザーの決定）: VPU は GPU と別の hardware だが、PowerVR の render node の Vulkan の executor（`src/drivers/gpu/powervr/vk/`）の queue family 0 に `VK_QUEUE_VIDEO_DECODE_BIT_KHR`（後に `_ENCODE_`）を足し、Vulkan Video として出す。新しい cdev・UAPI は作らない。
- Wave511 は Chips&Media の Wave5 系の decoder で、仕様は Linux の wave5 driver（StarFive の 6.12 の tree、SPDX `GPL-2.0 OR BSD-3-Clause`）から読める。firmware が bitstream を解析し参照を自分で持つ stateful の decoder なので、WS236（Pi 4 の MMAL）と同じく **SPS・PPS（H.265 は VPS も）を StdVideo から作り直して slice の前に置く**。
- Wave5 の参照の絵は圧縮（FBC）の形で、表示の絵は別の linear の buffer に同時に書かれる。よって Vulkan には **`DPB_AND_OUTPUT_DISTINCT`** で見せる: 出力（DST）は app の NV12 の linear の image に VPU が直に書き（zero copy）、DPB は driver の中の FBC の pool（DPB の image は形だけ）。今の libmedia は DISTINCT を既に扱える（`vkvideo-runtime.c:1668-1688`）。
- 出す順: `W5_CMD_DEC_FORCE_FB_LATENCY_PLUS1`（並べ替えを止める register）で decode の順に出す。WS236 の VUI の書き換えの hack は要らない見込み（p002 の probe で確かめる）。
- 出力の image の指定: Wave5 の frame buffer の index は FBC（参照）と linear（表示）の組。driver が Vulkan の active な DPB の slot に結ばれていない index を選び、その index の linear だけを「1 枚の frame buffer の更新」（SET_FB の option 1）で宛先の image に差し替え、表示の flag で他の index を使わせない。更新が使えない時は driver の linear の pool に受けて GPU の transfer で写す（§7.5、review 1 の 1 番で直した）。
- Wave420L は Wave4 系の H.265 の encoder（HEVC Main、StarFive の公表は 1080p30）で、**mainline に driver が無い**。参考になるのは StarFive の `soft_3rdpart` の vpuapi だけで、file ごとの SPDX（`LGPL-2.1 OR BSD-3-Clause`）と directory の `LICENSE.txt`（binary の再配布だけ）・header の proprietary の文が食い違う。encode の実装はユーザーの判断（Q322-2）の後。
- H.265 と encode は Vulkan の header（WS318 p011a・p011b）と libvulkan の転送が前提。H.264 decode は今の libvulkan と libmedia の DISTINCT の道で進められるが、その道はどの実の driver でもまだ走っていない（§12、Q322-7）。実装の Phase を WS318 の completed の前に始めるかは Q322-6。
- HAL の API は変えない（§19）。

## 1. 目的・範囲・範囲の外

- 目的: JH7110 の VPU で H.264・H.265 の decode と H.265 の encode を Vulkan Video として出し、libmedia（WS318 の後は共有の device）から使えるようにする。
- 範囲:
  - Vulkan Video decode: `VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264`・`VK_KHR_video_decode_h265`。8 bit 4:2:0、progressive。H.264 は Baseline（constrained を含む）・Main・High、H.265 は Main だけ（WS318 Q318-8 の Q1 の判断 A に従う。Wave511 は Main10 も解ける見込みだが出さない）。
  - Vulkan Video encode（Q322-2 が A の時）: `VK_KHR_video_encode_queue`・`VK_KHR_video_encode_h265`。Main、8 bit 4:2:0、IDR と P（B-frame 無し、WS318 Q318-9 の A）、rate control は無し（CQP）と CBR。
  - kernel: VF2 の VPU の glue（power domain・clock・reset・場所・割り込み）、Wave5 の decoder の core、Wave4 の encoder の core、PowerVR の executor の video、firmware の package。
  - libvulkan: H.265 decode と encode の転送（他の driver の WS が先に足していなければ、§12）。
- 範囲の外: H.264 の encode（hardware に無い。fallback は WS329 の独自の software の encoder。ws.md の題の「libavcodec」は 2026-11-01 のユーザーの決定（WS318 Q318-2: libavcodec を使わない）で古い）、Main10・4:2:2、interlaced・MBAFF・field、JPEG の CODAJ12（MJPEG の camera は扱わない、Q318-4）、libmedia 自身の変更（WS318）、VPU の省電力の細かな制御（§10.4 の簡単な形だけ）。

## 2. 今の姿（main eccd50a29 で読んだ事実）

| 物 | 事実 | 場所 |
| --- | --- | --- |
| PowerVR の queue | family 1 つ（GRAPHICS・COMPUTE・TRANSFER）、queueCount 1 | `powervr/vk/vk-device.c:585` |
| PowerVR の capset | 168 byte（vendor の "SDBZ" と flags 7）。native の 176 byte の語（video の bit）は無い | `powervr/render-runtime.c:51-62,176-198` |
| native の capset | 176 byte、byte 168 に magic `0x5a4e4154`、172 に flags（bit 0 = H.264 decode、bit 1 = sync command）。H.265・encode の bit は無い | `libvulkan/context.c:38-41,221-235`、nvrtx の先例 `nvrtx/runtime.c:447-454` |
| libvulkan の video | H.264 decode だけ。family の codec の操作は DECODE_H264 だけを残す。SPS の VUI は送らない | `libvulkan/video.c:8-21`、`instance.c:1392-1402` |
| Vulkan の header | 1.3.269 から選んだ物。`vk_video/` は H.264 の std と decode だけ。encode は provisional の EXT の番号だけ。H.265 の std は無い | `include/libc/vulkan/vk_video/`、WS318 §2 |
| PowerVR の memory | 全ての allocation は物理で連続の 1 本、core の DMA の上限の下（VF2 は `0xffffffff`）。CPU は uncached の別名でだけ触る。type は 2 つ（device local・host visible）、どちらも uncached | `render.c:13-19,423-446`、`vf2-gpu.c` の `VF2_GPU_DMA_LIMIT`、`vk/vk.h` の memory type |
| PowerVR の SLC | host visible か共有の buffer の page は SLC を迂回、private の buffer は SLC を通す。SLC の書き戻しと破棄は `powervr_command_slc_flush()`（firmware の command、core の lock の下） | `gpu-mmu.c:527-537`、`commands.c:311-335` |
| PowerVR の image | 形式は R8・R8G8・RGBA8・BGRA8 ほか。**multi-planar（NV12）は無い**。linear の行は 32 byte に揃え、行の数は 32 に丸める | `image-state.c:110-151`、`image-state.h:42-45`、`image-state.c:270-276` |
| 実行の形 | vkQueueSubmit は command buffer の record を順に歩き、dispatch は 1 本の stream、render pass と transfer は firmware に出して走らせたまま。fence は最後の点（kind・value）。core の lock は kick の周りだけで、submit の間ずっとは持たない | `vk/vk-queue.c:340-460,795-910,320,720,1387` |
| ioctl の直列 | open ごとの busy（同じ fd の ioctl は順に）。別の fd（compositor）は待たない | `gpu.c:226-233` |
| query pool | PowerVR の executor に無い。libvulkan は decode の family があれば `queryResultStatusSupport = VK_TRUE` を返す | `libvulkan/external-properties.c:708-713`（WS243 F1 と同じ問題） |
| libmedia の DISTINCT | COINCIDE が無い時に DISTINCT を使い、出力の image は 1 枚（`slots + distinct`）。readback（TRANSFER_SRC）で CPU へ | `libmedia/vkvideo-runtime.c:1666-1688,1716` |
| VF2 の PMU | GPU（2）と VOUT（4）の domain だけを許す（他は EPERM） | `vf2-pmu.c:143`、`vf2-pmu.h` |
| VF2 の CRG | 表に Wave511・Wave420L・AXIMEM の clock・reset が無い | `vf2-crg.c`（grep で 0 件） |
| VF2 の device tree | 板の tree は GPU に node を持たないことが多く、board が自分で場所を書く（`vf2_gpu_builtin`）。VPU も同じ前提にする | `kern/platform/visionfive2.c:130-155` |
| VF2 の DMA engine | `drv_dw_axi_dmac_copy(channel, dst, src, size)`（物理 address の memcpy） | `include/drivers/dma/dw-axi-dmac.h:97`、`vf2-dmac.h` |
| H.264 の parameter set の書き出し | Pi 4 の driver に StdVideo から SPS・PPS を作る物がある（VUI の書き換えつき） | `platform/rpi4/rpi4-h264.[ch]` |
| 先例 | Pi 4（stateful の VPU、COINCIDE、settling list）、nvrtx（同期の decode、query pool、COINCIDE） | `bcm2711/vk/vulkan-video.c:1-50`、`nvrtx/vk/vk-video.c`、WS236・WS243 の design |

## 3. hardware

### 3.1 JH7110 の配線（vendor の device tree と dt-bindings の値）

| 項目 | Wave511（decode、"VPU Dec"） | Wave420L（encode、"VPU Enc"） |
| --- | --- | --- |
| register | `0x130a0000`、64 KiB | `0x130b0000`、64 KiB |
| 割り込み（PLIC の番号、tree の値） | 13 | 15 |
| power domain（PMU の bit） | `JH7110_PD_VDEC` 3 | `JH7110_PD_VENC` 6 |
| clock（SYS CRG の番号） | `WAVE511_AXI` 70、`_BPU` 71、`_VCE` 72、`_APB` 73、`NOC_BUS_VDEC_AXI` 76、`VDEC_MAIN` 75（親 `VDEC_AXI` 69） | `WAVE420L_AXI` 78、`_BPU` 79、`_VCE` 80、`_APB` 81、`NOC_BUS_VENC_AXI` 82（親 `VENC_AXI` 77） |
| reset（SYS CRG の番号） | `WAVE511_AXI` 47、`_BPU` 48、`_VCE` 49、`_APB` 50、`AXIMEM0_AXI` 53（on-chip の SRAM） | `WAVE420L_AXI` 54、`_BPU` 55、`_VCE` 56、`_APB` 57、`AXIMEM1_AXI` 58 |
| clock の親 | `VDEC_AXI` は BUS_ROOT の分周、`WAVE511_VCE` は PLL0 の分周、`_BPU` は BUS_ROOT の分周 | `VENC_AXI`・`WAVE420L_BPU`・`_VCE` は PLL2 の分周 |
| tree の印 | `dma-coherent`（それでも vendor の kernel は L2 を自分で書き戻す。§9.2）、`starfive,vdec_noc_ctrl` | `dma-coherent`、`starfive,venc_noc_ctrl` |
| tree に無い reset | `NOC_BUS_VDEC_AXI` 31、`VDEC_MAIN` 52 | `NOC_BUS_VENC_AXI` 32 |
| DMA の address | 32 bit（vendor の driver は DMA mask 32） | 32 bit と推定（未確認） |
| on-chip SRAM | VPU から見た address 0、`0x2D000` byte（secondary AXI、行の buffer） | AXIMEM1（大きさは未確認） |

- Values referenced（設計の中の値の出典、実装でも block ごとに書く）: StarFive Linux `JH7110_VisionFive2_6.12.y_devel` 4cecf169f38e `arch/riscv/boot/dts/starfive/jh7110.dtsi`（`vpu_dec@130a0000`・`vpu_enc@130b0000`）、`include/dt-bindings/clock/starfive,jh7110-crg.h`・`reset/starfive,jh7110-crg.h`・`power/starfive,jh7110-pmu.h`、`drivers/clk/starfive/clk-starfive-jh7110-sys.c`（親と分周の形）、`drivers/pmdomain/starfive/jh71xx-pmu.c`（domain の bit）、`drivers/media/platform/chips-media/wave5/wave5-vpu.h`（`VDI_SRAM_BASE_ADDR` 0、`VDI_WAVE511_SRAM_SIZE`）、`wave5-vpu.c`（DMA mask 32）。
- AXIMEM0・AXIMEM1（on-chip の SRAM）は reset だけで clock は無い（`clk-starfive-jh7110-sys.c` に clock があるのは AXIMEM2 だけ）。
- `starfive,vdec_noc_ctrl`・`venc_noc_ctrl` の意味は tree の外（vendor の out-of-tree の vdec.c）にあり未確認。tree に無い NOC の reset（上の表）と関係する見込み（推測）で、p002 で状態を読む。p002 で、無くても boot と decode が通るかを見る。通らなければ vendor の GPL の kernel module の値を guardrail の規則（値の引用は明示）で読む。
- clock の rate: vendor の tree の `vpu_dec`・`vpu_enc` の node は assigned-clocks を持たない（jh7110.dtsi の grep で該当 0 件）。firmware（U-Boot）が残した rate のまま使う形で始め、p002 で rate を log に出す（vf2-gpu の `log_clock_rates` と同じ）。

### 3.2 Wave511 の能力（公表と driver から）

- 公表: H.265・H.264 の decode、4K60（または 1080p の多 stream）。StarFive の README は HEVC と AVC。
- driver から読める事: product code `0x5110`（`WAVE511_CODE`）、HEVC 10 bit の能力の bit、命令の queue の深さ 2（Wave521 系）、work buffer `WAVE521DEC_WORKBUF_SIZE`（1784 KiB）、code の buffer 2 MiB と temp 1 MiB、登録できる frame buffer は FBC・linear とも 31 まで（`WAVE5_MAX_FBS` 32 未満、wave5-vpuapi.c:289）。
- 報告する上限（案、p002 で firmware の報告と測りで直す）: maxCodedExtent は H.264 4096×2304（level 5.1 の最大の絵、36864 MB）・H.265 4096×2176（level 5.1 の MaxLumaPs 8,912,896 の中）、minCodedExtent 64×64、H.264 maxLevelIdc 5.1、H.265 maxLevelIdc 5.1（main tier）、maxDpbSlots H.264 17・H.265 16、maxActiveReferencePictures H.264 16・H.265 15。

### 3.3 Wave420L の能力

- 公表: HEVC Main の encode、FHD・UHD（StarFive の README・Yocto の recipe の説明は「FHD/UHD HEVC main profile L4.1」）、VF2 の仕様は 1080p30。
- register の形は Wave4 系（soft_3rdpart の `vpuapi/wave/wave4/`、header は WAVE410 の register map と名乗る）。Wave5 の encoder（Linux の wave5-vpu-enc.c）とは別の世代で、そのままは使えないと見る（未確認、p009 で product code を読んで確かめる）。

## 4. firmware と license

| file | 入手元 | license | 扱い |
| --- | --- | --- | --- |
| `chagall.bin`（Wave511） | GitHub `starfive-tech/soft_3rdpart` branch `JH7110_VisionFive2_devel` の `wave511/firmware/`（git LFS） | 同じ dir の `LICENSE.txt`: Copyright (C) 2022 Chips&Media。binary の再配布と使用を許す、著作権表示・条件・免責を添える、逆解析・逆 compile・逆 assemble の禁止、無保証 | zedBSD-firmware に入れ、新しい package `wave511-firmware`（`userland/firmware/wave511/`）で `/lib/firmware/` に license・WHENCE・manifest と一緒に入れる。PowerVR の firmware（`LICENSE.powervr`、逆解析の禁止）と同じ形 |
| `monet.bin`（Wave420L） | 同じ repo の `wave420l/firmware/`（`monet.h` は同じ物の C の配列と見られる） | 同じ形の `LICENSE.txt`（Chips&Media 2022、binary だけ、逆解析の禁止） | 同じく `wave420l-firmware`。encode の Phase（Q322-2 の後）で入れる |

- vendor の Linux の wave5 は `wave511_dec_fw.bin` を読む（`wave5-vpu.c:345`）。StarFive の buildroot が配る同じ名前の file と `chagall.bin` が同じ byte かは未確認（p002 で SHA-256 を比べ、違えば新しい方を pin する）。
- git LFS: raw の URL は LFS の pointer を返す事がある。取得の script は LFS の media の URL か pin した commit の tarball から取り、SHA-256 で確かめる（p002）。
- 逆解析の禁止: driver は firmware の command の返事と register の値だけで作る。firmware を disassemble しない（PowerVR と同じ注意を README に書く）。
- 判断: Q322-1（配布）。PowerVR の先例と同じ条件なので Q1 が決められる見込み。

## 5. 出典と引用の扱い（guardrail「MIT・GPL の出典のデータの引用」）

| 出典 | license | 使い方 |
| --- | --- | --- |
| StarFive Linux `JH7110_VisionFive2_6.12.y_devel` 4cecf169f38e（`/home/awe/visionfive-linux`）`drivers/media/platform/chips-media/wave5/`（wave5-hw.c・wave5-regdefine.h・wave5-vpuapi.[ch]・wave5-vpu.c・wave5-vpu-dec.c・wave5-vpuconfig.h・wave5-vpu.h） | 全 file `(GPL-2.0 OR BSD-3-Clause)`、Chips&Media 2021-2023 | register の offset・bit・command の番号・buffer の大きさの式（Value(s) referenced）、命令の順（Procedure referenced）。BSD-3-Clause を選ぶ。code・comment・構造・symbol の名前は写さない（独自の名前） |
| 同じ tree の `arch/riscv/boot/dts/starfive/jh7110.dtsi`・dt-bindings・`clk-starfive-jh7110-sys.c`・`jh71xx-pmu.c` | jh7110.dtsi・clock と reset の binding は `GPL-2.0 OR MIT`、power の binding は `GPL-2.0-only OR BSD-2-Clause`、clk は `GPL-2.0`、pmu は `GPL-2.0-or-later` | 番号・address・親子（値、block ごとに引用元を書く） |
| StarFive `soft_3rdpart` `wave420l/code/vpuapi/`（wave4.c・wave4_regdefine.h ほか） | file の先頭: `SPDX: LGPL-2.1 OR BSD-3-Clause` と「confidential and proprietary、Chips&Media の許諾の下でだけ使える」の文が並ぶ。dir の `LICENSE.txt`（2022）は binary の再配布だけを許す | **Q322-2 の答えまで読まない**（この設計では file の一覧と header の license の行だけを見た） |
| StarFive JH7110 TRM（Preliminary V2） | 公開の文書 | memory map・割り込みの番号（GPU の glue と同じ使い方） |
| ITU-T H.264（7.3.2.1・7.3.2.2）、H.265（7.3.2.1〜7.3.2.3） | 規格 | parameter set の syntax（Procedure referenced） |
| zedBSD の `rpi4-h264.c`・`bcm2711/vk/vulkan-video.c`・`nvrtx/vk/vk-video.c`・`i915/vk/vk-video.c` | Zlib（自分の物） | wire の読み方・記録の規則の手順（Procedure referenced） |

- zedBSD の code は Zlib。block ごとに「Value(s) referenced:」「Procedure referenced:」を複数行の comment で書き、file の先頭には書かない。
- Linux の wave5 の symbol 名（`W5_*`・`wave5_*`）は使わない。register の名前は自分で付ける（例 `WAVE5_REG_BS_RD_PTR` のような接頭辞は使わず、`VPU_DEC_STREAM_READ` のように役割で）。

## 6. 構成（責務と境界）

```
 libmedia / app ── libvulkan（wire）── /dev/gpu の PowerVR の render node
                                         │
          src/drivers/gpu/powervr/vk/vk-video*.c   … Vulkan Video の object・記録・実行・query
                                         │ struct drv_powervr_video（powervr.h、engine の ops）
          src/drivers/vpu/wave5-*.c（decode）  src/drivers/vpu/wave4-*.c（encode）
          src/drivers/vpu/vpu-h264-ps.c・vpu-h265-ps.c（parameter set の書き出し）
                                         │ kern_device_map・kern_irq_register・kern_pmem_*・kernel の file の読み
          src/drivers/platform/visionfive2/vf2-vpu.c（power・clock・reset・場所・engine の登録）
```

| 層 | 責務 | 知らない事 |
| --- | --- | --- |
| PowerVR の executor の video（`vk-video.c`・`vk-video-h264.c`・`vk-video-h265.c`、encode は `vk-video-encode.c`） | wire の読み書き、capability・形式の報告、session・parameters の object、4 つの記録（encode は 2 つ増える）の検べ、submit の中での実行の順、SLC の扱い、query、NV12 の image | VPU の register・firmware |
| engine の口（`struct drv_powervr_video`、`src/drivers/gpu/powervr/powervr.h`） | PowerVR が呼ぶ codec の engine の ops（§8.1）。VF2 の glue が登録する | どの SoC か |
| Wave5 の decoder（`src/drivers/vpu/wave5-core.c`・`wave5-decode.c`） | firmware の boot、instance、sequence、frame buffer の登録、出力の宛先の指定、1 枚の decode、誤りと reset | Vulkan |
| Wave4 の encoder（`src/drivers/vpu/wave4-core.c`・`wave4-encode.c`） | encoder の firmware、sequence の設定、1 枚の encode、header の生成 | Vulkan |
| parameter set（`src/drivers/vpu/vpu-h264-ps.c`・`vpu-h265-ps.c`） | 自分の形の SPS・PPS・VPS の値から RBSP と NAL（emulation prevention、start code）を書く | hardware |
| VF2 の glue（`src/drivers/platform/visionfive2/vf2-vpu.c`） | PMU の VDEC・VENC、CRG の clock・reset、tree か board の場所、割り込みの番号、engine の作成と PowerVR への登録 | Vulkan・codec |

- 場所の理由: PowerVR は JH7110 の外の SoC にもある core なので、executor は VPU を直に知らず ops の口だけを見る（bcm2711 は `rpi4-mmal.h` を直に include しているが、Pi 4 の VPU は VideoCore そのもので分ける意味が薄かった）。共通の video engine の口（`include/drivers/` の汎用の header）は作らない（master-design-policy §2.6 の遅い抽象化。WS323 の Mali と Amlogic で同じ形が要ると分かってから考える）。
- Pi 4 の `rpi4-h264.c` は共有にしない（§17 D7）。
- `src/drivers/vpu/` は新しい directory（Chips&Media の core は他の SoC にもあるので platform の下に置かない）。

## 7. Wave5 の decoder の動かし方

### 7.1 core（`wave5-core.c`）と電源

- attach（VF2 の glue の kernel thread、boot を止めない）: domain を on・clock・reset の解除 → register の map → product code を読んで `0x5110` を確かめる（違えば off に戻して ENODEV、video 無し）→ firmware の file を読む（root の mount を待つ、PowerVR の `firmware-file.c` と同じ形で最大 60 s）→ boot → version と feature を log に 1 行 → engine を PowerVR に登録。どこかで失敗すれば off に戻し、登録しない。
- boot: code の領域（2 MiB）と temp（1 MiB）を 4 GiB の下に連続で取り（`kern_pmem_alloc_limited`）、uncached の view に firmware を写し、remap の 2 page・code の base と大きさ・temp の base と大きさ・AXI の ID を書き、初期化の命令を出して待つ（Procedure referenced: wave5-hw.c `wave5_vpu_init`、`remap_page`）。
- 命令: 1 つの命令の手順（command の register、instance の番号と codec、host の割り込みの印、busy の register）は 1 関数。命令は `vpu_lock`（sleep の mutex）の下で 1 つずつ。queueing の命令（INIT_SEQ・DEC_PIC）は完了の割り込みを待ってから QUERY の GET_RESULT（Procedure referenced: wave5-hw.c `send_firmware_command`・`wave5_send_query`・`wave5_vpu_dec_get_result`）。CREATE_INSTANCE・DESTROY_INSTANCE は Linux の driver が busy の register を待って終えている（wave5-hw.c:266-289,585）ので、割り込みか busy かは p002 で確かめて決める。命令の queue の深さは 2 だが v1 は 1 つずつ（非同期は p007）。
- 待ち: 各命令は上限つき（DEC_PIC 500 ms、他 100 ms）。超えたら core の reset（§11）。
- 状態は 2 つに分ける: (a) 確かめの状態 `ABSENT`・`READY`（attach が boot まで通った。engine の登録と `operations()` の値はこれだけで決まり、boot の後は変わらない）、(b) 電源の状態。v1 は attach の後ずっと on（VF2 は電池の板でない）。省電力（最後の session の後に firmware の `SLEEP_VPU`・`WAKEUP_VPU`（wave5-regdefine.h:14-15）、または off と再 boot）は p007 の後の任意の Phase にし、その時も `operations()` は変えない。

### 7.2 instance（Vulkan の session ごと）

- 作成（vkCreateVideoSessionKHR の時）: work buffer（1784 KiB、0 で埋める）を取り、bitstream の ring（§7.3）を作り、CREATE_INSTANCE（codec は H.264 か H.265、work buffer、secondary AXI の SRAM の address 0 と大きさ、ring の base と大きさ、bitstream の endianness、命令の queue の深さ、Procedure referenced: wave5-hw.c `wave5_vpu_build_up_dec_param`）。失敗は `VK_ERROR_INITIALIZATION_FAILED`、memory が取れなければ `VK_ERROR_OUT_OF_DEVICE_MEMORY`。
- instance の数の上限は 8（session 8 本）。超えたら `VK_ERROR_INITIALIZATION_FAILED`。
- 破棄: DESTROY_INSTANCE（Linux の finish_seq と同じ命令、wave5-hw.c:1476-1479）。firmware が「まだ動いている」と答えたら上限つきで繰り返す（Procedure referenced: wave5-vpuapi.c `wave5_vpu_dec_close` の再試行）。失敗したら core の reset（§11）の後に buffer を返す。

### 7.3 bitstream（stateful の decoder への 1 枚）

- ring: instance ごとに 4 MiB の連続・uncached（4 GiB の下）。decode のたびに先頭から書く: 必要なら parameter set（下）、続いて app の bitstream の `[srcBufferOffset, srcBufferOffset + srcBufferRange)` を CPU で写す（画素ではなく bitstream の byte で、1080p で 1 枚 数十 KB）。read pointer を ring の先頭に戻し、write pointer は書いた終わり、explicit end（1 枚の終わり）の option で DEC_PIC を出す（Procedure referenced: wave5-hw.c `get_bitstream_options`、`wave5_vpu_decode`）。
  - read pointer の戻し方: QUERY の SET_BS_RD_PTR（wave5-hw.c `wave5_dec_set_rd_ptr`）を正の道とし、DEC_PIC の前の `BS_RD_PTR` の書き込みだけで効くかも p002 で見る。どちらも効かなければ ring を続けて使い、wrap の手前で書き込みを先頭に戻す形にする（R5）。
  - 4 MiB を超える 1 枚は decode せず ERROR。
- parameter set を前に置く時: RESET の後の最初、IDR（H.265 は IRAP）の前、その絵の SPS・PPS（VPS）の中身が前に送った物と違う時。H.264 は SPS と PPS、H.265 は VPS・SPS・PPS。
- 書き出し（`vpu-h264-ps.c`・`vpu-h265-ps.c`）: 自分の形の値（executor が StdVideo の field から写す、§8.3）から RBSP を書き、emulation prevention（0x03）、start code `00 00 01`。VUI: H.264 は libvulkan が送らないので書かない（`vui_parameters_present_flag = 0`）。H.265 は StdVideo の VUI を libvulkan が送るか p005 で決める（送らなければ書かない）。VUI が無くても絵の値は変わらない（出す順と時刻の情報だけ）。出す順は §7.4 の register で決める。
  - scaling list（H.264・H.265）・H.265 の long term の参照の表・short term の RPS の表も書く（StdVideo にある物は全部）。

### 7.4 sequence と frame buffer

- INIT_SEQ: 最初の decode、RESET の後で SPS が変わった時、sequence の変化の印の後に、その 1 枚の bitstream で出す。I slice を持つ絵ならどれでもよい（IDR・IRAP に限らない。H.264 の recovery point の I から始まる stream、seek の後のため）。GET_RESULT で絵の大きさ・必要な frame buffer の数・crop・bit 深さ・profile・VLC と parameter の buffer の大きさを得る（Procedure referenced: wave5-hw.c `wave5_vpu_dec_init_seq`・`wave5_get_dec_seq_result`）。INIT_SEQ が済んだら、同じ ring の同じ 1 枚をそのまま DEC_PIC する（その絵を捨てない）。
  - 検べ: 8 bit でなければ、または session の maxCodedExtent を超えれば、その絵は ERROR（session は sequence 無しのまま）。
- frame buffer の index: Wave5 の decoded・display の index は 1 つの空間で、index i は FBC（圧縮の参照）の i 番と linear（表示）の i 番の組を指す（wave5-vpuapi.c:513 で両方の index を直に比べ、wave5-vpu-dec.c:430-442 は両方で capture の buffer を引く、wave5-vpuapi.h:390-418 の説明）。display の flag が立った index は firmware が decode に使わない（V4L2 の driver は app が持つ capture の buffer の flag を立てたままにする、wave5-vpu-dec.c:1190-1210,1357-1366）。
- 数: N = firmware が求める数 + 1（vendor と同じ、wave5-vpu-dec.c:365）。FBC と linear を同じ N で登録する。上限は 31（`num >= WAVE5_MAX_FBS` を断る、wave5-vpuapi.c:289）。超えれば ERROR。
- FBC の組: 1 枚は luma（幅 × 高さを 32 に丸め）＋ chroma（幅/2 を 16 に丸め × 高さ）、加えて FBC の luma・chroma の offset の表と co-located の MV の buffer（大きさの式は Values referenced: wave5-vpuapi.h `WAVE5_FBC_LUMA_TABLE_SIZE`・`WAVE5_FBC_CHROMA_TABLE_SIZE`・`WAVE5_DEC_AVC_BUF_SIZE`・`WAVE5_DEC_HEVC_BUF_SIZE`、wave5-vpu-dec.c `wave5_prepare_fb`）と task の buffer（VLC × 2 ＋ parameter × 命令の queue の深さ）。driver が `kern_pmem_alloc_limited`（4 GiB の下）で取る（§17 D4）。
- linear の組: 最初は driver の linear の pool（N 枚の NV12、render node の buffer として作り、session の GPU の address の空間に map しておく、§7.5 の 3.）を登録する。stride は登録全体で 1 つ（`W5_COMMON_PIC_INFO` は `fb_arr[0].stride` だけ、8 の倍数で pic_width 以上、wave5-vpuapi.c:299-301）なので、pool の pitch は session の DST の image の pitch（§8.4 の配置、幅で決まる）と同じにする。
- SET_FB: linear の組、続いて FBC の組を 8 枚ずつ登録（Procedure referenced: wave5-vpuapi.c `wave5_vpu_dec_register_frame_buffer_ex`、wave5-hw.c `wave5_vpu_dec_register_framebuffer`）。NV12 は CbCr の interleave。登録の後に全 index の display の flag を立て、使ってよい index だけを下ろす（§7.5）。
- 並べ替えを止める: 毎回の DEC_PIC で「frame buffer の遅れを強いる」register に 1（Values referenced: wave5-regdefine.h `W5_CMD_DEC_FORCE_FB_LATENCY_PLUS1`、wave5-hw.c:999 の reorder の扱い）。これで decoded と display の index は毎回同じになる見込み（p002 で確かめる、R1）。
- RESET（seek）: SPS が同じなら FLUSH_INSTANCE だけで、FBC と linear の組を保つ（大きな連続の memory を取り直さない、R8）。SPS が変われば次の decode で sequence を作り直す。

### 7.5 出力の宛先（zero copy と予備の pool）

- driver は index ごとに「その index を持つ Vulkan の DPB の slot」（decoded の index の結果から覚える）を持つ。
1. 使ってよい index を選ぶ: この decode の時点で active な DPB の slot（記録の参照の一覧と、それまでの setup の slot のうち active な物）に結び付いていない index。無ければ ERROR（app の DPB が firmware の求める数を超えた、R4）。
2. その index i の linear を宛先にする: 宛先の DST の image（plane 0・plane 1 の物理 address、pitch が登録の stride と同じ時）で、SET_FB の「1 枚の frame buffer の更新」（option 1、wave5-regdefine.h:238-240 と UPDATE_FB の register 群 :313-326）を出し、linear[i] だけを差し替える（FBC[i] と MV[i] は今の値のまま）。
3. 更新が使えない（p002 で R3 が否）、または pitch が違う時: linear[i] は driver の pool のままにし、decode の後に GPU の transfer（`vk-transfer.c` の道、pool の buffer は session の空間に map 済み）で pool の i 番から宛先へ 2 plane を写す。CPU は画素を触らない（guardrail「画像の処理は GPU で」）。
4. display の flag: i だけを下ろし、他の全 index を立てる（UPDATE_DISP_FLAG の query、Procedure referenced: wave5-hw.c `wave5_dec_clr_disp_flag`・`wave5_dec_set_disp_flag`）。参照の index も立てるが、flag は「decode の先に選ばない」の印で、参照として読むのは止めない（V4L2 の driver で app が持つ buffer が参照のままでいられるのと同じ見込み、p002 で確かめる、R2）。
5. 結果の decoded の index が i でなければ ERROR（firmware の選び方が見込みと違う、R2）。setup の slot があれば、その slot と i を結ぶ。
- 今の libmedia（DISTINCT、出力の image は 1 枚）でも、参照の絵は FBC の組に残り、linear[i] を毎回同じ image に差し替えるだけなので、参照を持つ絵も decode できる。
- 差し替えた linear[i] の古い address（前の宛先）は、次に i を使う時にまた差し替えるので、firmware が古い宛先に書く事は無い（flag が立っている間は使わない）。それでも、宛先の image の memory は「i に入っている間」executor が retain する（§9.1）。

### 7.6 1 枚の decode の手順（まとめ）

1. ring に parameter set（必要なら）と slice を書く。
2. sequence が無ければ §7.4（INIT_SEQ の後、同じ 1 枚を続けて decode）。
3. §7.5 で index と宛先を決める。
4. DEC_PIC（explicit end、遅れ 0、secondary AXI の使い方の bit、Procedure referenced: wave5-hw.c `wave5_vpu_decode`）、割り込みを待ち、GET_RESULT（成否、decoded・display の index、POC、絵の種類、sequence の変化の印、誤りの情報）。
5. 結果の読み: decoded の index が 0 以上なら §7.5 の 5.。`-2`（skip、RASL などの捨てる絵、wave5-vpuapi.h:195-198）は COMPLETE だが宛先は変えない（app は表示しない絵）。`-1`（空きの FB が無い）は ERROR（R4）。POC が Vulkan の picture info の PicOrderCnt（H.264）・PicOrderCntVal（H.265）と違えば log に 1 行（ERROR にはしない、firmware と app の DPB のずれの印、R4）。
6. driver の pool に受けた時は GPU の transfer で写す（§7.5 の 3.）。
7. 結果を query の status に（COMPLETE か ERROR）。
- sequence の変化の印（解像度・DPB の大きさの変化）が立った時: FLUSH → INIT_SEQ → SET_FB（新しい大きさ）の後、ring に残る同じ絵を DEC_PIC し直す（その絵を捨てない）。

### 7.7 DPB と Vulkan の slot

- firmware は bitstream（slice header の MMCO・RPS）から参照を自分で管理する。Vulkan の DPB の slot と参照の一覧は、記録の時に Vulkan の規則（i915 と同じ: setup の slot、active な参照、slot の index の範囲、同じ picture resource）で検べ、実行の時に §7.5 の index の選びに使う。firmware には渡さない。
- DPB の image（DPB だけの usage）の中身は誰も読まないので、memory の要求は小さく（4 KiB）答える（§8.4）。
- app が bitstream と違う参照を Vulkan に書けば、firmware の結果は bitstream に従い、§7.5 の index の選びが firmware の参照と重なり得る（その時は 5. の不一致で ERROR）。app が規則を守る限り結果は同じ。

## 8. PowerVR の executor の Vulkan Video

### 8.1 engine の口（`src/drivers/gpu/powervr/powervr.h` に足す、driver の中の header）

```c
/* The codec operations an engine performs (bits). */
#define DRV_POWERVR_VIDEO_DECODE_H264	0x1U
#define DRV_POWERVR_VIDEO_DECODE_H265	0x2U
#define DRV_POWERVR_VIDEO_ENCODE_H265	0x4U

struct drv_powervr_video_ops {
	uint32_t (*operations)(void *engine);
	int (*limits)(void *engine, uint32_t operation, struct drv_powervr_video_limits *limits);
	int (*open)(void *engine, uint32_t operation, const struct drv_powervr_video_open *open, void **stream);
	int (*decode)(void *stream, const struct drv_powervr_video_decode *job, struct drv_powervr_video_result *result);
	int (*encode)(void *stream, const struct drv_powervr_video_encode *job, struct drv_powervr_video_result *result);
	int (*reset)(void *stream);
	void (*close)(void *stream);
};

int drv_powervr_video_register(const struct drv_powervr_video_ops *ops, void *engine);
```

- 型の中身（案）: `limits` は最大・最小の大きさ・level・DPB の数。`open` は codec、maxCodedExtent、DPB の数、linear の pool を作る callback（executor が render node の buffer として作り session の空間に map した物の物理 address と GPU の address を返す）。`decode` の job は bitstream（kernel の pointer と長さ）、有効な parameter set（`struct vpu_h264_sps` などの自分の形、§8.3）、IDR・IRAP・I slice の印、active な DPB の slot の一覧と setup の slot、宛先（2 plane の物理 address・pitch・行の数と、executor が付けた不透明な鍵）、POC。`result` は status、使った index、pool に受けた時はその index（executor が transfer で写す）、linear[i] から外れた前の宛先の鍵（executor が retain を離す）、時間（tick）。
- retain の持ち主は executor（§9.1）。engine は鍵を覚えて返すだけで、memory の object を触らない。
- `decode`・`encode` は同期（上限つきで待つ）。p007 で非同期（submit と完了の callback）を足す。
- `operations()` は firmware の boot が済んだ後だけ 0 以外。render node の session の open の時に読み、その session の間は固定（latch、WS236 M2 と同じ）。
- 登録は 1 つだけ（VF2 は decode と encode を 1 つの engine にまとめた glue が登録）。登録の前に開いた session は video 無し。

### 8.2 報告

- capset: `operations()` が 0 でなく、`capacity >= 176` の時だけ 176 byte。byte 168 に `0x5a4e4154`、172 に flags: bit 0 H.264 decode（今の定義）、H.265 decode と H.265 encode の bit は libvulkan と同時に決める（§12、案 bit 2・bit 3）。
- queue family 0: GRAPHICS・COMPUTE・TRANSFER に `VIDEO_DECODE`（と encode があれば `VIDEO_ENCODE`）。queueCount 1 のまま。QUEUE_FAMILY_VIDEO_PROPERTIES は DECODE_H264 | DECODE_H265（| ENCODE_H265）。
- capabilities（decode）: flags `DPB_AND_OUTPUT_DISTINCT`、`SEPARATE_REFERENCE_IMAGES`、minBitstreamBufferOffsetAlignment 1、minBitstreamBufferSizeAlignment 16、pictureAccessGranularity 16×16（H.265 も 16×16 で始め、p002 で Wave5 が書く行の丸めを測って直す）、§3.2 の上限、stdHeaderVersion は各 codec の std の名前と版。
- format properties（decode）:
  - DST: `G8_B8R8_2PLANE_420_UNORM`、tiling OPTIMAL（中は linear、§8.4）、usage `VIDEO_DECODE_DST | SAMPLED | TRANSFER_SRC`、create flags `MUTABLE_FORMAT | EXTENDED_USAGE`（SAMPLED は plane の view の R8・R8G8 の形式で満たす。NV12 の形式そのものの features に SAMPLED_IMAGE は出さない）。
  - DPB: 同じ形式、OPTIMAL、usage `VIDEO_DECODE_DPB` だけ。
- `vkGetPhysicalDeviceFormatProperties(NV12)`: optimal と linear に `VIDEO_DECODE_OUTPUT | VIDEO_DECODE_DPB | TRANSFER_SRC`（encode があれば `VIDEO_ENCODE_INPUT`）。SAMPLED_IMAGE と YCbCr 変換の bit（COSITED・MIDPOINT）は出さない（libvulkan に sampler YCbCr conversion が無い、WS318 §2。YCbCr の変換の要る形式に SAMPLED_IMAGE を出すなら変換の bit が要る）。
- query: `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` の pool（作成・破棄・結果）と記録（reset・begin・end）。decode は実行の時に結果が決まるので、submit の後に結果が読める。

### 8.3 object と parameters

- session（`vk-video.c`）: profile・maxCodedExtent・DPB の数・形式を検べ、engine の `open`。memory の要求は binding 1 つ（4 KiB、type 0）。中身は使わない（memory は driver が持つ、§7.4・§17 D4）。
- parameters（`vk-video-h264.c`・`vk-video-h265.c`）: H.264 は SPS 32・PPS 256、H.265 は VPS 16・SPS 16・PPS 64 を StdVideo の field で保つ。update は鍵が重ならない物の追加だけ（libvulkan が先に断る）、`updateSequenceCount` は前の + 1。template から写す。
- decode の時に、その絵の id で SPS・PPS（VPS）を引き、自分の形（`struct vpu_h264_sps` ほか、`src/drivers/vpu/vpu-codec.h`、名前は自分で付ける）に写して engine に渡す。前に送った中身との比べ（§7.3）は engine の側で行う。

### 8.4 NV12 の image（`vk-image.c`・`image-state.c`）

- `G8_B8R8_2PLANE_420_UNORM` を video の usage の時だけ作れるようにする（他の usage の NV12 は断る）。tiling に関わらず linear の配置: plane 0 の pitch = 幅を 32 byte に揃え、行 = 高さを 32 に丸め（`POWERVR_IMAGE_LINEAR_ROWS`）、plane 1 は plane 0 の後（4 KiB 境界）、pitch は plane 0 と同じ、行 = 高さ / 2 を 32 に丸めた数（transfer は linear の image の 32 行の tile 全体を触る、`image-state.c:259-276`。plane 1 も丸めないと readback の PLANE_1 の copy が image の外を読む）。`layout.bytes` は両 plane の丸めた大きさの和。Wave5 の linear の出力は Y と CbCr の stride が共通なので、この配置にそのまま書ける。
- DPB だけの usage の image は memory の要求を 4 KiB にし、bind の検べ（`vk-image.c:355` が `layout.bytes` を見る）も同じ大きさで通す（中身を使わない、§7.7）。
- plane の view: aspect PLANE_0 を R8、PLANE_1 を R8G8 の view として作れる（MUTABLE_FORMAT の image）。texture の語は linear の R8・R8G8 の物（今の `powervr_view_words` が linear を扱う）を plane の address で作る。
- `vkGetImageSubresourceLayout`: PLANE_0・PLANE_1 の offset と pitch を答える（OPTIMAL でも。i915・bcm2711 と同じ私的な約束）。
- copy: `vkCmdCopyImageToBuffer` の PLANE_0・PLANE_1（libmedia の readback）を transfer の道（`vk-transfer.c`）で R8・R8G8 の linear の image として写す。NV12 の image への copy・blit・clear・render target は断る。

### 8.5 記録と実行（`vk-command.c`・`vk-queue.c`）

- 記録: `GPU_OP_CMD_BEGIN_VIDEO_CODING`・`_END_`・`_CONTROL_`・`_DECODE_VIDEO` を command buffer の record にする。記録の時に session・parameters・slot・resource を i915 と同じ規則で検べ、object を retain し、record に深い写しを置く（WS243 I6 と同じ）。API の規則違反は記録の失敗、bitstream の値の不正はその絵の ERROR。
- 実行（`build_command_buffer` の中、record の順）:
  1. CONTROL の RESET: engine の `reset`（FLUSH_INSTANCE と sequence の破棄、送った parameter set の記録を消す）。
  2. DECODE（`build.sent` に数える。後の失敗が refusal でなく lost になるように、`vk-queue.c:427-428`）: (a) それまでの dispatch の stream を流し（`run_flush`）、session の GPU の仕事（render・transfer の点）の終わりを待つ（VPU が書く宛先を GPU がまだ読んでいるかもしれない、と GPU が書いた物を VPU が読む、の両方のため）。(b) SLC の書き戻しと破棄（`powervr_command_slc_flush`、§9.2）。(c) engine の `decode`（同期）。(d) pool に受けた時は transfer で宛先へ写す。(e) 前の宛先の鍵の retain を離し、新しい宛先を retain する。(f) 結果を query に。
  3. 後の GPU の record（readback の copy、sample の pass）は decode の後で kick するので、decode の結果を読む。
- fence: 今のまま最後の GPU の点。decode は同期なので fence の時には終わっている。decode だけの submission の fence は「既に届いた点」（session の最後の点）で、待つ物が無い。
- 速さ: 1 枚ごとに CPU が VPU を待つ（1080p で数 ms の見込み）。同じ fd の他の ioctl はその間待つが、compositor（別の fd）は待たない（§2 の ioctl の直列）。

## 9. memory

### 9.1 所有と寿命

| 物 | 持ち主 | 寿命 |
| --- | --- | --- |
| firmware の code・temp | Wave5 の core | boot から shutdown まで（reset の時は同じ領域に写し直す） |
| work buffer・bitstream の ring | instance（session） | session の作成から破棄まで |
| FBC の組・MV・表・task | instance の sequence | INIT_SEQ から次の INIT_SEQ（大きさが変わった時）・session の破棄まで（SPS が同じ RESET では保つ） |
| linear の pool | executor（render node の buffer、session の空間に map） | session の video の object の破棄まで。engine は物理 address だけを持つ |
| linear[i] に入れた DST の image の memory | app（入っている間は executor が memory の object を retain） | result の「外した鍵」で離す。session の破棄で全部離す |
| app の bitstream の buffer | app（記録が retain） | 記録の破棄まで。VPU は ring の写しを読むので app の buffer を直に読まない |
| SRAM（AXIMEM0） | core | 常に（instance が共有。命令は 1 つずつなので重ならない） |

- 解放の順: VPU が書いているかもしれない memory は、VPU を止めてからだけ返す。DEC_PIC の timeout の道は、`decode` が返る前に core を reset に保持（reset の line を assert、§11）するので、返った後に executor が離す memory を VPU は書かない。reset の保持に失敗したら engine は domain を off にしてから返す。session の close（`powervr_vk_close`）は video の object の破棄で `close` を呼び、`close` は DESTROY_INSTANCE か reset の保持を済ませてから返る。

### 9.2 cache・SLC・address

- CPU の cache: 全ての buffer を uncached の view（`kern_pmem_map_uncached`、map の時に cached の行を書き戻して捨てる、`src/hal/riscv64/page.c:406-418` の説明）で触る。VPU は物理 address を使う。vendor の kernel は tree が `dma-coherent` なのに L2 を書き戻している（`wave5-vdi.c:17-23`、`wave5-vpu-dec.c:1180,1357`）。VPU の DMA が L2 を通らない（DRAM を直に読み書きする、GPU と同じ）なら、uncached の view だけを使う zedBSD では書き戻しは要らない。VPU が L2 を通る port なら、CPU の uncached の書き込み（ring）と L2 の古い行、VPU の書いた行が L2 に dirty で残り snoop しない GPU が DRAM の古い値を読む、の 2 つが起こり得る。**どちらかは未確認**なので p002 の probe で確かめる（§14.3: CPU が uncached で模様を書き VPU に読ませる、VPU が書いた物を GPU の transfer で読む、直後と、direct map で同じ page を読んで L2 を汚した後の両方）。L2 を通ると分かれば、kernel の data cache の口（`kern/dcache.h`）で該当の範囲を書き戻し・捨ててから VPU に渡す形に変える（HAL は変えない）。書いた後、命令の前に `kern_io_write_barrier`。
- PowerVR の SLC: VPU は SLC を通らない。app の private な device local の memory は SLC を通る（§2）ので、GPU が書いて VPU が読む（encode の入力）・VPU が書いて GPU が読む（decode の出力）の時に古い行が残り得る。v1 は decode・encode の前後に SLC の書き戻しと破棄を 1 回ずつ（§8.5 の (b)、後は VPU の終わりの後）。費用は p004 で測り、重ければ video の usage の image に bind された memory の page を SLC の迂回に張り替える案（§17 D6 の代わり）を p007 で。
- 32 bit: VPU の DMA は 32 bit。PowerVR の allocation は全部 `0xffffffff` の下（`VF2_GPU_DMA_LIMIT`）、driver の buffer も `kern_pmem_alloc_limited` の上限 `0xffffffff`。PowerVR の上限が将来広がったら、video の usage の image の memory だけは 4 GiB の下を保つ（`vk-memory.c` の allocate に条件を足す、その時の Phase で）。

### 9.3 zero copy の範囲

- decode の出力: VPU が app の DST の image の memory に直に書く（§7.5 の 2.）。1 枚の frame buffer の更新が使えない時・pitch が違う時だけ、pool から GPU の transfer で 1 回写す。
- 表示: app（WS318 の後は libmedia の共有の device）が DST の image の plane の view を sample する。今の libmedia（PRIVATE の device）は readback（TRANSFER_SRC の copy）して CPU の frame にする（今の i915・Pi と同じ道、WS318 の後に GPU の道）。
- encode の入力: app の ENCODE_SRC の image（NV12 linear）を VPU が直に読む（§13）。

## 10. 並行性・lock・割り込み

### 10.1 lock

| lock | 守る物 | 順 |
| --- | --- | --- |
| render node の open ごとの busy（既存） | executor の session | 1（外） |
| PowerVR の `core->lock`（既存） | firmware の command（SLC の flush） | decode の前後に短く取って離す。`vpu_lock` と入れ子にしない |
| `vpu_lock`（新、sleep の mutex、core ごと） | VPU の命令の register・instance の表・core の状態 | 2 |
| `vpu_irq_lock`（新、spin、割り込みを止めて） | 割り込みの理由の bit と待ち手の起こし | 3（内）。割り込みの handler はこれだけを取る |

- 待つ間は `vpu_lock` を持ったまま（命令は 1 つずつ）。別の session の decode はその間待つ（1080p で数 ms）。待つ間に PowerVR の `core->lock`・executor の他の lock を持たない（SLC の flush は `vpu_lock` を取る前と離した後）。

### 10.2 割り込み

- `kern_irq_register(13, …)`（encoder は 15）。handler の手順（Procedure referenced: wave5-vpu.c:49-92 `wave5_vpu_handle_irq`）: 割り込みの理由の register を読み、理由の clear と割り込みの clear の register を書き、instance ごとの完了の register（INIT_SEQ は sequence の完了、DEC_PIC は queue の命令の完了）の bit を clear し、理由を記録し、待ち手を起こす（`waitq_wake_all`）。level か edge かは p002 で決める。
- domain が off の間に register を読むと bus が止まる恐れがあるので、handler は core の電源の状態を見て off なら何も読まない。v1 は attach の後ずっと on（§7.1）なので、off にするのは attach の失敗と 3 回の reset の後だけで、その前に `kern_irq_unregister` する。
- 割り込みが来ない時（番号の誤り）: 待ちの上限で timeout。p002 で poll の形（1 ms）と比べる。

### 10.3 DMA

- VPU は自分の DMA で memory を読み書きする bus master（descriptor は無く、register に address を書く）。DMA engine（dw-axi-dmac）は使わない（review 1 の 21 番: 1 回ごとに poll で待ち、行ごとの copy は遅い。guardrail「画像の処理は GPU で」に合う GPU の transfer を使う）。

### 10.4 電源

- v1: attach で on にして boot し、以後 on のまま（§7.1）。省電力は後の任意の Phase。

## 11. 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| VPU の domain・clock・reset・product code・firmware の file が無い・boot の失敗 | engine を登録しない（video 無しの device、log 1 行）。GPU は今どおり |
| session の作成の失敗（work buffer・ring・CREATE_INSTANCE） | `VK_ERROR_INITIALIZATION_FAILED`、libmedia は software に落ちる（WS318 Q318-1） |
| INIT_SEQ の失敗・8 bit でない・大きすぎる | その絵は ERROR、sequence 無しのまま次の I slice の絵を待つ |
| DEC_PIC の誤り（成功の印 0・decoded の index の不一致・空きの FB 無し） | その絵は ERROR、宛先は変えない（pool に受けた時は写さない）。submission は失わない（i915・Pi と同じ） |
| DEC_PIC の timeout（500 ms） | `decode` が返る前に、vpu_lock の下で core の reset の line を assert して VPU を止め、全 instance を「壊れた」に、reset を解いて firmware を boot し直す（Procedure referenced: wave5-hw.c `wave5_vpu_reset`・`wave5_vpu_re_init`）。他の session の instance も firmware と一緒に消えるので、壊れた instance は次の decode の時に CREATE_INSTANCE から作り直し、次の I slice の絵まで ERROR。続けて 3 回 reset なら reset の line を assert したまま domain を off にし、engine を止める（以後の session の作成は失敗、boot まで video 無し。`operations()` はそのままなので capset は video を出したまま、session の作成で断る） |
| 4 MiB を超える絵・parameter set が無い | その絵は ERROR |
| pool からの transfer の失敗 | その絵は ERROR |
| SLC の flush の失敗 | submission は EIO（GPU の仕事が残っているかもしれないので、今の executor の規則どおり lost） |
| app が linear[i] に入れた image の memory を解いた | executor が retain しているので VPU の書く先は残る。i を次に使う時に差し替えて離す |

## 12. libvulkan・libmedia・他の WS との関係

| 物 | 持つ所 | 注意 |
| --- | --- | --- |
| H.265 の Vulkan の header（`VK_KHR_video_decode_h265`・std） | WS318 p011a（toolchain の扱い、Q1 の許可） | WS322 の H.265 の Phase の前提 |
| KHR の encode の header | WS318 p011b | encode の Phase の前提 |
| libvulkan の H.265 decode の転送（`video.c` の構造体の encode、`instance.c` の codec の操作の許可、`device.c` の拡張の表、capset の新しい bit） | 最初にそれを要る driver の WS（WS318 §10.3 は WS319 を想定）。WS322 が先なら WS322 p005 で足す（Q1 が順を決める） | i915（WS319）・Pi（WS320・WS321）と同じ wire にする |
| libvulkan の encode の転送（session・parameters の取り出し `vkGetEncodedVideoSessionParametersKHR`・`vkCmdEncodeVideoKHR`・encode の feedback の query・rate control） | 同上（WS322 p010 か WS319） | |
| libmedia の H.265 の Vulkan の解析（StdVideoH265 を作る） | WS318 p016〜p020 | WS322 の H.265 の Video Player の確かめの前提。driver の確かめは probe の道具（§14）で先にできる |
| libmedia の encoder の Vulkan Video（H.265） | WS318 p022 | |
| libmedia の DISTINCT の readback の道（今の code、`vkvideo-runtime.c:1666-1688`） | 持ち主は未定（Q322-7）。i915・nvrtx・bcm2711 は全部 COINCIDE だけを報告する（`i915/vk/vk-video.c:824`、`nvrtx/vk/vk-video.c:737`、`bcm2711/vk/vulkan-video.c:1275`）ので、この道はどの実の driver でもまだ走っていない | p004 で初めて走る。bug が出たら直す持ち主が要る |
| libmedia の DISTINCT の GPU の道（plane の view を sample） | WS318 p006b・p007a | 今は readback の道 |
| `vkvideo-probe`（WS083 の道具） | 今は H.264 の COINCIDE だけ（DST と DPB を同じ image で decode、`userland/tests/vkvideo-probe/main.c:578,695,1272`）。Intel でない時は linear で読む（WS236 J4） | **p004 で DISTINCT に対応させる**（DPB の image と出力の image を分け、出力を readback）。WS322 が持つ（+3 LW） |

- 2026-11-01 のユーザーの決定（WS318 Q318-1・Q318-2）: hardware が無い codec は独自の software へ落ちる。hardware の試験はその driver の WS で行う。→ WS322 は VF2 の実機の確かめを自分の Phase に持つ（§14）。

## 13. encode（Wave420L）の方針（Q322-2 が A の時。細部は p008 の設計）

- 形: Vulkan の encode の session ＝ Wave4 の encoder の instance。SET_PARAM 相当で GOP（IDR と P、参照 1 枚）、rate control（CQP か CBR）、大きさを設定し、ENC_PIC で 1 枚ずつ。
- 入力: app の ENCODE_SRC の NV12 linear の image（§8.4 と同じ配置）。Wave4 の encoder が source の address を絵ごとに受けるなら zero copy（p009 で確かめる）。
- 参照・再構成の絵: driver の中の pool（DPB の image は形だけ、decode と同じ）。
- 出力: Vulkan の `dstBuffer` の範囲に encoder が書く。firmware が絵ごとの出力の address を受けなければ、driver の ring に受けて CPU で `dstBuffer` に写す（bitstream の byte、画素ではない）。
- header: firmware が slice header を作る。VPS・SPS・PPS は `vkGetEncodedVideoSessionParametersKHR` が最初の encode の前にも呼ばれ得る（container の header のため）ので、firmware に頼らず `vpu-h265-ps.c` で session parameters の値（firmware の設定に合わせて直した値）から software で書く案を推す。firmware の設定に合わない所は feedback（`hasStdSPSOverrides` などを TRUE）で知らせる。firmware が bitstream に自分の header を入れるなら、それを止めて software の header を前に置く（p008 で決める）。
- feedback の query: `VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR`（bitstream の offset・書いた byte）。
- 能力（案）: maxCodedExtent 1920×1088（公表の 1080p30。4K は p009 で測ってから）、maxDpbSlots 2、maxActiveReferencePictures 1、rateControlModes DISABLED | CBR、maxQualityLevels 1、encodeInputPictureGranularity 8×8（p009 で直す）。

## 14. 試験の計画

### 14.1 host（各 Phase、gcc で driver の file をそのまま compile、`plan/ws322/tests/`）

- T1 parameter set: JVT の適合の stream（`plan/tools/h264sw/conformance/fetch.sh` の 101 本のうち progressive で、StdVideo で表せない FMO・ASO の Baseline の物を除く）と ws083 の stream の SPS・PPS を libmedia の parser で StdVideo の値にし、`vpu-h264-ps.c` で書き出し、それを同じ parser で読み直して値が全部一致（scaling list の差分の符号化などは一意でないので byte では比べない）。H.265 は HEVC の適合の stream（JCT-VC の物、p006 で取得の script）で VPS・SPS・PPS を同じ形で。emulation prevention の境界の例。
- T2 命令の組み立て: 各命令の register の値の並びを「（register、値）の表」を返す純な関数にして、固定の期待の表と比べる（CREATE_INSTANCE・INIT_SEQ・SET_FB の 8 枚ずつの分け・DEC_PIC・UPDATE_DISP_FLAG）。
- T3 index の選び: active な DPB の slot に結ばれていない index の選び、slot と index の結び（setup の slot、slot の解放）、display の flag の mask（N = 2〜31）、空きが無い時の ERROR、pitch が違う時の pool の道、外した鍵の返し。
- T7 失敗の道の単体: 命令の timeout の後に reset の line を assert してから返る、DESTROY の再試行、`-1`・`-2` の index の扱い（register の読みを偽の関数に替えて）。
- T4 大きさの式: FBC・表・MV・task・NV12 の配置（720p・1080p・4K）。
- T5 wire の往復: libvulkan の `video.c` の encoder と PowerVR の reader を 1 本で往復（WS243 T7 と同じ形）。
- T6 記録の検べ: slot・参照・範囲の規則違反が記録の失敗になり、正しい列が通る。
- build: riscv64（VF2 の config）の `vmunix`（warning 0）、config の off の build の object が変わらないこと、amd64 の vmunix と libvulkan が壊れていないこと。

### 14.2 QEMU

- QEMU に JH7110 の VPU は無い（riscv64 の virt と VF2 の image の boot は今の回帰のまま、boot-test.sh で login prompt）。video は VF2 の実機だけで確かめる。

### 14.3 VF2 の実機（Q1 が板の窓を割り当てる。1 本の script: 配置 → 起動 → 実行 → 回収 → 要約）

- p002 の probe（`CONFIG_DRIVER_VF2_VPU_PROBE`、既定 n、試験の config だけ）: 起動の後に kernel thread で、埋め込んだ小さな H.264 と H.265 の stream（ws083 の B の pyramid の物と、自分で x265 で作る 352×288 の B の物）を decode し、各 frame の NV12 の FNV-1a を host の ffmpeg（H.264 は WS326 の software の decoder でもよい）の参照の表（decode の順と表示の順の両方）と照らす。同じ起動で次を log に: product code・firmware の版・feature、clock の rate、NOC の reset の状態、(R1) 遅れ 0 で decoded = display、(R2) index の組の仮定（decoded の index が linear と FBC の同じ番号か）と display の flag で index が決まるか、参照の index に flag を立てても参照として読まれるか、(R3) 1 枚の frame buffer の更新（option 1）で linear[i] を差し替えて正しく書かれるか、(R5) read pointer の戻し（SET_BS_RD_PTR と register の書き込み）、(R14) 一貫性（CPU が uncached で書いた模様を VPU が読む・VPU が書いた物を GPU の transfer で読む、直後と direct map で L2 を汚した後）、CREATE_INSTANCE の完了が割り込みか busy か、1080p の 1 枚の時間。合否は「全 frame 一致」と各項目の観測の記録（項目は判定でなく事実を残す）。
- 失敗の道（p004 の実機、試験の config の fault の注入: DEC_PIC の後の割り込みを外す、壊した bitstream、IRQ の番号を外して poll）: timeout → core の reset → 同時に動く別の session の回復（次の I の絵から COMPLETE）、3 回の reset で止まる、decode の途中と timeout の後の fd の close で memory を返す前に VPU が止まっている（返した後の memory を probe の模様で見て書かれていない）、2 つの session の同時の decode（H.264 と H.265）、linear[i] に入れた image の memory の app の解放、DST を device local（SLC を通る）と host visible（SLC を迂回）の両方の memory の type で。
- p004（H.264 の Vulkan の道）: `vkvideo-probe --list`（VIDEO_DECODE の family と H.264 の拡張）、DISTINCT に対応させた `vkvideo-probe --expect=…`（ws083 の stream の hash 一致）、JVT の適合の progressive の物の一部（Baseline・Main・High の各数本）の hash を WS326 の software の decoder の出力と照らす、Video Player で 1080p30 の H.264 の mp4 を再生（画面の PNG、FRAMES の late と表示の数、`top` の CPU）。
- p006（H.265）: probe の道具（`plan/ws322/tests/` の小さな Vulkan の program、または WS318 の後の vkvideo-probe の H.265）で HEVC の適合の stream の Main の物の hash を host の ffmpeg の参照と照らす。Video Player は WS318 p020 の後。
- p007（速さ）: 1080p60・4K30 の H.264・H.265 の decode の時間（VPU の命令の tick）、SLC の flush の費用、Video Player の 1080p30 の再生。1080p60 の再生は WS318 の GPU の道（p009）の後の p012 で（今の readback は uncached の読みで 1080p60 に届かない見込み、R13）。
- encode（p011・p012）: 試験の絵（色の帯と動く物、NV12）を encode し、出た bitstream を host の ffmpeg で decode して PSNR（CQP で 35 dB 以上を目安、p008 で決める）、bitstream の適合（host の ffmpeg が誤り無く読む）。
- QEMU の証拠と実機の証拠は分けて書く。

## 15. Phase の分け方と受け入れ（案、LW は見積り）

| Phase | 目的 | 型 | 依存 | 受け入れ | LW |
| --- | --- | --- | --- | --- | --- |
| p001 | 調べと設計（この文書）、review | O | — | design-reviewer の反映、問いを Q1 へ | — |
| p002 | VF2 の VPU の glue と Wave5 の core の boot、firmware の package（wave511）、probe | O（high） | p001、Q322-1、Q322-6、VF2 の板の窓 | 実機で product code `0x5110` と firmware の版の log、probe の H.264・H.265 の全 frame が参照と一致、R1・R2・R3・R5・R14 の観測の記録。riscv64 の build の warning 0、off の build の object 不変 | 20 |
| p003 | PowerVR の executor: NV12 の image・plane の view・subresource layout・plane の copy、query pool（RESULT_STATUS_ONLY）、capset 176 と video の bit、queue family の flag | O | p001 | host の T4・T5（NV12 の部分）・T6 の一部、build。video の engine が無い時の報告は今と同じ（capset 168） | 10 |
| p004 | H.264 decode の道: engine の口、Wave5 の decoder（instance・sequence・FB・index と宛先・decode・誤り・reset）、`vpu-h264-ps.c`、executor の session・parameters・記録・実行、vkvideo-probe の DISTINCT 対応 | O（high） | p002、p003、Q322-7（libmedia の DISTINCT の道の直しの持ち主） | host の T1〜T3・T5〜T7。実機で vkvideo-probe の ws083 の hash が一致、JVT の progressive の一部が WS326 と一致、§14.3 の失敗の道の項目、Video Player で 1080p30 の H.264 を再生（PNG と FRAMES、readback の道の時間も記録） | 24 |
| p005 | libvulkan の H.265 decode の転送（構造体・codec の操作・拡張の表・capset の bit） | O | WS318 p011a。他の WS が先に足していれば取り下げ | host の wire の往復、amd64・riscv64 の libvulkan の build、i915・nvrtx・Pi の H.264 の今の道が壊れない（host の試験） | 8 |
| p006 | H.265 decode: `vpu-h265-ps.c`、Wave5 の HEVC、executor の H.265 の parameters | O | p004、p005 | host の T1（H.265）。実機で HEVC の適合の Main の一部の hash が host の ffmpeg と一致。Video Player の H.265 は WS318 p020 の後（この Phase の受け入れに入れない） | 16 |
| p007 | 速さ: 非同期の decode（video の点の kind、settle の列。`render-queue.c`・`render-private.h` を触る）、SLC の扱いの見直し、命令の queue 2 | O（high） | p004（H.265 は p006） | 実機で 1080p60・4K30 の decode の時間（VPU の tick）が 1 枚の時間の内（16.6 ms・33 ms）、Video Player の 1080p30 が落ちない | 12 |
| p008 | encode の設計の詳細（Wave4 の命令・GOP・rate control・header・feedback の写し） | O | Q322-2 が A、WS318 p011b | design-reviewer の反映 | 6 |
| p009 | Wave420L の bring-up（glue・firmware の package wave420l・boot・probe の encode） | O（high） | p008、VF2 の板 | 実機で product code と版の log、probe の encode の bitstream を host の ffmpeg が読み PSNR が目安以上 | 14 |
| p010 | libvulkan の encode の転送（H.265） | O | WS318 p011b。他の WS が先なら取り下げ | host の wire の往復、build | 10 |
| p011 | executor の H.265 encode（session・parameters・取り出し・記録・実行・feedback の query・CQP・CBR） | O（high） | p009、p010 | host の試験、実機で試験の program の encode を host で decode して PSNR、bitstream の適合 | 20 |
| p012 | 実機の受け入れ（Video Player の H.264・H.265 の 1080p60 の再生と 4K30 の decode、encode の往復、desktop と GPU の非退行） | O | p006・p007・p011、WS318 p009・p020・p022 | 一覧の項目の PASS（PNG、FRAMES、PSNR） | 8 |
| p013 | 規約の全文の見直し | conformance-reviewer | 全部 | coding-style の全文で見直し、動作は不変 | 4 |

- 合計 約 152 LW（8 LW ≈ 2〜3 h なので 約 38〜57 h）。encode（p008〜p011、50 LW）を外すと 約 102 LW。p002 は glue・firmware の package・boot・decoder の大半・probe を含むので、見積りの幅が最も大きい（実機の様子で ±50%）。
- 型: O = Opus、high = phase-runner-high（kernel・driver）。標準廉価実装手順（spec-module-delegation）に合うのは parameter set の書き出し（`vpu-h264-ps.c`・`vpu-h265-ps.c`、規格で振る舞いが決まる）だけで、他は hardware を探りながら作るので普通の Phase。p004・p006 の中の書き出しの部分だけを Sonnet に分けるかは Q1 が決める（推し: 分けない、量が小さい）。
- 先に進められる物（Q322-6 が A の時）: p002・p003・p004 は WS318 を待たない。p005・p006 は WS318 p011a、encode は Q322-2 と WS318 p011b。ユーザーの決定（WS318 の completed の後に個別の driver）に対し、2026-11-01 の前倒しが明示したのは p001 なので、実装の前倒しは Q322-6 で確かめる。

## 16. 危険

| ID | 危険 | 見込み・代わり |
| --- | --- | --- |
| R1 | 遅れ 0 の register でも firmware が表示の順で（遅れて）出す | p002 で確かめる。decoded の index の絵は decode の直後に linear[i] にある見込みなので、表示の順は使わず decoded の index だけを見る形で足りる。それも違えば WS236 と同じく SPS の VUI に `max_num_reorder_frames = 0` を書く（H.265 は `sps_max_num_reorder_pics = 0`、`sps_max_latency_increase_plus1 = 0`） |
| R2 | index の組の仮定が違う（linear と FBC の index が別）、display の flag で index が決まらない、参照の index に flag を立てると参照が読まれない | 宛先の指定をやめ、firmware が選んだ decoded の index の linear（driver の pool）から GPU の transfer で宛先へ写す（1080p で 1 枚 3 MB の読み書き） |
| R3 | 1 枚の frame buffer の更新（option 1）が効かない・参照を壊す | linear は driver の pool のままにし、GPU の transfer で写す（§7.5 の 3.） |
| R4 | firmware の DPB と app の DPB がずれる（app の規則違反、seek の後の RESET の抜け）、app の DPB の slot の数が firmware の求める数より少なく空きの index が無い | POC の比べで log。RESET で FLUSH_INSTANCE（SPS が同じなら組を保つ）。libmedia は seek の後に RESET を記録する。空きが無い時は ERROR（N は firmware の求める数 + 1 なので、規則を守る app では起こらない見込み） |
| R5 | DEC_PIC ごとの read pointer が効かない | ring を続けて使う形（§7.3） |
| R6 | 割り込みの番号・形（level・edge）が違う | poll の 1 ms に落とす（Linux の driver の fallback と同じ） |
| R7 | NOC の制御（`vdec_noc_ctrl`）が要る | vendor の GPL の module の値を引用の規則で読む |
| R8 | FBC の pool の連続の大きな allocation（4K で 1 枚 約 14 MB × 18）が取れない | 解像度の上限を下げて報告し直す（1080p の pool は 約 56 MB）。p007 で測る |
| R9 | SLC の flush の費用・global な flush が他の session を遅らせる | 費用を測る。重ければ video の image の memory を SLC の迂回で map する（§17 D6 の代わり） |
| R10 | Wave420L の参照の license（Q322-2） | 答えが C なら encode は WS330 の software だけ（p008〜p011 を取り下げ） |
| R11 | firmware の file の版の違い（`chagall.bin` と `wave511_dec_fw.bin`） | SHA を比べ、vendor の kernel が使う物を選ぶ |
| R12 | 1 つの queue で decode を同期に待つので app の描画の submit が遅れる | p007 の非同期 |
| R13 | 今の libmedia の readback（DISTINCT は flight 1、`vkvideo-runtime.c:1696,2582`）が uncached の memory を CPU で読む（PowerVR に host cached の type は無い、`vk-device.c:538-540`。uncached の書き込みで 8 MB に約 30 ms、`render.c:432-436`、読みはもっと遅い見込み）ので、1080p60 は届かず 30 も際どい | 1080p60 の受け入れは WS318 の GPU の道（p006b・p007a・p009）の後の p012。WS243 の I7・I9 と同じ問題 |
| R14 | VPU の DMA が L2 を通る（tree は `dma-coherent`、vendor は L2 を書き戻す）と、uncached の view だけでは一貫しない | p002 の probe で確かめ、通るなら kernel の data cache の口で範囲を書き戻し・捨てる（§9.2） |

## 17. 判断（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | PowerVR の device の family 0 に video を足す | ユーザーの決定（Vulkan Video として見せる、OS の API）。libmedia は decode と transfer を同じ device に求め、PowerVR の executor は memory・image・queue・fence を既に持つ（WS236 J1 と同じ理由） | 別の VkPhysicalDevice（全部の object を作り直す）、別の cdev と ioctl（ユーザーの決定に反する） |
| D2 | DPB_AND_OUTPUT_DISTINCT、DPB は driver の FBC の pool | Wave5 の参照は FBC の形で sample できない。出力の linear は別の buffer に同時に書かれる（wave5-vpu-dec.c の linear と non-linear の登録）。今の libmedia は DISTINCT を扱える | COINCIDE（参照が linear でないので不可）、DPB の image に FBC を置く（firmware が FBC の buffer を自分で選ぶので Vulkan の slot と結べない） |
| D3 | driver が Vulkan の slot と FB の index を結び、参照に使われていない index i を選び、1 枚の frame buffer の更新で linear[i] を宛先に差し替え、display の flag で i だけを使わせる。更新が使えない時は driver の pool と GPU の transfer | Wave5 の index は FBC と linear の組（§7.4）。参照の FBC を壊さずに zero copy にできる。今の libmedia の出力 1 枚でも参照を持つ絵が decode できる | 第 1 版の「linear を 1 枚だけ空ける」（参照を持つ絵で空きの FB が無くなる、review 1 の 1 番）、常に pool と copy（4K で毎秒 約 0.7 GB の読み書き）、DMA engine の copy（1 回ごとの poll、行ごとは遅い、GPU でない） |
| D4 | VPU の内部の buffer は driver が kernel の連続の memory で取り、session の memory の要求は 4 KiB の 1 つ | 大きさ（task の buffer）は INIT_SEQ の後にしか分からない。FBC は 1 枚ずつ連続が要る。WS236 と同じ | session の memory に置く（作成の時に大きさが決まらない、32 個の binding が要る） |
| D5 | decode は同期で始める | 正しさが分かりやすい（nvrtx D2・Pi の最初と同じ）。速さは p007 | 最初から非同期（fence の点の kind を新しく作る手間を正しさの確かめの前に入れない） |
| D6 | SLC は decode・encode の前後に flush で扱う | 既存の command で済み、memory の map を変えない | video の memory を SLC の迂回で map（bind の時の張り替えが要る、p007 の代わり）、全部の memory を迂回（GPU の描画が遅くなる） |
| D7 | parameter set の書き出しは `src/drivers/vpu/` に新しく書き、`rpi4-h264.c` と共有しない | §2.6 の遅い抽象化。H.265 の書き出しも要り、Pi の物は VUI の書き換えに寄っている。完了した WS236 の code を触らない | 共有に移す（bcm2711 の driver の変更と再試験が要る） |
| D8 | engine の口は PowerVR の driver の中の header に置く | 今の使い手は 1 つ。汎用の口は WS323 で同じ形が要ると分かってから | `include/drivers/` に汎用の video engine の口（早い抽象化） |
| D9 | H.265 は Main だけ、encode は IDR と P、B 無し | WS318 Q318-8・Q318-9 の Q1 の判断（A） | Main10（libmedia が出さない） |
| D10 | attach の thread で on → product code → root を待って firmware → boot まで済ませ、通った時だけ engine を登録。`operations()` はそれで固定し、電源は v1 では on のまま | capset は fd を開く時に読まれる。電源の状態と確かめの状態を分けないと video が出ない・消える（review 1 の 2 番） | 最初の session で lazy に boot（capset が嘘になる）、idle の off（`operations()` が揺れる。省電力は後の Phase で firmware の sleep を使う） |
| D11 | 実機の確かめを先に probe で（p002） | WS236 §12 の教え（stateful の firmware の振る舞いは読むだけでは決まらない）。R1〜R3・R5 の答えで p004 の形が変わる | Vulkan の道を全部作ってから確かめる（直しが大きい） |

## 18. 問い（ユーザー・Q1 へ）

phase.md の「判断を待つ点」に背景・用語・選択肢・得失・推しを書いた。要約:

| ID | 問い | 推し | 誰が |
| --- | --- | --- | --- |
| Q322-1 | VPU の firmware（`chagall.bin`・`monet.bin`、Chips&Media の binary だけの再配布・逆解析の禁止）を zedBSD-firmware に入れ、package で配るか | A: 入れる（PowerVR の firmware と同じ条件と形） | Q1（先例で決められる見込み） |
| Q322-2 | Wave420L（H.265 encode）の register の参考に、StarFive の soft_3rdpart の Wave4 の source（file の SPDX は LGPL-2.1 OR BSD-3-Clause、dir の LICENSE.txt と header の文は proprietary）を値と手順の出典として使ってよいか | A: file の SPDX の BSD-3-Clause を採り、値と手順だけを block ごとに出典つきで使う（code は写さない）。食い違いは記録に残す | ユーザー（危険の受け入れ） |
| Q322-3 | 受け入れの解像度と速さ: decode は 1080p60 と 4K30 まで出して確かめるか、1080p30 だけか | A: 4K まで報告し、受け入れは p004〜p007 で 1080p30 の再生と 4K30 の decode の正しさと時間、1080p60 の再生は WS318 の GPU の道の後の p012（R13） | ユーザー（範囲） |
| Q322-4 | libvulkan の H.265 と encode の転送をどの WS が持つか（WS322 と WS319） | 先に着手する方。WS322 が先なら p005・p010 | Q1 |
| Q322-5 | ws.md の題の「H.264 encode は … libavcodec」を「独自の software（WS329）」に直す | 直す（2026-11-01 の Q318-2 に合わせる） | Q1（master.md の行も） |
| Q322-6 | 実装の Phase（p002〜p004）も WS318 の completed の前に始めてよいか（2026-11-01 の前倒しが明示したのは p001） | A: 始める（H.264 decode は今の libvulkan・libmedia で確かめられ、WS318 と file が重ならない） | Q1（ユーザーの指示の範囲の読み。違えばユーザーへ） |
| Q322-7 | libmedia の DISTINCT の readback の道（どの実の driver でも未走行）に bug が出た時、WS322 の中で直すか、WS318 に渡すか | A: WS322 p004 の中で最小に直す（WS318 は p006 で frame の model を作り直すので、その前の今の code の直しは小さく済む）。WS318 の担当に知らせる | Q1 |

## 19. HAL

- 変えない。要る物は全部 kernel の口と driver で足りる: register の map（`kern_device_map`）、割り込み（`kern_irq_register`・`kern_irq_set_mode`）、物理の連続の memory（`kern_pmem_alloc_limited`）、uncached の view（`kern_pmem_map_uncached`）、firmware の file の読み（kernel の file の口、`powervr/firmware-file.c` と同じ）、data cache の書き戻し（R14 の時、`kern/dcache.h`）、時間と待ち（`kern/clock.h`・`kern/sched.h`）。power domain・clock・reset は platform の driver（`vf2-pmu.c`・`vf2-crg.c`）の表の追加で、HAL の責務ではない。
- 新しい code は HAL の header を include しない（guardrail「driver は HAL を知らない」）。

## 20. 既存の file への変更（所有の重なり）

| file | 変更 | 注意 |
| --- | --- | --- |
| `src/drivers/platform/visionfive2/vf2-pmu.c`・`.h` | VDEC（3）・VENC（6）の domain を許す | VF2 の他の WS（BUG-387 の P4 の作業）と重ならないか Q1 が見る |
| `src/drivers/platform/visionfive2/vf2-crg.c` | Wave511・Wave420L の clock と reset、AXIMEM0・AXIMEM1 の reset（clock は無い）、NOC の reset（31・32・52、要れば）の表の行 | 同上 |
| `src/kern/platform/visionfive2.c` | VPU の place（builtin）と attach の呼び出し（`#if` の中） | 同上 |
| `src/drivers/gpu/powervr/powervr.h`・`render-runtime.c`・`vk/vk-device.c`・`vk/vk-image.c`・`image-state.c`・`vk/vk-command.c`・`vk/vk-queue.c`・`vk/vk-transfer.c`・`vk/vk-object.c`・`vk/vk.h`、p007 は `render-queue.c`・`render-private.h` | §8 の hook | PowerVR の他の WS（WS291 ほか）と重ならないか Q1 が見る |
| `userland/tests/vkvideo-probe/`（p004） | DISTINCT の対応 | WS083 の道具 |
| `userland/desktop/libvulkan/`（p005・p010） | H.265・encode の転送 | WS319・WS318 と調整 |
| `userland/firmware/`（README、新しい `wave511/`・`wave420l/`）、`config/drivers/firmware.list` | firmware の package | |
| `platform/riscv64/vmunix.mk`（か VF2 の file の一覧）、`config/drivers/architecture/riscv64.drivers` | 新しい file と config（`CONFIG_DRIVER_VF2_VPU`、既定は p004 の実機の確かめまで n、`CONFIG_DRIVER_VF2_VPU_PROBE` は常に n） | |

新しい file: `src/drivers/vpu/wave5-core.c`・`wave5-decode.c`・`wave5.h`、`src/drivers/vpu/wave4-core.c`・`wave4-encode.c`・`wave4.h`（encode の Phase）、`src/drivers/vpu/vpu-h264-ps.c`・`vpu-h265-ps.c`・`vpu-codec.h`、`src/drivers/platform/visionfive2/vf2-vpu.c`・`.h`、`src/drivers/gpu/powervr/vk/vk-video.c`・`vk-video-h264.c`・`vk-video-h265.c`・`vk-video-encode.c`、試験は `plan/ws322/tests/`。

## 21. review

- review 1（design-reviewer、2026-11-01）の 27 件（blocker 1）と反映は [design-review.md](design-review.md)。第 2 版はその反映の後の版。
