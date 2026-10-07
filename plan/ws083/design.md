# WS083 の設計: Vulkan Video の H.264 decode と i915 の VCS・MFX（ws083-p001）

**第 3.1 版**（2026-10-07 夕、P1）。第 1 版（同日、kernel-and-driver-designer）に design-reviewer の review（§15）の blocking 4・should-fix 17・minor を織り込んだのが第 2 版（反映先は §14）、第 2 版への再 review の blocking 2・should-fix 12・minor 12 を織り込んだのが第 3 版（反映先は §16）。

範囲は [ws.md](ws.md) と Q1 の 2026-10-07 の指示: p001 はこの設計、p002 は libvulkan の骨組み（`VK_KHR_video_queue`・`VK_KHR_video_decode_queue`・`VK_KHR_video_decode_h264` の queue family・capability・format・session・parameters・memory の bind・`vkCmdBegin/End/ControlVideoCodingKHR`・`vkCmdDecodeVideoKHR` の記録と host の試験）、その後は host で build・試験できる物（VCS の ring・context の立ち上げの code、MFX AVC の command stream の builder と host の fixture）。実機（Dell Latitude 5330、Alder Lake-P、Gen12 LP、`8086:46a8`）の確認は 5330 が戻ってから。最初の到達点は H.264 8 bit 4:2:0 progressive、I frame から、次に P・B と DPB。encode・H.265・AV1 は範囲の外。Mesa・intel-media-driver の code は写さず、公開の PRM と両 project の **事実** だけを使う（§7）。

この文書の「事実」は worktree `agent/p1`（main `6eb93d0fb` の上）の file を読んだ物で、file と行を添える。「決定」は D 番号、「人の判断」は H・HD 番号（§10）、確かめていない物は §11 に集める。

## 0. 読んだ物と前提

- 規則: `AGENTS.md`、`plan/guardrail.md`（HAL・UAPI・license・kernel が include できる libc の header は `libc/vulkan/*` だけ）、`plan/coding-style.md`（全文）、`plan/ws031/i915-rebuild-rules.md`（`intel/` の header の書き方、転記の出典・SHA、`.inc` は配列の初期化子の中で include する行の並びだけ）、`plan/ws075/ws.md`（HAL・UAPI の変更は `proposed/` に提示して承認まで当てない）、`plan/ws167/phase001/phase.md`・`phase002/phase.md`（Kei GPU command protocol、`GPU_OP_OWN_FIRST = 0x10000` から zedBSD だけの command）。
- libvulkan: `userland/desktop/libvulkan/`（`internal.h`・`instance.c`・`device.c`・`commands.c`・`commands-generated.inc`・`external-properties.c`・`context.c`・`codec.h`・`dispatch.c`・`dispatch-table.inc`・`query.c`・`Makefile`・`exports.map`・`tools/maintain-*.noct`・`README.md`）、`include/libc/vulkan/`（`vulkan_core.h`・`vulkan_external.h`・`API-PROVENANCE.md`）、`include/uapi/gpu.h`・`gpu-op.h`・`gpu-job.h`・`gpu-fence.h`。
- i915: `src/drivers/gpu/i915/`（`i915.c`・`engine.[ch]`・`context.[ch]`・`submit.[ch]`・`request.[ch]`・`request-queue.h`・`session.[ch]`・`worker.[ch]`・`reset.c`・`job.c`・`command.c`・`irq.c`・`mmio.[ch]`・`device.c`・`device-info.[ch]`・`defaults.c`・`gt-power.c`・`workarounds.c`・`firmware.[ch]`、`intel/engine-table.inc`・`forcewake-ranges.inc`・`gt-regs.h`・`commands.h`・`lrc-offsets.h`・`genxml.h`・`mocs.h`、`render/internal.h`・`dispatch.c`・`instance.c`・`command.c`・`draw.c`・`image.[ch]`・`memory.h`・`gfx.h`・`heap.h`・`object.h`・`vulkan.c`・`batch.h`）、`src/drivers/gpu/venus/venus.c`・`transport.c`、`plan/ws031/tests/run-vk-host-tests.sh`、`plan/uat/2026-10-05pm/dmesg.txt`。
- 事実の参照（code は写さない）: Mesa 25.0.7（worktree の `build/mesa-tools/mesa-25.0.7/`。`src/intel/genxml/gen120.xml` の SHA-256 `e2452c7d…` は `intel/genxml.h` の記載と一致）: `src/intel/vulkan/anv_video.c`・`genX_cmd_video.c`・`anv_image.c`・`anv_formats.c`・`anv_physical_device.c`、`src/intel/genxml/gen{120,110,90,80,75}.xml`。Khronos の registry `/usr/share/vulkan/registry/vk.xml` と header `/usr/include/vulkan/vulkan_core.h`（Debian `libvulkan-dev` 1.4.309.0-1、SHA-256 `39f358bb99f7be5206524d119de4a5b5ab5c515c329c2857ff395d37e6387b4c`）、`/usr/include/vk_video/`（同じ package。Mesa 25.0.7 の `include/vk_video/` と 4 file の SHA-256 が一致）。
- 道具: host の `ffmpeg` 7.1.5（`libx264` あり）。

## 1. 今の形（事実）

### 1.1 libvulkan は薄い転送器

- 公開 API は Vulkan 1.0 の 137 command と WSI・properties2・external の KHR で、`physical->properties.apiVersion` は `VK_API_VERSION_1_0` に固定（`instance.c` 1005〜1009）。instance の `apiVersion` は 1.1 を要求して開く（`instance.c` 756）。
- `vkGetPhysicalDevice*` は backend に `GPU_OP_GET_PHYSICAL_DEVICE_*` を送って返事を decode する。queue family は `physical_load_queues`（`instance.c` 1130〜1260）が backend の返事をそのまま cache し、`queueFlags` を**濾さない**。
- 拡張の有無は libvulkan が自分で決める（`physical->supported_extensions`、`instance.c` 1075〜1093）。`vkCreateDevice` は名前を `strcmp` で照合する（`device.c` 306〜382）。properties2 系は `external-properties.c` が手元で答える。
- 記録: 各 `vkCmd*` は `command_record_begin`（`commands.c` 535〜560）で `[opcode u32][0 u32][cmdbuf id u64]` を書き、引数を続け、`command_record_finish`（564〜618）が追記する（1 MiB か `max_resource_bytes` を超えると途中で backend に流す。流すのは decode 済みの prefix で、GPU は走らない）。
- 入口の表 `dispatch-table.inc`・`api-commands.tsv` は `tools/maintain-dispatch.noct` が公開 header から作り、件数 173 を検査する。
- 公開 header `include/libc/vulkan/vulkan_core.h` は virglrenderer 1.1.0 の `venus-protocol/vulkan_core.h`（1.3.269）からの選択（`API-PROVENANCE.md`）。その **pinned の入力は disk に無い**。video の **enum の値**（`VK_STRUCTURE_TYPE_VIDEO_*`、`VK_IMAGE_USAGE_VIDEO_DECODE_*`、`VK_QUEUE_VIDEO_DECODE_BIT_KHR`、`VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR`・`DPB_BIT_KHR`（2348〜2349）、`VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR`（150）、`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`）はあるが、`VkVideo*` の構造体・関数・`StdVideo*`・`VK_KHR_synchronization2` は無い。
- opcodes は `include/uapi/gpu-op.h`（ws167）。`GPU_OP_PROTOCOL_VERSION`（26 行、enum の前）は tree の他の所で使われていない。

### 1.2 i915 の Vulkan 実行器（`src/drivers/gpu/i915/render/`）

- `dispatch.c` が opcode を graphics object → command 記録 → 範囲 → transport・instance の順に渡す。知らない opcode は reader を poison し submission を失敗させる。
- **同期実行**: `i915_queue_submit`（`command.c` 3396〜）は command buffer の操作列を 1 つの batch に記録し、`draw.c` 426 の `drv_i915_worker_run_sync(session->vk->i915, &session->gpu->contexts[I915_ENGINE_RCS0], batch_va)` で最後まで走らせてから返事する。barrier・semaphore は decode するだけ。submit の先頭の queue の id（u64）は**読み捨てる**。
- 失敗の写像 `i915_command_result`（`command.c` 316〜333）: ENOMEM → `VK_ERROR_OUT_OF_DEVICE_MEMORY`、ETIMEDOUT・EIO → `VK_ERROR_DEVICE_LOST`、他（EINVAL 等）→ `VK_ERROR_INITIALIZATION_FAILED`（`vkQueueSubmit` の返せる値ではない）。
- `i915_instance_get_device_queue2`（`render/instance.c` 865〜900）は family・index を読み捨て、queue を object 表に入れるだけ（「XXX: there is one timeline, on RCS0」）。
- memory: `VkDeviceMemory` は libvulkan が export する blob。memory type は 1 つ、`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`（`instance.c` 482〜492）。batch の object は 1 MiB（`heap.h` 109 `I915_GFX_BATCH_BYTES`）、溢れは `batch.h` の overflow で数える。
- image: linear と、depth・multisample だけ Y tile（`image.c`）。planar（NV12）は無い。
- queue family は 1 つ（graphics|compute|transfer、`instance.c` 507〜545）。
- capset（`vulkan.c` 268〜296 `i915_render_capset_fill`）: 168 byte、byte 160 に vendor tag `0x5a424453`、byte 164 に vendor flags 7（OPAQUE|STRICT_QUEUE|QUIESCE）。`I915_RENDER_CAPSET_WORDS` 64（256 byte、`GPU_CAPSET_MAX` 256）。

### 1.3 i915 の GT と engine

- GT の engine の表 `intel/engine-table.inc` は RCS0・BCS0・**VCS0（0x1c0000）**・VCS2・VECS0。5330 の実機の dmesg（`plan/uat/2026-10-05pm/dmesg.txt` 68 行）に `engine[2] vcs0: class=1 inst=0 base=0x1c0000 reset_domain=0x8 ctx_size=8192 caps=0x3`。`drv_i915_engines_init`（`engine.c` 385〜440）は GT が報告する全 engine に status page・execlists の state・kernel context を作り、`defaults.c`（`drv_i915_engines_record_defaults`）は毎 boot に全 engine の既定の context image を execlists で記録する（VCS0 の execlists の実績）。
- driver の **engine record**（request の番号と queue）は別物: `request-queue.h` 33〜35 で `RCS0 0`・`BCS0 1`・`COUNT 2`。`i915.c` 143〜160 の `drv_i915_publish` の loop は index 0 を RENDER、**それ以外を全部 COPY** にする。`reset.c` 308・347・391、`session.c` 163・284 は `I915_ENGINE_COUNT` で回り、`job.c` 319〜325・`command.c` 423〜425 は RCS0・BCS0 を名で選ぶ。
- LRC: `context.c` が class で `gen12_xcs_offsets` / `gen12_rcs_offsets` を選ぶ。request の flush は class で分かれ、video は `MI_INVALIDATE_BSD`（`request.c` 534〜575）。
- 割込み: `irq.c` 466〜474 が VCS の class の割込みも enable し、class・instance で engine に引き当てる。
- forcewake: `I915_FORCEWAKE_MEDIA_VDBOX0` ほか。device の start は 5 domain 全部を service の間持つ（`device.c` 118〜129、`worker.c` 310）。
- 電源: `gt-power.c` 148〜166 で media の power gating を有効。
- firmware: GuC・HuC を使わない（execlists）。
- **worker は RCS0 だけ**: `worker.c` 597〜605（context の作成で RCS0 以外は record だけ）、1388〜1394（`i915_worker_run` は RCS0 以外を ENOTSUP）、hardware context の表は 32 個（`I915_WORKER_CONTEXTS`、68 行）、submit・待ちは `worker->render_index` 固定（640・1441〜1442・1539〜1540）。`drv_i915_worker_run_sync(struct i915_device *, struct i915_context *, uint64_t)`（`worker.h` 95）。timeout は 1596〜1607 の「XXX: a hang. No reset, no recovery」で ETIMEDOUT を返すだけ。

### 1.4 Venus（QEMU の host の renderer）

- zedBSD の Venus の host（virglrenderer の fork、`plan/ws014`）は **168 byte の capset を同じ magic `0x5a424453` で**出し、flags は 3・7・15（kernel の `venus/transport.c` 35〜39・1860〜1893、libvulkan `context.c` 27〜32・165〜190）。stock の Venus は vendor 部を持たない。libvulkan は 168 byte の時だけ vendor 部を読み、flags を**完全一致**で比べる。
- Debian 13 の Mesa 25.0.7 の ANV は video decode を既定で無効（`anv_physical_device.c` 2556）。QEMU の host が Venus の wire に video の command を持つかは未確認（§11）。
- 結論: QEMU/Venus では video の queue family も拡張も出さない（D2・D3）。

### 1.5 Mesa（ANV 25.0.7）と genxml から取った事実

- H.264 decode の capability（`anv_video.c` 112〜161）: `minBitstreamBufferOffsetAlignment` 32、`SizeAlignment` 1、`pictureAccessGranularity`・`minCodedExtent` 16x16、`maxCodedExtent` 4096x4096、`maxDpbSlots` 17、`maxActiveReferencePictures` 16、`maxLevelIdc` 5.1。
- session の scratch（`anv_video.c` 405〜421）: intra row store `width_in_mb * 64`、deblocking filter row store `width_in_mb * 64 * 4`、BSD/MPC row `width_in_mb * 64 * 2`、MPR row `width_in_mb * 64 * 2`。
- 画像ごとの direct MV buffer（`anv_image.c` 883〜894）: `w_mb * h_mb * 128` byte、64 KiB 整列。
- 命令列（`genX_cmd_video.c` 882〜1256）: §6.3 の順。short format（910〜918）。参照の並べ方: `MFX_PIPE_BUF_ADDR_STATE` の Reference Picture[i] は `pReferenceSlots[i]` の i、`dpb_slots[slotIndex] = i` を作り、`MFD_AVC_DPB_STATE`・`MFX_AVC_DIRECTMODE_STATE` は**その i** で埋め、`MFD_AVC_PICID_STATE` の Picture ID[i] = slotIndex、残りは 0xffff（1043〜1085）。`slotIndex < 0` の参照は飛ばす。
- **scaling list**（1125〜1153）: `StdVideoH264ScalingLists` の list は **zig-zag の scan 順**で、hardware の Forward Quantizer Matrix は **raster 順**。ANV は `Forward[m*16 + zscan[q]] = list[q]`（4x4）、`Forward[zscan8[q]] = list8x8[k][q]`（8x8）。
- genxml: `MFX_WAIT` は `gen75.xml` 2015〜2021 で length 1、**Command Subtype 既定 1**（27:28）、SubOpcode 0、Command Type 3。Gen12 で効く `MFX_AVC_IMG_STATE` は `gen110.xml` 1900 の 21 dword の定義で、Frame Size（1907）は **16 bit**、Frame Width・Height は 8 bit の MB 数 −1（`gen80.xml` 1775〜1784 は Gen8 の 14 dword の定義で、出典にしない）。4096x4096 は 65536 MB で Frame Size に入らない（level 5.1 の MaxFS は 36864 MB）。
- tiling: Gen12 LP の MFX の宛先は Tile Y（`MFX_SURFACE_STATE` の Tile Walk YMAJOR、ANV も同じ）。ANV は `Y Offset for U(Cb)` と `Y Offset for V(Cr)` の両方に chroma plane の行を入れる（937〜938）。
- format の feature（`anv_formats.c` 617〜619）: video の format は `VIDEO_DECODE_OUTPUT`・`VIDEO_DECODE_DPB` の feature を出す。

### 1.6 Vulkan の registry の依存

`vk.xml`（1.4.309）: `VK_KHR_video_queue` は `(VK_VERSION_1_1+VK_KHR_synchronization2),VK_VERSION_1_3`、`VK_KHR_video_decode_queue` は `VK_KHR_video_queue+(VK_KHR_synchronization2,VK_VERSION_1_3)`、`VK_KHR_video_decode_h264` は `VK_KHR_video_decode_queue`。`VkVideoReferenceSlotInfoKHR.pPictureResource` は `optional="true"`（7439 行）。spec version は 1.4.309 で video_queue 8・video_decode_queue 8・video_decode_h264 9・synchronization2 1（`/usr/include/vulkan/vulkan_core.h` 8938・9234・9716 行）。NV12 の format と plane の aspect は `VK_KHR_sampler_ycbcr_conversion`（1.1 core）の物。

## 2. 決定の一覧

| ID | 決定 | 理由 | 選ばなかった案 |
| --- | --- | --- | --- |
| D1 | **decode は short format**（hardware が slice header を読む）: `MFD_AVC_DPB_STATE`・`MFD_AVC_PICID_STATE`・`MFD_AVC_SLICEADDR`・`MFD_AVC_BSD_OBJECT` で slice を渡す | `StdVideoDecodeH264PictureInfo` に ref list の修正・重み表は無い。ANV も short format | long format: 実機で short が動かない時の fallback（U1） |
| D2 | video の API は **native の i915 の backend だけ**で名乗る。capset に **native の語**（176 byte、byte 168 tag、byte 172 の bit 0）を足し、Venus の fork の vendor flags の空間は使わない（§4.3、HD1） | fork と stock の Venus は 176 byte を出さない。flags の完全一致の方針を緩めない | vendor flags に bit を足す（第 1 版）: fork と空間を共有し完全一致を崩す |
| D3 | libvulkan は video の拡張を出さない session で、backend の `queueFlags` から `VK_QUEUE_VIDEO_DECODE_BIT_KHR`・`ENCODE` を落とす | 規格の違反を防ぐ | そのまま |
| D4 | 新しい command は **zedBSD 独自の opcode**（`GPU_OP_OWN_FIRST` から 14 個）。差分は `proposed/gpu-op-video.diff`、承認を待つ（H1） | ws167 の約束 | Venus の番号を推測する |
| D5 | **Vulkan 1.0 の device のまま**名乗り、規格に合わない点を README・API-PROVENANCE に全部書く（N1 apiVersion 1.0 で video の拡張、N2 `VK_KHR_sampler_ycbcr_conversion` 無しで NV12 と `PLANE_0/1` の aspect、N3 OPTIMAL の image に `vkGetImageSubresourceLayout` を答える（zedBSD の私的な約束）。HD6） | 1.1 は別 WS の規模。利用者は自前の app | 1.1 に上げる |
| D6 | `VK_KHR_synchronization2` は **libvulkan の中の翻訳**（6 command を 1.0 の command へ）。新しい opcode は要らない（H2） | 実行器は barrier を無視し同期で終えるので意味を保つ | backend に新 opcode |
| D7 | queue family は **index 1、`VK_QUEUE_VIDEO_DECODE_BIT_KHR` だけ、queue 1 つ** | VCS0 は別の engine | family 0 に足す |
| D8 | DPB と出力は **coincide**、`SEPARATE_REFERENCE_IMAGES`、DPB は 1 層の image を slot ごとに | MFX の post deblocking の宛先がそのまま参照（ANV と同じ） | distinct・配列の DPB |
| D9 | 出力・DPB の format は NV12 だけ、tiling OPTIMAL = **Tile Y**、pitch は 128 B の倍数、plane は 32 行の倍数 | Gen12 LP の MFX | linear（U3） |
| D10 | direct MV buffer は **session の memory**（bind index 4〜）に置く | 実行器の image は private の領域を持てない | ANV の image の private binding |
| D11 | engine record `I915_ENGINE_VCS0`（index 2、`COUNT` 3）。session の VCS0 の hardware context は**遅延で**、最初の `vkCreateVideoSessionKHR` で作る。worker は render 32・video 8 の 2 つの context 表を持ち、作成は device の mutex の下で 1 本、session の close で解放（§6.1） | 全 session に VCS の context を作ると無駄 | open で作る |
| D12 | 実行は**既存の同期の形**: decode の batch を `drv_i915_worker_run_sync(device, &contexts[VCS0], batch_va)` で走らせる | 実行器全体が同期 | VCS だけ非同期 |
| D13 | MFX の command の定義は **`intel/genxml-video.h`**（新、`intel/genxml.h` と同じ書き方: Mesa の genxml の値を出典・SHA 付きで転記） | WS031 と同じ license の扱い（H4） | PRM から手で起こす |
| D14 | 公開 header: `include/libc/vulkan/vulkan_video.h`（3 拡張 + sync2 の宣言）を `tools/maintain-video.noct` が **Khronos Vulkan-Headers 1.4.309**（Debian `libvulkan-dev` 1.4.309.0-1 の `/usr/include/vulkan/vulkan_core.h`、SHA は §0）から選び、`include/libc/vulkan/vk_video/`（`vulkan_video_codecs_common.h`・`vulkan_video_codec_h264std.h`・`vulkan_video_codec_h264std_decode.h`、Apache-2.0）を同じ package から写す。network は要らない（H3） | 1.3.269 の pinned の入力は disk に無い。video の構造体の配置は 1.3.238 の確定以降変わらず、std の header は 1.0.0。core の 1.3.269 の選択とは別の file にし、混ぜない | network で 1.3.269 を取る（取れても版の違いは同じ） |
| D15 | result status query と inline query は**最初の目標に入れない** | 規格で任意 | 最初から |
| D16 | 実機の判定は **frame ごとの hash**（crop 後の NV12 の Y・UV の SHA-256）を host の ffmpeg の参照と比べる。試験の stream は**小さい合成の stream を tree に**（`plan/ws083/tests/streams/`、作った script・参照の hash と一緒に）。AGENTS.md の「試験の image は tests/ の config.mk と個別の file の複写だけ」に合わせる（HD4） | stream を build/ に置くと image の規則に合わない | 実写の stream・ITU-T の conformance の stream（HD4 の選択肢） |
| D17 | **kernel は app の値を信じない**: 実行器は MFX の command を組む前に SPS・PPS・picture・slice の値を検べ（§6.6）、合わなければ**その picture を走らせずに飛ばす**（`VK_SUCCESS`、出力の中身は未定義、log の 1 行）。規格は不正な bitstream の decode の結果を未定義とするだけ | WS121 は信頼できない stream を流す。MFX に範囲外の値を渡すと読み越し・hang の危険 | 検べない（第 1 版） |
| D18 | app の valid usage の違反（未 bind、reset 前、family の混在、slot の image の不一致）で submit を拒む時は `VK_ERROR_DEVICE_LOST`（`vkQueueSubmit` が返せる値）にする。batch の溢れは ENOMEM → `VK_ERROR_OUT_OF_DEVICE_MEMORY`。`i915_command_result` の他の用途は変えない | EINVAL → `VK_ERROR_INITIALIZATION_FAILED` は `vkQueueSubmit` の値でない | 今の写像 |
| D19 | 実機の確認（p005）と engine reset（p007）までは、実行器が native の語の video の bit を立てるのは **boot の `i915.debug=video`** がある時だけ。`i915_boot_word`（`device.c` 801〜826）は値の完全一致で、kernel の boot の parser（`src/kern/boot.c` 379〜389）は `i915.debug` に `off`・`display` しか受けない。p003b で boot.c に `video` と `display,video`（`,` で区切った語の並び）を足し、その試験と `i915_boot_word` の語の照合を直す（kernel の boot.c は i915 の外。p003b の範囲に入れる）。p008 で既定へ | 実機で未確認の engine を app に見せない。boot の行だけで T1 が試せる | 既定で出す |
| D20 | `maxCodedExtent` は 4096x4096 のまま。picture ごとに**総 MB 数 ≤ 36864**（level 5.1 の MaxFS、Frame Size の 16 bit に入る）を D17 で検べる | 縦長の動画（1080x1920 など）を受けたい。Frame Size の field の幅 | 4096x2304 にする |
| D21 | 使わない参照の address（Reference Picture[i]、Direct MV Buffer[i]）は **0 にせず**、その decode の宛先の image と書き込みの MV buffer を入れる | PPGTT の 0 番地は対応付けが無く、hardware が先読みすると fault | 0（ANV） |
| D22 | parameters の SPS・PPS は**追加の時に 1 件ずつ**確保する（id → pointer の表、SPS 32・PPS 256 の枠） | 固定の表は SPS 32 + PPS 256 で約 150 KB の kernel memory | 固定の表 |
| D23 | NV12 の OPTIMAL の `vkGetPhysicalDeviceFormatProperties` は `optimalTilingFeatures = VIDEO_DECODE_OUTPUT | VIDEO_DECODE_DPB`（1.0 の 32 bit の feature にある bit）。video の usage 以外（SAMPLED 等）を video の profile 付きで聞かれたら `VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR` | 規格と ANV | 0 にする（第 1 版） |
| D24 | golden の自己参照を避ける: host の golden は **genxml の XML を読む独立の decoder**（`plan/ws083/tests/genxml-decode.py`、Mesa の XML を data として読む、tree に写さない）で field に戻し、`StdVideo*` の入力から期待した field と比べる。genxml は import・exclude・上書きで継承する（gen120 → gen110 → gen90 → gen80 → gen75）ので、decoder はこの解決を実装し、解決自体を試験する（例: Gen12 の `MFX_AVC_IMG_STATE` は gen110 1900 の 21 dword で gen80 1775 の 14 dword ではない、`MFD_AVC_SLICEADDR` 4・`MFD_AVC_BSD_OBJECT` 7 は gen110 1885・1874）。入力の Mesa の tree（`build/mesa-tools/mesa-25.0.7`、5 file の SHA は §7）を SHA で固定し、無い・違う時ははっきり失敗する | builder の bit 位置の誤りを builder 自身の golden では見つけられない | 手の検算だけ（第 1 版） |

## 3. API の面（libvulkan が報告する物）

### 3.1 拡張

capset の native の語に video の bit がある session（§4.3）でだけ、`vkEnumerateDeviceExtensionProperties` に次を足す。spec version は D14 の header の値（8・8・9・1）。`VK_KHR_video_decode_h264` の版 9 の意味（setup の参照 picture は `StdVideoDecodeH264PictureInfo.flags.is_reference` が立つ時だけ DPB の slot を有効にする）で実装する。

| 拡張 | 条件 | bit |
| --- | --- | --- |
| `VK_KHR_synchronization2` | 一覧に出すのは native の bit がある時だけ。`vkCreateDevice` の時に instance の `VK_KHR_get_physical_device_properties2` を検べる | `VULKAN_DEVICE_SYNCHRONIZATION2 = 256` |
| `VK_KHR_video_queue` | `VK_KHR_synchronization2` も enable | `VULKAN_DEVICE_VIDEO_QUEUE = 512` |
| `VK_KHR_video_decode_queue` | `VK_KHR_video_queue` | `VULKAN_DEVICE_VIDEO_DECODE_QUEUE = 1024` |
| `VK_KHR_video_decode_h264` | `VK_KHR_video_decode_queue` | `VULKAN_DEVICE_VIDEO_DECODE_H264 = 2048` |

`VkPhysicalDeviceSynchronization2FeaturesKHR.synchronization2` は TRUE（D6）。規格に合わない点（D5 の N1〜N3）は `userland/desktop/libvulkan/README.md` と `include/libc/vulkan/API-PROVENANCE.md` に書く。

### 3.2 queue family

backend が返す family を 2 つにする（`render/instance.c` の queue family の返事）:

| index | queueFlags | queueCount | timestampValidBits | minImageTransferGranularity |
| --- | --- | --- | --- | --- |
| 0 | GRAPHICS \| COMPUTE \| TRANSFER（今のまま） | 1 | 0 | 1,1,1 |
| 1 | `VK_QUEUE_VIDEO_DECODE_BIT_KHR` | 1 | 0 | 0,0,0 |

- `vkGetPhysicalDeviceQueueFamilyProperties2KHR` の pNext: `VkQueueFamilyVideoPropertiesKHR.videoCodecOperations`（family 1 は `DECODE_H264`、他は 0、新 op `GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES` で 1 回取り cache）、`VkQueueFamilyQueryResultStatusPropertiesKHR.queryResultStatusSupport` は FALSE。
- 実行器は `vkGetDeviceQueue2` の **family と index を読んで queue object に覚え**（今は読み捨て、§1.2）、`vkQueueSubmit` で submit の先頭の queue の id から family を引く（今は読み捨て）。
- family 1 の submit が受ける操作: video coding の 4 つに加え、規格が decode の queue に許す同期の command（`vkCmdPipelineBarrier`・`vkCmdSetEvent`・`vkCmdResetEvent`・`vkCmdWaitEvents`・`vkCmdExecuteCommands`（secondary の中も同じ規則）・`vkCmdResetQueryPool`）。これらは family 0 と同じ実行器の処理（barrier は何もしない、event は CPU 側の今の処理）。それ以外の graphics・compute・transfer の操作があれば D18 で拒む。family 0 に video の操作があっても拒む。`vkCmdWriteTimestamp` は `timestampValidBits` 0 なので app が使えない（規格）。

### 3.3 `vkGetPhysicalDeviceVideoCapabilitiesKHR`

受ける profile: `DECODE_H264`、`420`、luma・chroma 8 bit、`stdProfileIdc` ∈ {BASELINE 66, MAIN 77, HIGH 100}、`pictureLayout = PROGRESSIVE`。他は規格の error。

| field | 値 | 出典・理由 |
| --- | --- | --- |
| `flags` | `SEPARATE_REFERENCE_IMAGES` | D8 |
| `minBitstreamBufferOffsetAlignment` / `SizeAlignment` | 32 / 1 | ANV と同じ、§6.3 |
| `pictureAccessGranularity` / `minCodedExtent` | 16x16 | MB |
| `maxCodedExtent` | 4096x4096 | D20（picture ごとの MB 数は D17 で 36864 まで） |
| `maxDpbSlots` / `maxActiveReferencePictures` | 17 / 16 | DPB・PICID の 16 枠 + 現 picture |
| `stdHeaderVersion` | `VK_STD_vulkan_video_codec_h264_decode` 1.0.0 | header |
| decode の flags | `DPB_AND_OUTPUT_COINCIDE` | D8 |
| `maxLevelIdc` | 5.1 | D20 |
| `fieldOffsetGranularity` | 0,0 | progressive |

### 3.4 format と image

- `vkGetPhysicalDeviceVideoFormatPropertiesKHR`: profile の list が §3.3 だけなら 1 件 `{ NV12, identity, imageCreateFlags 0, 2D, OPTIMAL, imageUsageFlags = DECODE_DST | DECODE_DPB }`（要求との共通部分でなく対応する全体、規格の読みは p002 で確かめる）。他の usage を求められたら `VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR`（D23）。list が空か他の profile なら `VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR`。
- `vkGetPhysicalDeviceFormatProperties`（NV12）: `optimalTilingFeatures = VIDEO_DECODE_OUTPUT | VIDEO_DECODE_DPB`、linear・buffer は 0（D23）。
- `vkGetPhysicalDeviceImageFormatProperties2KHR`: pNext の `VkVideoProfileListInfoKHR` を libvulkan が照合し、NV12・2D・OPTIMAL・usage ⊆ {DST, DPB} なら backend へ。backend は `maxExtent 4096x4096x1、maxMipLevels 1、maxArrayLayers 1、sampleCounts 1、maxResourceSize`。他の usage は `VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR`。
- `vkCreateImage`: NV12・2D・1 level・1 layer・1 sample・OPTIMAL・usage ⊆ {DST, DPB}。layout は §6.4。
- `vkGetImageSubresourceLayout`: aspect `PLANE_0/1` で plane の offset・rowPitch・size を返す。OPTIMAL への答えは規格外（N3）で、zedBSD の私的な約束として README に書く（試験の app の de-tile が使う。SAMPLED・TRANSFER_SRC が入れば不要になる、HD5）。
- `vkCreateImageView`: NV12 の 2D view、aspect COLOR。

### 3.5 session と parameters

- `vkCreateVideoSessionKHR`: `queueFamilyIndex` 1、`flags` 0（`INLINE_QUERIES`・`PROTECTED_CONTENT` は `VK_ERROR_FEATURE_NOT_PRESENT`）、profile §3.3、format NV12、`maxCodedExtent` ≤ 4096x4096（16 に切り上げ）、`maxDpbSlots` ≤ 17、`maxActiveReferencePictures` ≤ 16、std header の名と版 ≤ 1.0.0（違えば `VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR`）。backend は session を作り、最初の session なら VCS0 の hardware context を作る（D11）。
- `vkGetVideoSessionMemoryRequirementsKHR`: bind index 0〜3 は row store 4 本、4〜4+maxDpbSlots−1 は slot ごとの MV buffer、4+maxDpbSlots は setup の無い picture の MV buffer（§6.4）。
- `vkBindVideoSessionMemoryKHR`: 全 bind index を bind するまで decode の submit は D18 で拒む。
- `vkCreateVideoSessionParametersKHR`・`vkUpdateVideoSessionParametersKHR`: SPS ≤ 32、PPS ≤ 256、template の写し、`updateSequenceCount` は今の +1、既にある key の追加（update）と H.264 の範囲の外の id は `VK_ERROR_INITIALIZATION_FAILED`（p002 で直し: 固定した 1.3.269 の header では `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` が `VK_ENABLE_BETA_EXTENSIONS` の中の encode の値。create は template の key を同じ key の set で置き換えてよい）。SPS・PPS は libvulkan が規格の error のために手元に写し、backend にも送る（実行器が真実、D22 で 1 件ずつ確保）。`pOffsetForRefFrame`・`pScalingLists` は deep copy、VUI は捨てる。
- 破棄: `GPU_OP_DESTROY_*`。session の destroy は bind した memory を解放しない。

### 3.6 command buffer の video coding scope（第 2 版で規格から書き直し、B1）

family 1 の pool の **primary** の command buffer に記録する（新 file `video.c`）:

- `vkCmdBeginVideoCodingKHR(pBeginInfo)`: `videoSession`、`videoSessionParameters`（decode で要る）、`pReferenceSlots[]`。各要素の意味は規格どおり:
  - `slotIndex ≥ 0` かつ `pPictureResource != NULL`: その slot に対応付けた picture の resource を scope に bind する（slot はすでに有効か、この scope の decode で有効になる）。
  - `slotIndex < 0` かつ `pPictureResource != NULL`: slot の無い resource を bind する（この scope の decode の **setup の参照 picture** の宛先に使う）。
  - `slotIndex ≥ 0` かつ `pPictureResource == NULL`: その slot を**無効にする**。
  - 記録の形は §4.2（`pPictureResource` は present の u64 で NULL を表す）。
- `vkCmdControlVideoCodingKHR(flags)`: `RESET` だけ。全 slot を無効にする。session の最初の使用の前に必須。
- `vkCmdDecodeVideoKHR(pDecodeInfo)`: `srcBuffer`・`srcBufferOffset`（32 の倍数）・`srcBufferRange`、`dstPictureResource`、`pSetupReferenceSlot`（NULL 可）、`pReferenceSlots[]`（各 pNext に `VkVideoDecodeH264DpbSlotInfoKHR.pStdReferenceInfo`、全て Begin で bind 済みの有効な slot）、pNext の `VkVideoDecodeH264PictureInfoKHR`（`pStdPictureInfo`、`sliceCount`、`pSliceOffsets[]`）。coincide なので setup がある時 `dstPictureResource` と `pSetupReferenceSlot->pPictureResource` は同じ resource。setup の slot は、`flags.is_reference` が立つ時は decode の後に有効になりその picture が入り、立たない時は decode の後に**無効**になる（版 9 の「reference にしない picture の reconstructed picture の slot は invalidate」の読み。規格の本文で確かめる、U17）。
- `vkCmdEndVideoCodingKHR`。

### 3.7 synchronization2 の翻訳（D6）

`vkCmdPipelineBarrier2KHR`・`vkCmdSetEvent2KHR`・`vkCmdResetEvent2KHR`・`vkCmdWaitEvents2KHR`・`vkCmdWriteTimestamp2KHR`・`vkQueueSubmit2KHR` の 6 つを 1.0 の command へ。stage2 の下位 32 bit に 1.0 の bit があればそれ、無い bit（`VIDEO_DECODE`、`COPY` 等）は `ALL_COMMANDS`、`NONE` は `TOP_OF_PIPE`（src）/`BOTTOM_OF_PIPE`（dst）。access2 は `MEMORY_READ`/`MEMORY_WRITE` へ。`VkSubmitInfo2` は `VkSubmitInfo` へ（timeline の値は 0）。

## 4. protocol（Kei GPU command protocol の追加）と capset

### 4.1 `include/uapi/gpu-op.h` の差分（承認まで当てない、H1）

差分は [`proposed/gpu-op-video.diff`](proposed/gpu-op-video.diff)（SHA は [proposed/README.md](proposed/README.md)）。14 opcode `0x10000`〜`0x1000d`（VIDEO_CAPABILITIES、VIDEO_FORMAT_PROPERTIES、QUEUE_FAMILY_VIDEO_PROPERTIES、CREATE/DESTROY_VIDEO_SESSION、GET_VIDEO_SESSION_MEMORY_REQUIREMENTS、BIND_VIDEO_SESSION_MEMORY、CREATE/UPDATE/DESTROY_VIDEO_SESSION_PARAMETERS、CMD_BEGIN/END/CONTROL_VIDEO_CODING、CMD_DECODE_VIDEO）と `GPU_OP_PROTOCOL_VERSION` 2（版 1 の番号は不変、版の注を足す）。

- capset の wire version（byte 0、今 1）は Venus の約束なので変えない。
- video の番号の範囲の上限（`I915_VK_VIDEO_OP_LAST`）は**実行器の中**（`render/video.h`）に置き、UAPI に足さない。
- 他の UAPI（`gpu.h` など）は変えない。

### 4.2 record の形（wire）

既存の framing（`[opcode][reply flag][...]`、object は u64 の id、pointer は「present」の u64、配列は u64 の数、構造体は `sType u32`・`pNext` 連鎖、知っている sType だけを書く）に揃え、`maintain-codec.noct` の表で生成する。

- pNext の連鎖: `VkVideoProfileInfoKHR` → `VkVideoDecodeH264ProfileInfoKHR`・`VkVideoDecodeUsageInfoKHR`、`VkVideoCapabilitiesKHR` → `VkVideoDecodeCapabilitiesKHR`・`VkVideoDecodeH264CapabilitiesKHR`、parameters の create → `VkVideoDecodeH264SessionParametersCreateInfoKHR`、update → `VkVideoDecodeH264SessionParametersAddInfoKHR`、`VkVideoDecodeInfoKHR` → `VkVideoDecodeH264PictureInfoKHR`、`VkVideoReferenceSlotInfoKHR` → `VkVideoDecodeH264DpbSlotInfoKHR`、`VkPhysicalDeviceVideoFormatInfoKHR` → `VkVideoProfileListInfoKHR`。
- `VkVideoReferenceSlotInfoKHR`: `[sType][pNext…][slotIndex i32][present][VkVideoPictureResourceInfoKHR]`（present 0 が NULL = slot の無効化、§3.6）。
- 構造体の中の数の field は宣言の位置に u32 で書き、配列の pointer は `[u64 数]{要素}`（core の codec と同じ）。値で持つ構造体（`dstPictureResource`）は sType から全部。符号付きの値（`int8_t`・`int32_t`）は符号を広げて u32。
- `StdVideo*` は field を宣言順に（bit field の flags は u32、宣言の最初の flag が bit 0。reserved の field も書く。`PicOrderCnt[2]` は `[u64 2]{u32 ×2}`）。SPS の `pOffsetForRefFrame` は `num_ref_frames_in_pic_order_cnt_cycle` 個（255 まで）、`pScalingLists` は present + 本体（`StdVideoH264ScalingLists`: 2 つの mask を u32 で、4x4 の 6 本×16 を `[u64 96]` + 96 byte、8x8 の 6 本×64 を `[u64 384]` + 384 byte、全て scan 順）。pOffsetForRefFrame は `[u64 数]{u32}`（NULL は数 0）、VUI は present 0。

| op | 送る物 | 返事 |
| --- | --- | --- |
| VIDEO_CAPABILITIES | `[physical][present][profile+chain][present][caps の形]`（形 = `[sType][link]`、link は `0` か `1` + 次の形。decode・H.264 の caps だけ） | `[result][present][caps+chain]`（chain は入れ子: caps の sType、link 1、decode の sType、link 1、H.264 の sType、link 0、H.264 の field、decode の field、caps の field） |
| VIDEO_FORMAT_PROPERTIES | `[physical][present][format info+chain][present 1][u32 32][u64 32]`（libvulkan は常に 32 個まで一度に聞き、手元で切り詰めて `VK_INCOMPLETE`） | `[result][present][u32 count][u64 count]{[sType][link 0][field]}`（count ≤ 32） |
| QUEUE_FAMILY_VIDEO_PROPERTIES | `[physical][u32 family の数]` | `[present][u32 family の数]{u32 codec ops}` |
| CREATE_VIDEO_SESSION | `[device][present][create info+chain][allocator 0][present][id]` | `[result][present][id]` |
| DESTROY_VIDEO_SESSION | `[device][session][allocator 0]` | なし |
| GET_VIDEO_SESSION_MEMORY_REQUIREMENTS | `[device][session][present 1][u32 32][u64 32]` | `[result][present][u32 count][u64 count]{[sType][link 0][bind index][VkMemoryRequirements]}`（count ≤ 32） |
| BIND_VIDEO_SESSION_MEMORY | `[device][session][u32 count][u64 count]{VkBindVideoSessionMemoryInfoKHR}` | `[result]` |
| CREATE/UPDATE/DESTROY_VIDEO_SESSION_PARAMETERS | create `[device][present][create+chain][allocator 0][present][id]`、update `[device][params][present][update+chain]`、destroy `[device][params][allocator 0]` | create・update `[result]`（create は `[present][id]` も） |
| CMD_BEGIN/END/CONTROL_VIDEO_CODING、CMD_DECODE_VIDEO | `[cmdbuf][present][struct+chain]` | なし（記録） |

### 4.3 capset の native の語（HD1、第 2 版で変更、B4）

[proposed/README.md](proposed/README.md) の「capset の native の語」のとおり。native の i915 だけが 176 byte（byte 168 tag `0x5a4e4154`、byte 172 の bit 0 = `VIDEO_DECODE_H264`）。libvulkan は vendor 部を `bytes == 168 || bytes == 176` で読み（flags の完全一致の判定は不変）、`bytes == 176` かつ tag が合う時だけ native の bit を読み、`context->video_h264` にする。fork と stock の Venus は 168 以下なので影響しない。実行器が bit を立てる条件は D19。

- **順序**: 古い libvulkan は 176 byte を受けると vendor 部を読まず（`context.c` 170）OPAQUE・strict_queue・quiesce を失い compositor が退行する。i915 の `render/vulkan.c` の `i915_render_capset_fill`（271〜297）を 176 byte にするのは、libvulkan の変更（p002）が main に入った**後**の p003b で、**native の bit が立つ時だけ** 176 byte にする（D19 の門が閉じていれば今の 168 byte のまま）。`plan/ws031/tests/i915-vk-cmd-test.c` 733・744 の 168 の assert は p003b で両方の場合に直す。
- **timeline**（第 3.1 版で直し）: libvulkan は queue ごとに timeline を予約し（`device.c` 99〜128・587〜619）、kernel は timeline 0〜2 を engine record に写す（`session.c` 60〜95、他は EINVAL）。fence の marker は timeline の engine record の queue に積まれ、その record の context で走る（`command.c` 486〜512、`worker.c` 1280）。実行は同期なので timeline は順序にしか効かない。だから **全ての queue（video の queue も）の timeline を RCS0 の record に写す**: kernel の `drv_i915_engine_for_timeline` は「0 → RCS0（今のまま）、1・2 → RCS0（今のまま）、3 以上 → RCS0」とし（2 つ目の device・2 つ目の queue の EINVAL を無くす）、marker が VCS0 で走ることは無い（遅延の VCS0 の context の前の fence、hang した VCS0 への marker の積み込みを避ける）。engine を選ぶのは実行器の submit（queue の family、§3.2）だけ。capset byte 152 は変えない（libvulkan は 0 でないことしか見ない: `context.c` 153〜159、上限は `VULKAN_QUEUE_TIMELINE_COUNT` 64）。
- **family 1 の出し方**: 実行器は native の bit が立つ時だけ family 1 を返す（D19 の門が閉じていれば family は 1 つ）。D3 の濾しで `queueFlags == 0` の family が見えることは無い。

## 5. libvulkan の構造

| 物 | 変更 |
| --- | --- |
| `include/libc/vulkan/vulkan_video.h`（新） | `tools/maintain-video.noct`（新）が D14 の 1.4.309 の header から 3 拡張と sync2 の宣言を選ぶ。`vulkan.h` が include。先頭で `vk_video/vulkan_video_codec_h264std_decode.h` を include |
| `include/libc/vulkan/vk_video/`（新、3 file） | 1.4.309 の package から写す（Apache-2.0、`API-PROVENANCE.md`・`LICENSE-API` に版・path・SHA） |
| `internal.h` | object kind 2 つ、拡張の bit 4 つ、`struct vulkan_context` に `video_h264`、physical に family ごとの codec ops |
| `video.c`（新） | §3.3〜3.6 の 13 entry point、session・parameters の object |
| `sync2.c`（新） | §3.7 の 6 entry point |
| `context.c` | capset の native の語（§4.3） |
| `instance.c`・`device.c`・`external-properties.c` | 拡張の列挙と照合、D3 の濾し、pNext、format の feature（D23） |
| `codec.c/h`・`commands-generated.inc`・`dispatch-table.inc`・`api-commands.tsv` | 生成し直し（173 → 192） |
| `Makefile` | `video.c`・`sync2.c`。export の数の検査の所在は p002 で確かめる（U13） |
| `README.md`・`API-PROVENANCE.md` | 非適合 N1〜N3、header の来歴、私的な約束 |

object の寿命は既存の device の子の形。command の記録は byte 列で、app の pointer を借りない。

## 6. i915 の側

### 6.1 engine record と VCS0 の context・submission（第 2 版で直し、S1・S6・S17）

| 物 | 変更（file） |
| --- | --- |
| engine record | `request-queue.h`: `I915_ENGINE_VCS0 2U`、`I915_ENGINE_COUNT 3U`。`i915.c` 143〜160 の loop を index ごとの class の選択に直す（0 RENDER、1 COPY、2 **VIDEO**。今は「0 以外は COPY」）。GT に VCS0 が無い device では index 2 の record の `initialized = 0`、native の bit を立てない。`reset.c` の 3 つの loop は全 record の request を落とす（VCS0 も同じ処理でよい）。`session.c` 163・284 の loop は VCS0 を record だけで作る（下）。`job.c`・`command.c` は RCS0・BCS0 を名で選ぶので変えない |
| session の context | open は VCS0 を record だけ（`created = 1`、hardware context なし、今の 597〜605 の形）にする。実行器が最初の `vkCreateVideoSessionKHR` で `drv_i915_worker_context_attach(device, &session->contexts[I915_ENGINE_VCS0])`（新）を呼ぶ。attach は **device の mutex の下**で「まだ無ければ作る」（同じ session の 2 つの video session が同時に来ても 1 本）。close は今の `drv_i915_worker_context_destroy` が両方の表を探して解放する |
| worker | `struct i915_worker` に `video_index`（GT の engine の VCS0、無ければ −1）と `video_contexts[I915_WORKER_VIDEO_CONTEXTS = 8]`。`i915_worker_find` は context の engine の index で表を選ぶ（render の表と video の表）。`i915_worker_run` の「RCS0 以外は ENOTSUP」（1388〜1394）を「record の engine に hardware context があれば走る」に、`drv_i915_execlists_submit` と待ち（1441〜1442・1539〜1540）に **その engine** の `ge`・`el` を渡す。LRC は `context.c` が class で xcs の offsets を選ぶ。ring は 16 KiB |
| request・batch | `request.c` はそのまま（xcs の flush と `MI_INVALIDATE_BSD`）。`MI_BATCH_BUFFER_START` も同じ |
| 割込み | `irq.c` は変えない |
| forcewake・電源 | service は 5 domain を持ち続ける。batch の先頭で `MI_FORCE_WAKEUP` と `MFX_WAIT`（ANV の Gen12 と同じ、§6.3） |
| firmware | HuC は要らない見込み（U2、p005 で確かめる） |
| **hang の封じ込め**（S6、第 3・3.1 版で直し） | VCS0 の decode が timeout（ETIMEDOUT）または CSB の error（EIO、`worker.c` 1563〜1573）で終わったら: その submit は `VK_ERROR_DEVICE_LOST`、device の **video を死んだ印**にし以後の video の submit を全部 `VK_ERROR_DEVICE_LOST`（render は続ける）。その session を既存の **quarantine**（`reset.c` 368〜397 の `i915_isolate` と同じ印 `session->quarantined = 1`）にし、vm（PPGTT の table）と object（app の VkDeviceMemory を含む）は checked reset まで残る（`session.c` 236〜240・`memory.c` 503〜522 の既存の扱い）。今の close は quarantine の時 batch pool は残す（`session.c` 303〜306）が context は解放する（226〜233）ので、video の死んだ印の時は **VCS0 の context の record（LRC・ring・timeline page）を owner から切り離して「保持中」にし**（探索・再利用・解放の対象から外す。session は close の最後に `kern_free` されるので、owner の pointer を残すと同じ address の新しい session に一致する: `worker.c` 1685、`session.c` 248）、log を出す。`live_contexts` は 0 に戻らないので `drv_i915_worker_destroy` は worker を残す（`worker.c` 559〜563）。video の decode の batch の持ち主は **video session**（実行器の video の object）とし、`vkDestroyVideoSessionKHR` と `drv_i915_render_close`（`draw.c` 128〜158 は quarantine を見ない）でも、video が死んでいれば解放せず保留の list に移す。video が死んでいる間は attach と `vkCreateVideoSessionKHR` を拒む（`VK_ERROR_INITIALIZATION_FAILED`）。quarantine はその session の新しい resource・job を拒む（`resource.c` 110・310、`command.c` 419・496）ので「render は続ける」のは**他の session**だけ。checked reset は未実装（`worker.c` 1052〜1060 は ENOTSUP）なので、死んだ印と保持した物は再起動まで残る。p007 の engine reset で `i915_quarantine_release` に保留の list の解放を足す。engine 単位の reset（`GRDOM_MEDIA`。dmesg の reset_domain 0x8 は `device-info.c` の誤りで、正しくは 0x20。ws083-p007 で直した）は HD2 |
| display | `display/` は触らない（別の担当が変更中） |
| device-info | 変えない |

### 6.2 実行器の video の module（`render/video.c`・`video.h`、新）

- object kind `I915_VK_OBJ_VIDEO_SESSION`・`I915_VK_OBJ_VIDEO_SESSION_PARAMETERS`。`dispatch.c` の `i915_dispatch_route` に `GPU_OP_OWN_FIRST..I915_VK_VIDEO_OP_LAST` → video。
- `struct i915_video_session`: profile、`max_coded`、`max_dpb_slots`、`max_refs`、bind の表と `bound_mask`、scratch の大きさ、DPB の slot 表 `slots[17]`（`{image, active}`）、`reset_done`。
- `struct i915_video_params`: SPS・PPS の id → pointer の表（D22）、`update_sequence`。
- 記録: `enum i915_gfx_op_kind`（`gfx.h` 87）に `VIDEO_BEGIN`・`VIDEO_CONTROL`・`VIDEO_DECODE`・`VIDEO_END`。slice offset の配列は 256 まで記録し、超える picture は §6.6 の 9 で飛ばす。
- dispatch: `dispatch.c` の route で video の module へ送るのは `GPU_OP_OWN_FIRST..+9`（物理の問い合わせ・session・parameters）だけ。`+10..+13`（CMD_*）は `drv_i915_gfx_rec_dispatch` が受けるよう、その範囲の検べ（`render/command.c` 284〜288、今は `opcode > GPU_OP_CMD_EXECUTE_COMMANDS` で `handled = 0`）に `0x1000a`〜`0x1000d` を足す（route の COMMAND_BUFFER は `i915_dispatch_unported` で拒む: `dispatch.c` 101〜103）。
- video の context の表（8）が尽きたら `vkCreateVideoSessionKHR` は `VK_ERROR_OUT_OF_DEVICE_MEMORY`。parameters の update の途中で確保が失敗したら、その update で足した物を全部外して元に戻す（rollback）。wire の `StdVideo*` の bit field の flags は名前で 1 つずつ詰め・読む（memcpy にしない）。
- submit: queue の family（§3.2）が 1 なら `drv_i915_video_submit`（新）。**video 専用の batch と cursor**（session の render の `work` の batch とは別、`drv_i915_gfx_flush` は RCS0 に固定なので使わない: `draw.c` 406〜437・426）に §6.3 を書き、engine を引数に取る flush で `drv_i915_worker_run_sync(device, &contexts[I915_ENGINE_VCS0], batch_va)`。1 decode が 1 op（固定部約 328 dword（最後の `MI_FLUSH_DW` と BBE を含む） + slice ごと 11 dword、256 slice で約 3140 dword、`I915_GFX_OP_MAX_DWORDS` 4096 の中）。batch に次の decode の余地が無ければ flush して続ける（1 MiB を超える command buffer を失敗させない）。既存の溢れの error は ENOSPC（`draw.c` 372〜374）で `i915_command_result` では INITIALIZATION_FAILED になるので、video の submit は自分で `VK_ERROR_OUT_OF_DEVICE_MEMORY`／`DEVICE_LOST` に写す（D18）。

### 6.3 MFX AVC VLD の command 列と `StdVideo*` からの写像

命令の header（Command Type 3 << 29 | Pipeline 2 << 27 | Opcode << 24 | SubOpcode A << 21 | SubOpcode B << 16 | DWord Length = 総 dword − 2）と長さは genxml の既定値。`intel/genxml-video.h` に転記する。

| 順 | 命令 | 長さ | Opcode/SubA/SubB | 主な field と出どころ |
| --- | --- | --- | --- | --- |
| 1 | `MI_FLUSH_DW` | 5 | MI 0x26 | Video Pipeline Cache Invalidate |
| 2 | `MI_FORCE_WAKEUP` | 2 | MI 0x1d | MFX Power Well Control 1、Mask Bits 0x300（ANV の 768） |
| 3 | `MFX_WAIT` | 1 | Command Type 3、**Command Subtype 1**（`gen75.xml` 2019）、SubOpcode 0、MFX Sync Control Flag 1 | PIPE_MODE_SELECT の前後に 2 回 |
| 4 | `MFX_PIPE_MODE_SELECT` | 5 | 0/0/0 | AVC、Decode、Short Format、VLD、Post Deblocking Output 1、他 0 |
| 5 | `MFX_SURFACE_STATE` | 6 | 0/0/1 | Surface ID 0、Width/Height = 宛先の image の extent −1、Tile Walk YMAJOR、Tiled 1、Pitch −1、Interleave Chroma 1、PLANAR_420_8、**Y Offset for U(Cb) と Y Offset for V(Cr) の両方** = Y plane の行数（§6.4） |
| 6 | `MFX_PIPE_BUF_ADDR_STATE` | 65 | 0/0/2 | Post Deblocking Destination = 宛先、Intra Row Store = bind 0、Deblocking Filter Row Store = bind 1、Reference Picture[i] = `pReferenceSlots[i]` の image（参照の並び i）、**使わない i は宛先の address**（D21）、MOCS は実行器の surface と同じ |
| 7 | `MFX_IND_OBJ_BASE_ADDR_STATE` | 26 | 0/0/3 | `va = srcBuffer の GPU address + srcBufferOffset`、bitstream の base = `va & ~4095`、Upper Bound = buffer の bind の範囲の終わり（buffer の VA の整列に依らない形） |
| 8 | `MFX_BSP_BUF_BASE_ADDR_STATE` | 10 | 0/0/4 | BSD/MPC Row Store = bind 2、MPR Row Store = bind 3 |
| 9 | `MFD_AVC_DPB_STATE` | 27 | 1/1/6 | 参照の並び i の `StdVideoDecodeH264ReferenceInfo`: Non-Existing、Long Term、Used for Reference（top・bottom が共に 0 なら 3）、FrameNum。ANV の事実（§1.5）と同じく **i で**埋める |
| 10 | `MFD_AVC_PICID_STATE` | 10 | 1/1/5 | Picture ID[i] = `pReferenceSlots[i].slotIndex`、残り 0xffff |
| 11 | `MFX_AVC_IMG_STATE` | 21 | 1/0/0 | 第 1 版の表のとおり（Frame Width = `pic_width_in_mbs_minus1`、Frame Height = 高さの MB −1、**Frame Size = 総 MB（≤ 36864、D20・D17）**、Image Structure Frame、PPS・SPS の flag、Number of Reference Frames = `referenceSlotCount`、Current Picture Frame Number = `frame_num` ほか） |
| 12 | `MFX_QM_STATE` ×2（+2） | 18 | 0/0/7 | 4x4 Intra（list 0〜2）、4x4 Inter（3〜5）、transform_8x8 なら 8x8 Intra（8x8 の list 0）・Inter（list 1）。**`StdVideoH264ScalingLists` は scan 順、hardware は raster 順**: `Forward[m*16 + zigzag4[q]] = list4[m][q]`、`Forward[zigzag8[q]] = list8[k][q]`（B2）。導出（fall-back）は §6.5 |
| 13 | `MFX_AVC_DIRECTMODE_STATE` | 71 | 1/0/2 | Direct MV Buffer[i] = 参照 i の slot の MV buffer、使わない i は書き込みの MV buffer（D21）、Write = setup の slot の MV buffer（setup が無いか `is_reference` が 0 なら予備の bind）、POC List[2i]・[2i+1] = 参照 i の `PicOrderCnt`、[32]・[33] = 現 picture |
| 14 | slice ごと: `MFD_AVC_SLICEADDR`（最後以外）と `MFD_AVC_BSD_OBJECT` | 4 / 7 | 1/1/7、1/1/8 | `skew = va − base`（7 の va と base）、start = skew + slice の NAL の先頭（§6.6 の 8 で start code の後ろ）、length = 次の offset（最後は `srcBufferRange`）− その先頭。SLICEADDR は次の slice。BSD_OBJECT の Last Slice・concealment の bit |
| 15 | `MI_FLUSH_DW` + `MI_BATCH_BUFFER_END` | 5 + 1 | | |

start code は §6.6 の 8 で offset から 4 byte 以内の `00 00 01` を探してその後ろから読む（3 byte・4 byte のどちらも受ける。見つからなければ飛ばす）。

### 6.4 memory の形と大きさ

| 物 | 置き場所と形 | 大きさ・整列 |
| --- | --- | --- |
| bitstream | app の `VkBuffer` | offset 32 の倍数、base は 4 KiB 整列、upper bound は bind の範囲の終わり |
| 出力・DPB の image（NV12、Tile Y） | `struct i915_gfx_image` に `planar`・`tiled`・`chroma_offset`・`chroma_rows` | `pitch = align(width, 128)`、Y plane = `pitch * align(height, 32)`、UV = `pitch * align(height/2, 32)`、`chroma_offset` = Y plane、全体を 4 KiB に。Y Offset for U・V = `align(height, 32)`。width・height は 16 に切り上げ |
| Tile Y の中身 | 128 B × 32 行 = 4 KiB の tile、中は 16 B 幅の列ごとに 32 行（swizzle なし） | |
| row store 4 本 | bind 0〜3 | §1.5 の大きさ、4 KiB 整列 |
| direct MV buffer | bind 4〜4+maxDpbSlots−1 と予備 1 本 | `w_mb * h_mb * 128`、4 KiB 整列（U5） |

### 6.5 session の状態と DPB（第 2 版で書き直し、B1）

- session: `created` → `bound` → `reset`（`RESET` を含む submit が走った）→ decode 可。`reset` 前の decode は D18。
- slot 表は実行器が submit の中で持つ（`slots[i] = {image, active}`）。
  - Control `RESET`: 全 slot を `active = 0`。
  - Begin: `pReferenceSlots[]` を scope の bind の一覧として覚える。`slotIndex ≥ 0` かつ resource が NULL の要素はその slot を `active = 0`。`slotIndex ≥ 0` の resource は、その slot が active ならその image と同じでなければ D18。
  - Decode: `pReferenceSlots[i]` は全て Begin で bind され active な slot（でなければ D18）。setup がある時、その resource は Begin で bind 済みで `dstPictureResource` と同じ（D18）。decode の後、`flags.is_reference` が立てば `slots[setup.slotIndex] = {image, active 1}`、立たなければ `slots[setup.slotIndex].active = 0`（版 9、U17）。
  - **slot の遷移は GPU を走らせる前に submit 全体で模擬して検べる**。模擬は submit の中の順序で状態を進める（同じ submit の中の「RESET → decode」を受ける: `reset` は「その submit の模擬の中で RESET を過ぎた」で真）。模擬で D18 にする物に、`pReferenceSlots` の `slotIndex` の重複と、bind した memory の storage が外れている（`memory->object == NULL`、`render/memory.c` 191〜208 の blob の detach の後）を足す（D18 の拒否は submit の最初に全部決め、一部だけ走ってから拒む形にしない）。D17 で飛ばす decode も slot の遷移（setup・`is_reference`）は行う（次の正しい decode の参照が D18 にかからないように）。
  - Begin で inactive の slot を `slotIndex ≥ 0` の resource で bind する形は受けるが、規格で許されるかは未確認なので試験で「正しい形」として固定しない（FFmpeg は setup を −1 で bind する）。
  - 各 decode の MFX の表（DPB_STATE・PICID・DIRECTMODE）はその decode の `pReferenceSlots[]` から組む。
- **scaling list**: SPS・PPS の `scaling_list_present_mask`・`use_default_scaling_matrix_mask` から規格 7.4.2.1.1.1・7.4.2.2 の fall-back 規則 A・B で 4x4 の 6 本・8x8 の 2 本（4:2:0）を導く（PPS が `pic_scaling_matrix_present_flag` なら PPS、でなければ SPS、どちらも無ければ Flat_16）。Default_4x4/8x8 と zig-zag の表（Table 7-3・7-4・8-12・8-13）は `render/video-h264-tables.c`（新、`static const`、出典の表番号）に置く（`.inc` は使わない）。導出の結果は scan 順で、§6.3 の 12 で raster 順に並べ替える。
- 並行性: object 表は session の mutex、submit は device の worker で直列。decode と描画は互いに待つ（U8）。
- memory の寿命: 今の buffer・image は bind した `VkDeviceMemory` の record を生の pointer で持ち、`vkFreeMemory` の後も残る（`render/memory.c` 657・687 の bind_image・bind_buffer、free は 397〜411、XXX の注記は 231〜235。既存の欠陥、Bug ticket にする）。video は session の memory の bind を **memory の identity** で持ち、submit の時に object 表で引き直す（無ければ D18）。srcBuffer・宛先・参照も submit の時に引き直した memory で address を作る。

### 6.6 kernel の検べ（D17、B3）

`drv_i915_video_submit` は各 decode の batch を書く前に次を検べ、一つでも合わなければ**その decode を飛ばす**（MFX を書かない、log `i915: video: skip …`、submit は続ける）:

1. `pic_parameter_set_id` の PPS と、その `seq_parameter_set_id` の SPS が parameters にある。
2. SPS: `chroma_format_idc == 1`、`bit_depth_luma_minus8 == 0`・`chroma == 0`、`frame_mbs_only_flag == 1`、`pic_order_cnt_type ≤ 2`、`log2_max_frame_num_minus4 ≤ 12`、`log2_max_pic_order_cnt_lsb_minus4 ≤ 12`。
3. MB の幅・高さ（`pic_width_in_mbs_minus1 + 1`、`pic_height_in_map_units_minus1 + 1`）が session の `maxCodedExtent` の MB 以下、宛先と各参照の image の extent の MB 以下、総 MB ≤ 36864。
4. PPS: `num_ref_idx_l0/l1_default_active_minus1 ≤ 31`、`weighted_bipred_idc ≤ 2`、`pic_init_qp_minus26` が −26〜25、chroma の QP offset が −12〜12。
5. slice: `sliceCount` 1 以上、`pSliceOffsets` が厳密に増え、各 offset + 4 ≤ `srcBufferRange`、`srcBufferOffset + srcBufferRange` が buffer の bind の範囲の中（start code の検べは 8）。
6. 全ての参照の image の pitch・chroma の行（Y Offset for U/V）・format・tiling が宛先と同じ（MFX は宛先の `MFX_SURFACE_STATE` で参照も読むので、小さい参照は範囲の外を読む。ANV `genX_cmd_video.c` 927〜939 も宛先だけを書く）。
7. 宛先・参照・row store・MV buffer の GPU address が 4 KiB 整列（bind の offset は今 整列を検べない: `memory.c` の `i915_gfx_bind_buffer`・`i915_gfx_bind_image`）、`codedOffset == 0`、`baseArrayLayer == 0`。
8. slice の長さ: 「次の offset − offset ≥ 4」を明示に（u32 の underflow を防ぐ）。start code は offset から 4 byte 以内で `00 00 01` を探し、その後ろを slice の先頭にする（4 byte の start code `00 00 00 01` も受ける）。
9. `sliceCount` が 256 を超える picture は飛ばす（1 op は `I915_GFX_OP_MAX_DWORDS` 4096 の中、`heap.h` 110）。規格に合わない点 N4 として README に書く。

値の違反（1〜9、6 の参照の layout を含む）は picture を飛ばす。`referenceSlotCount ≤ 16`、`slotIndex` が `0..maxDpbSlots−1`、parameters が Begin の session の物であることは app の valid usage なので D18（submit の前の模擬で拒む）。

app の valid usage の違反（§6.5 の D18）は submit を拒む（libvulkan はこれを context 全体の error にする: `sync.c` 456〜470 の `context->error`。browser の GPU context ごと止まるので、bitstream の値の違反は必ず「飛ばす」側に置く）。値の違反は picture を飛ばす（規格は不正な bitstream の結果を未定義とする）。

### 6.7 既存の枠組みとの関係

- drv_gpu・cdev・ioctl・HAL・PCI・display・Venus の driver: 変えない。
- i915 の `tests/`（kernel 内の scenario）: p005 で VCS の scenario（空の batch・store）を `tests/execution/` に。

## 7. ライセンスと転記

- 新しい code は Zlib、`coding-style.md` §13 の header。
- `intel/genxml-video.h`: Mesa の genxml（MIT の data）から MFX の命令の opcode・長さ・field の bit 位置・enum の値を転記（`intel/genxml.h` と同じ notice、出典と SHA-256: `gen110.xml` `6598e556…`、`gen90.xml` `d86fb566…`、`gen80.xml` `2962677c…`、`gen75.xml` `a5688679…`、`gen120.xml` `e2452c7d…`）。`plan/ws031/i915-vk-license-audit.md` に行を足す。命令列の組み立て・写像・DPB の論理は新規（H4）。
- Khronos の header（`vk_video/*.h`、`vulkan_video.h` の選択元）: Apache-2.0。`API-PROVENANCE.md` に package・path・SHA（H3）。
- H.264 の規格の表: ITU-T H.264 の値、`video-h264-tables.c` に表番号。
- genxml の XML を読む試験の decoder（D24）は XML を実行時に data として読み、tree に写さない。
- 試験の stream（D16）: 合成の絵から host の ffmpeg（libx264）で作る。出力の bitstream は我々の物で encoder の license は及ばない。小さい物だけ tree に（HD4）。

## 8. 試験計画

### 8.1 host

| 試験 | 中身 | 判定 |
| --- | --- | --- |
| libvulkan の video の record（`plan/ws083/tests/host-libvulkan-video.c`、p002） | libvulkan の `video.c`・`sync2.c` ほかを host で compile、transport を stub | §4.2 の byte 列、返事の decode、sync2 の写像、**Begin の 3 つの形（slot あり、`slotIndex −1` の setup、resource NULL の無効化）と Control RESET の記録** |
| 往復（`host-video-roundtrip.c`、p003） | stub の byte 列を実行器の host fixture に | 同じ struct、scratch の大きさ、slot 表の遷移（RESET → setup で有効 → NULL で無効、`is_reference` 0 の setup は無効に、飛ばした decode も遷移する、submit の前の模擬で一部だけ走らない）、D18 の拒否、§3.2 の family の規則 |
| D17 の検べ（p003〜p004） | §6.6 の各項の境界（範囲外の 1 つずつ） | 飛ばす（log）／通す |
| MFX の command stream（`host-mfx-avc.c`、p004・p006） | stand-in の worker で batch の dword 列を取る | genxml の XML の独立の decoder（D24）で field に戻し、`StdVideo*` の入力からの期待と一致。**非対称の scaling list（全ての係数が違う値）で raster への並べ替え**、PICID の 0xffff、SLICEADDR の先読み、D21 の address |
| NV12 の layout | 16x16・1920x1080・4096x4096 | pitch・plane・大きさ |
| VCS の立ち上げの host（p003） | engine record 3 つ、`i915.c` の class、session の VCS0 の record、attach の 1 本化、`i915_worker_find` の 2 表、`i915_worker_run` の engine の選択、hang の印 | compile（kernel の flag、`-Werror`）と fixture の PASS |
| build | `make -j16`（i915 の config、warning 0）、GPU の無い config で symbol 0 | |

### 8.2 QEMU（T1、p004 の後に 1 回）

Venus の経路の回帰だけ: boot-test、compositor の起動、`vkdemo`。`vkvideo-probe --list` が family 1 無し・video の拡張 0 を印字（sync2 も出ない: native の語が無い）。

### 8.3 実機（T1、5330、p005・p006）

| 段 | 試験 | 判定 |
| --- | --- | --- |
| VCS の bring-up | kernel の scenario: VCS0 の context、空の batch と `MI_STORE_DWORD_IMM` | breadcrumb、store の値、割込み、`engine_dump` に error 無し |
| I frame | `vkvideo-probe`（p004 で host で作る、§9）、`i915.debug=video` の boot。tests/streams の I frame を decode、de-tile、crop、SHA-256 | 参照の hash と一致（Baseline・Main・High） |
| P・B と DPB | 全 frame | 全 frame の hash が一致 |
| 性能 | 1080p の decode の時間 | 記録 |

全て `flock /tmp/i915-hw.lock` の下、T1 に依頼。HD3 が許せば、5330 の Linux（host 側）で ANV（`ANV_VIDEO_DECODE=1`）に同じ stream を decode させ、`INTEL_DEBUG=bat` の MFX の field と比べる（p005 の補い）。

### 8.4 試験の stream（HD4 の既定）

`plan/ws083/tests/make-streams.sh`（新）: ffmpeg の `testsrc`・`mandelbrot` から libx264 で Baseline・Main・High、64x64・352x288（各 5〜15 frame）、`-bf 2`（Main・High だけ、Baseline に B は無い）、`slices=4`、非対称の cqm（`-x264-params cqmfile=…`、script が作る。x264 の cqm は High だけで効くので非対称の scaling list は High の stream だけで試す）、`-fps_mode passthrough`（frame の順を変えない）、`h264_mp4toannexb`。出力は `plan/ws083/tests/streams/`（合計 200 KiB 以下、commit する）、参照は `ffmpeg -i … -f rawvideo -pix_fmt nv12` の frame ごとの SHA-256 を `streams/*.sha256`（POC 順 = 表示順、`vkvideo-probe` も表示順で印字）。x264 が出さない形（long term、frame_num の gap、4 byte の start code）は host の golden（合成の `StdVideo*`）だけで確かめ、実機は HD4 の判断（ITU-T H.264.1 の conformance の stream を使うか）。1080p の性能の stream は 30 frame でも大きいので build/tmp で作り、image に入れる方法は p008 で決める。

## 9. Phase の分け方と受け入れ（第 2 版で依存を直し、S10・S11）

| Phase | 内容 | 受け入れ | 依存 |
| --- | --- | --- | --- |
| ws083-p001 | この設計、design-reviewer、人の判断の提示 | review の反映、Q1 の ACK | — |
| ws083-p002 | libvulkan の骨組み（§5 の全部）、host の試験 §8.1 の 1 行目 | host の試験 PASS、build warning 0、export の数、PROVENANCE（T1 の §8.2 は p004 の受け入れ） | p001、H1・H2・H3・HD1・HD6 |
| ws083-p003a | i915 の VCS の土台（wire は要らない。hang の封じ込めは HD2 (a) の既定の形で作り、判断が (b) なら p007 を先にする）: engine record VCS0 と `i915.c` の class、worker の engine ごとの context 表・`context_attach`・`i915_worker_run` の engine の選択、hang の封じ込め（quarantine と VCS の context・batch の保持）、engine を引数に取る flush。VCS の host の試験 | §8.1 の 6 行目 PASS、vmunix の build warning 0 | p001 |
| ws083-p003b | video の object（§6.2・§6.5 の slot 表・§6.6 の検べ）、capset の 176 byte と timeline（§4.3、native の bit の時だけ）、family 1、`vkGetDeviceQueue2` の family。往復の host の試験 | §8.1 の 2・3 行目 PASS、vmunix の build warning 0 | p002（main に入った後）、p003a、H1・HD1 |
| ws083-p004 | MFX AVC の I frame の builder（`intel/genxml-video.h`、NV12 Tile Y、§6.3）、`genxml-decode.py`、試験の stream と `make-streams.sh`、`userland/tests/vkvideo-probe/`（host で build、§8.2 の `--list` もここで作る） | §8.1 の 4・5 行目 PASS（I frame の 2 本以上）、build warning 0、T1 の §8.2（QEMU の回帰） | p002（`vulkan_video.h`）、p003b、H4・H5・HD4 |
| ws083-p005 | 実機: VCS の bring-up、I frame の hash（`i915.debug=video`）、HuC 不要の確認 | §8.3 の 1・2 行目 PASS（T1） | p004、5330 |
| ws083-p006a | P・B と DPB、scaling list の fall-back、複数 slice の host の golden（実機は要らない） | §8.1（P・B）PASS | p004 |
| ws083-p006b | P・B と DPB の実機の hash | §8.3 の 3 行目 PASS（T1） | p005、p006a |
| ws083-p007 | `GRDOM_MEDIA` の engine 単位の reset（HD2 (a) の前提）と VCS の hang の回復 | host の試験、実機で人工の hang からの回復（T1） | p005 |
| ws083-p008 | 性能、D19 の既定化（p007 の後）、利用者への案内、SAMPLED・TRANSFER_SRC（HD5）、result status query | 記録、ws.md の制限 | p006b、p007 |
| ws083-p009 | 全文規約の見直しと回帰 | 規約の照合、`make -j16`、boot-test、実機の回帰 1 回 | 全 Phase |

p003a は人の判断も wire も要らないので今から始められる。§8.2 の T1 の QEMU の回帰は p004（`vkvideo-probe` を作る）の後に回す。

## 10. 人の判断（Q1 経由でユーザーへ）

| ID | 問い | 既定の案 |
| --- | --- | --- |
| H1 | `proposed/gpu-op-video.diff`（gpu-op.h に zedBSD 独自の 14 opcode、版 2）の承認 | 承認 |
| H2 | Vulkan 1.0 のまま video の拡張を名乗る形: (a) sync2 を libvulkan の翻訳で足し、規格に合わない点（N1〜N3）を記録、(b) sync2 も足さない、(c) 1.1 に上げる（別 WS） | (a) |
| H3 | Khronos Vulkan-Headers 1.4.309（Debian `libvulkan-dev` の file、disk にある）から video の宣言を選び、`vk_video/` 3 file（Apache-2.0）を tree に入れる。core の 1.3.269 の選択とは別 file | 可 |
| H4 | Mesa の genxml（MIT の data）から MFX の値を `intel/genxml-video.h` に転記（WS031 と同じ方針） | 可 |
| H5 | 試験の stream を host の ffmpeg（libx264）で合成の絵から作る | 可 |
| HD1 | capset に native の語（176 byte、byte 168 tag、172 の bit）を足し、Venus の fork の vendor flags は使わない（`proposed/README.md`） | 可 |
| HD2 | engine reset の無い VCS に信頼できない動画を流す危険: (a) p002〜p006 は reset 無しで進め（hang は video だけ止め context を残す、§6.1）、WS121・WS122 に video を開く前に `GRDOM_MEDIA` の reset を別 Phase で入れる、(b) reset を先に作る | (a) |
| HD3 | 5330 が戻った時、5330 の host の Linux で ANV（`ANV_VIDEO_DECODE=1`）に同じ stream を decode させ MFX の field を比べてよいか | 可（5330 が戻ってから） |
| HD4 | 試験の stream: (a) 小さい合成の stream（合計 200 KiB 以下）と参照の hash を `plan/ws083/tests/streams/` に commit、(b) 加えて ITU-T H.264.1 の conformance の stream（long term・frame_num の gap）を取得して tree の外で使う、(c) どちらも tree に入れない | (a)、(b) は p006 で要れば改めて聞く |
| HD5 | ベータ2（fg019）で decode した絵を画面に出す経路（SAMPLED・TRANSFER_SRC の usage）が要るか | 要らない（p008 で ws031-p037 と合わせて判断） |
| HD6 | 規格に合わない点を全部記録して名乗る（N1 apiVersion 1.0、N2 ycbcr の拡張無しで NV12・plane の aspect、N3 OPTIMAL の subresource layout） | 可（H2 (a) と一緒に） |

## 11. リスクと確かめていない事

| | 内容 | 影響・備え |
| --- | --- | --- |
| U1 | short format（D1）の実機の動作 | 動かなければ long format（slice header の parser）へ |
| U2 | HuC 不要は ANV からの推論 | p005 で確かめる。要るなら Future |
| U3 | Gen12 LP の MFX の linear の宛先 | Tile Y で進める |
| U4 | `MI_FORCE_WAKEUP` の要否 | ANV と同じく書く |
| U5 | MV buffer の整列 4 KiB（ANV は 64 KiB） | 実機で崩れれば 64 KiB |
| U6 | 4096 幅・level 5.1 の実機の性能 | p008 |
| U7 | Venus の wire の video | D2 で送らない |
| U8 | 1 worker の同期実行で decode と描画が互いに待つ | p008 で測る |
| U9 | hang の回復が無い | §6.1 の封じ込め、HD2 |
| U10 | 4 byte の start code の app | §6.6 の 8 で受ける |
| U11 | 版 9 の意味（`is_reference`）の実装と、1.4.309 の video の構造体の配置が 1.3.269 の core と混ざって問題が無いか | p002 の host の試験と ABI の検査 |
| U12 | zig-zag と default scaling list の表の誤り | D24 の decoder と非対称の cqm の stream |
| U13 | libvulkan の export の数の checker の所在 | p002 |
| U14 | （解決）`MFX_WAIT` の Command Subtype は `gen75.xml` 2019 の既定 1 | — |
| U15 | D17 の検べが hardware の hang を全部防ぐかは不明（正しい形の値でも MFX が止まる bitstream） | §6.1 の封じ込め、HD2 |
| U16 | 宛先の address を使わない参照に入れる D21 が hardware に読まれても害が無いか | 読まれても宛先と同じ image で fault しない。p005 で hash を見る |
| U17 | 版 9 の `is_reference` 0 の setup slot の扱い（無効にする、§6.5）と、Begin で inactive の slot を `slotIndex ≥ 0` で bind してよいか、4 byte の start code を規格が許すか、`VkVideoFormatPropertiesKHR.imageUsageFlags` の意味は、規格の本文が disk に無く推測 | p002 の着手の時に Vulkan の規格（1.3.274 以降の "DPB Slot States"・H.264 decode の節）で確かめ、違えば §3.6・§6.5・§6.6 を直す |

## 12. 敵対的レビュー（自己、第 1 版の時点）

| 指摘 | 扱い |
| --- | --- |
| A1 1.1 + sync2 が要るのに 1.0 で名乗る | H2・HD6 |
| A2 Venus の host が video の family を出すと拡張の無い family が見える | D3 |
| A3 実行器が queue の family を読み捨てる | §3.2: family を覚え submit で引く |
| A4 bind 前の Begin | D18 |
| A5 setup の無い picture の MV buffer | 予備の bind |
| A6 Reference Picture と PICID の並びの対応 | §1.5 の ANV の事実と §6.3 の 6・9・10・13 を i で |
| A7 readback | N3 の私的な約束と de-tile、HD5 |
| A8 1 MiB で記録を流す形 | 流すのは decode の prefix だけ |
| A9 worker の context 表が 2 つ | `i915_worker_find` が engine で表を選ぶ |
| A10 `referenceSlotCount` > 16 | §6.6 の 6 |
| A11 Number of Reference Frames | ANV と同じ。実機の P/B で |
| A12 SPS・PPS の写しの二重 | 受け入れる |
| A13 display の担当との衝突 | `i915.c`・`device.c` は GT 側、`display/` は触らない |
| A14 `GPU_OP_PROTOCOL_VERSION` 2 | 利用箇所は無い（§1.1） |

## 13. 範囲外

encode、H.265・AV1、interlace（field・MBAFF）、配列の DPB、保護 content、result status query と inline query、decode 出力の sampler・copy（p008 と HD5）、VCS2 の利用、engine reset（HD2）、非同期の実行、Venus での video。

## 14. 第 2 版の反映の対応（design-reviewer、2026-10-07）

| 指摘 | 反映 |
| --- | --- |
| B1 Begin の slot の意味 | §3.6・§4.2（resource の present）・§6.5・§8.1 |
| B2 scaling list の並べ替え | §1.5・§6.3 の 12・§6.5・§8.1（非対称）・§8.4 |
| B3 kernel が app の値を信じる | D17・§6.6・§8.1 |
| B4 capset の bit | D2・§1.4・§4.3・`proposed/README.md`・HD1 |
| S1 engine record の所在 | §1.3・§6.1 |
| S2 family 1 の同期の command | §3.2 |
| S3 EINVAL の写像 | §1.2・D18 |
| S4 NV12 と ycbcr | D5 の N2・HD6 |
| S5 format の feature | D23・§3.4 |
| S6 VCS の hang | §6.1・HD2・U15 |
| S7 Frame Size 16 bit | §1.5・D20・§6.3 の 11・§6.6 |
| S8 golden の自己参照 | D24・§8.1・HD3 |
| S9 x264 の限界・frame の順・image の規則 | D16・§8.4・HD4 |
| S10 試験の app の Phase、OPTIMAL の layout | §9 p004・D5 の N3・§3.4 |
| S11 Phase の依存、bit の門 | D19・§9 |
| S12 `MFX_WAIT` の subtype | §1.5・§6.3 の 3・U14 |
| S13 batch の大きさ | §6.2 |
| S14 pinned の header が disk に無い | D14・§1.1・H3 |
| S15 使わない参照の address | D21・§6.3 |
| S16 parameters の memory | D22 |
| S17 遅延の VCS context | D11・§6.1 |
| minor（行番号、hunk の順、`OWN_LAST` は実行器へ、`proposed/` へ、ScalingLists の本数、V の Y Offset、`.inc`、dmesg の vcs0） | §1・§4.1・§4.2・§6.3 の 5・§6.5・§1.3 |

## 15. design-reviewer の review（第 1 版への、2026-10-07、履歴）

結果: blocking 4・should-fix 17・minor 6+。

- B1 Begin の DPB slot の意味が逆: `slotIndex = -1` は「slot 無しで bind」（setup picture）、slot を無効にするのは slot index と `pPictureResource = NULL`（vk.xml:7439 optional）。
- B2 scaling list の並べ替えが逆: `StdVideoH264ScalingLists` は scan 順、hardware は raster 順（ANV `genX_cmd_video.c` 1155–1182）。非対称の cqm の試験 stream を足す。
- B3 kernel が app の SPS の値を検べずに MFX の command を作る。
- B4 capset の vendor bit が曖昧、Venus の fork も同じ magic で flags 3・7・15 の 168 byte を出す → HD1。
- should-fix S1〜S17、minor、追加の人の判断 HD1〜HD6（内容は §14 の反映先を参照）。
- 正しいと確かめられた点: short format、§6.3 の command の長さ・opcode、genxml の SHA、row store・MV の大きさ、Gen12 LP の Tile Y、既存の forcewake・AUX・TLB・xcs flush・VCS の割込み・execlists。

## 16. 第 3 版の反映の対応（design-reviewer の再 review、2026-10-07）

| 指摘 | 反映 |
| --- | --- |
| blocking 1 `is_reference` 0 の setup slot | §3.6・§6.5（無効にする）・§8.1・U17 |
| blocking 2 hang の後の資源の解放 | §6.1 の hang の封じ込め（quarantine、VCS の context と batch の保持）・p003a |
| S1 飛ばした decode の slot、一部だけ走る拒否 | §6.5（飛ばしても遷移、submit の前の模擬）・§6.6 の末 |
| S2 参照の layout | §6.6 の 6 |
| S3 4 KiB の整列 | §6.3 の 7・14、§6.6 の 7 |
| S4 vkFreeMemory の後の pointer | §6.5 の memory の寿命（既存の欠陥は Bug ticket を Q1 に依頼） |
| S5 slice の数 | §6.2・§6.6 の 9（飛ばす、N4） |
| S6 slice の長さ・4 byte の start code | §6.6 の 8・U17 |
| S7 video の batch と RCS の flush | §6.2 |
| S8 i915 の capset の Phase と順序 | §4.3 の順序・p003b |
| S9 timeline | §4.3 の timeline・p003b |
| S10 family 1 の出し方 | §4.3 の family 1 |
| S11 Phase の依存 | §9（p003a・p003b、p006a・p006b、p007 の reset、p008、§8.2 は p004 の後） |
| S12 genxml の継承と出典 | D24・§1.5 |
| minor 1 `i915_boot_word` | D19 |
| minor 2 `enum i915_gfx_op_kind` | §6.2 |
| minor 3 CMD_* の op の行き先 | §6.2 |
| minor 4 valid usage の分類 | §6.6 の末 |
| minor 5 inactive の slot の bind | §6.5・U17 |
| minor 6 `imageUsageFlags` | §3.4・U17 |
| minor 7 memory type | §1.2 |
| minor 8 行番号 | §1.4 |
| minor 9 x264 の cqm・B | §8.4 |
| minor 10 Phase の番号 | §9（p008 を使った） |
| minor 11 rollback・parameters の session・context の表・bit field | §6.2 |
| minor 12 transport.c の行 | 事実の記述は 1858〜1893 として読む（§1.4 の 1860〜1893 は範囲の中） |

第 2 版への再 review の全文は Q1 への報告にある（P1 の 2026-10-07 の返却）。要点: B2・B4 は解消、命令の長さ・SHA・版・engine の事実は確かめられた。

## 17. 第 3.1 版（最後の review、2026-10-07）

最後の review（第 3 版へ）: blocking 1・should-fix 7・minor 7。前回の blocking 1 は解消、blocking 2 は方向は正しく保持の穴が 1 つ（S1）。全部を本文に反映した（再 review はしない、P1 の規則の「1 回まで」）。

| 指摘 | 反映 |
| --- | --- |
| B1 family 1 の timeline を VCS0 に写すと marker が VCS0 で走る | §4.3 の timeline（全て RCS0 に、byte 152 は変えない） |
| S1 保持した record が解放済みの session を指す | §6.1（owner から切り離し「保持中」、attach を拒む、worker は残る） |
| S2 batch pool の事実・video の batch の持ち主・EIO・「render は続ける」の範囲 | §6.1 |
| S3 boot の parser | D19（`src/kern/boot.c` を p003b に） |
| S4 dispatch の経路 | §6.2 |
| S5 start code の矛盾・6 番の分類 | §6.6 の 5・8 と末、§6.3、U10 |
| S6 §9 の循環 | §9（§8.2 は p004 の受け入れ） |
| S7 模擬の定義 | §6.5 |
| minor 1〜7 | proposed/README.md の版、行番号、p008 への参照、p003a の注、sync2 の条件、checked reset（§6.1）、328 dword |
