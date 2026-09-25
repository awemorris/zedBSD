# WS031 libvulkan → kernel → GPU 呼出し経路の調査（2026-09-18、読み取りのみ）

目的: Vulkan アプリから GPU までの既存の呼出し経路を、コードに実在するものだけで 1 本の図にし、後で表示側に別の似た入口を増やさないための材料にする。ファイルは一切変更していない。全 169 entry point の監査まではしていない（明記）。

結論: **現状、アプリから i915 GPU までの経路は端から端までは繋がっていない。** (1) legacy i915 の Vulkan executor が返す capability set を libvulkan が拒否する。(2) executor の多くが stub。(3) parity 構成では device が公開されず（`/dev/gpuN` 無し）、`parity/` は `drv_gpu_*` も `drv_i915_vk_*` も参照していない。

## 1. 呼出し経路

```
Vulkan アプリ（例: userland/base/vkdemo、標準の <vulkan/vulkan.h>）
  -> /lib/libvulkan.so（userland/base/libvulkan/、vk* 169 個、dispatch-table.inc）
       vkXxx() -> family 別 encoder（codec.c, commands.c, pipeline.c …）= Venus wire format 1（opcodes.h）
       -> vulkan_context_execute/transaction（context.c:442/708）
            先頭に vkSetReplyCommandStreamMESA(178)、末尾に trailer（137 probe）
  == user/kernel 境界: /dev/gpuN、ioctl 'G'（include/uapi/gpu.h） ==
       open(/dev/gpu*) instance.c:121, context.c:95
       GPU_GET_INFO(0) c:106, GPU_GET_CAPSET(3) c:132, GPU_BLOB_CREATE(4) c:321,
       GPU_RESOURCE_MAP(9)+mmap c:603/626, GPU_COMMAND_SUBMIT(12) c:803 | GPU_COMMAND(7) c:812,
       GPU_COMMAND_WAIT(13) c:975
  -> GPU core src/drivers/gpu/gpu.c: copyin、version/size/flags/bytes<=65536/word 整列の検査、
       stream の kernel 内 snapshot（gpu.c:2540-2571, 4584-4628）-> ops->command / commands->submit
  -> i915 backend src/drivers/gpu/i915/i915.c: i915_command(1163) / i915_command_submit(1208)
       先頭語 != I915_STREAM_MAGIC(0x31394958) なら Vulkan stream
       i915_vk_command_reply(1097): session の slot id で reply blob を解決
       -> drv_i915_vk_command（vk/vk.c:115）-> i915_vk_cmd_dispatch（vk/cmd.c:319）-> res/pipe/cmdbuf/sync
  -> legacy の service: drv_i915_gem_create + drv_i915_gem_bind_vm(session->gpu->vm)［open ごとの PPGTT］
       i915_vk_queue_submit（vk/cmdbuf.c:781）: RCS0 で drv_i915_request_alloc/queue/kick、
       request->context = &gpu->contexts[RCS0]（LRC は i915_open で生成、i915.c:697）
  -> execlists/ring -> GPU; irq -> engine.c:295 retire -> completed_seqno（request.c:172/212）、
       waitq_wake_all(retire_waitq)（request.c:329）-> fence ready（vk/sync.c:160）
```

## 2. libvulkan

- 場所: `userland/base/libvulkan/`、`/lib/libvulkan.so`。公開 header は `include/libc/vulkan/`。
- **Khronos の loader ではない。** Venus wire protocol の client として Vulkan を実装する、プロジェクト固有のライブラリ。ICD も layer も無い。README は「現在の backend は amd64 zedBSD 上の Venus」とし、SPIR-V は native driver へそのまま渡すと書いている。
- entry point: `api-commands.tsv` と `dispatch-table.inc` で 169 個（README の 155 とは不一致）。Vulkan 1.0 core 137、拡張 32（surface 5、display 7、swapchain 5、display_swapchain 1、wayland 2、properties2 7、external memory 3、external fence 3）。
- 明らかな stub は見つからなかった（例: `vkQueueBindSparse` は実際に encode して enqueue、`queue.c:85`）。全数の監査はしていない。

## 3. 実装の所在と境界

- **分担**: userland = API object、wire codec、WSI。kernel の `vk/` executor = stream の decode と GPU 固有の全処理（`vk-internal.h:11-13`）。
- device node: `/dev/gpuN`（GPU core が公開、`include/drivers/gpu.h:197`。libvulkan は `/dev/gpu*` を走査、`instance.c:1325`）。
- ioctl: `'G'` 0〜13 は `include/uapi/gpu.h:46-59`。他に `gpu-allocation.h`（14-15）、`gpu-fence.h`（16-23）、`gpu-display.h`（24-29, 32）、`gpu-scanout.h`（30, 31, 33）、`gpu-job.h`（34-38）。

| struct | 主な field | size |
|---|---|---|
| `gpu_command` | version, size, address u64, bytes, flags | 24 B |
| `gpu_command_submit` | ＋ timeline, reserved, sequence u64 | 40 B |
| `gpu_command_wait` | version, size, sequence, timeout_ns, flags, status | 32 B |
| `gpu_blob_create` | version, size, bytes, blob_id, handle, flags, resource_id | — |
| `gpu_resource_map` | version, size, handle, offset, bytes | — |
| `gpu_capset` | header ＋ data[256] | — |

- GPU core の検査: ABI version と struct size、flags、byte 数 ≤ `GPU_COMMAND_MAX`、4 byte 整列、user address 範囲（`gpu_user_range`）、stream の kernel 内完全複製、session 所有の handle（`gpu.c:2540-2571, 4584-4628`）。
- executor 側の検査は bounds 付き reader／writer のみ（`vk/cmd.c:183-315`）。**穴**: object 表が device 単位で session 単位でない（`cmd.c:33, 137`）。`i915_vk_buffer_bind`／`i915_vk_image_bind` が offset を memory size と照合しない（`res.c:211, 261`）。`i915_vk_command_reply` は `offset >= object->bytes` を検査する（`i915.c:1125`）。

### 現状つながらない理由

| 問題 | 内容 |
|---|---|
| capability set の拒否 | executor は `capset[0]=1` を 156 byte だけ返す（`vk/vk.c:174-176`）。libvulkan は XML protocol version 1.3.269 と offset 152 の非 0 の timeline 数を要求（`context.c:141-159`）、さらに magic `0x5a424453`・strict-queue・quiescence flag を持つ 168 byte の vendor suffix を要求（`instance.c:654-656`）。→ i915 node は `INCOMPATIBLE_DRIVER` として skip される |
| reader のずれ | `i915_vk_cmd_builtin` が instance／device 系 opcode（0-17, 19-20, 148+, 180）を payload を消費せずに受理（`cmd.c:463-466`）→ 以後 stream とずれる |
| command 記録の配送 | libvulkan は大きな stream に opcode 180（vkExecuteCommandStreamsMESA）を出す（`context.c:1080`）。executor は無視する（`cmdbuf.c:609-614` の comment は「vkCmd* の記録はそれで届く」と書いている） |

host 側の試験 `plan/ws031/tests/i915-vk-*-test.c` は存在するが、今回は中身を見ていない。

## 4. 機能ごとの担当層

| 機能 | 層と状態 |
|---|---|
| SPIR-V parse | kernel `vk/spirv.c:123`。少数の opcode のみ（FAdd/FSub/FMul/Dot/Compose/Extract/Sample/ExtInst）。libvulkan は SPIR-V を未解釈で転送（`resources-generated.inc:265`） |
| EU code 生成 | kernel `vk/compile.c:50`、`vk/eu.c`（`pipe.c:265-268` から）。GEM object に配置（`pipe.c:275-291`）。register 規約は "refined on hardware" と注記 |
| descriptor | libvulkan が encode（`descriptors.c:478`）。kernel は関数はあるが（`res.c:415-526`）opcode 70-79 は EINVAL（`res.c:872-895`）、`i915_vk_cmd_bind_descriptor_sets` は no-op（`cmdbuf.c:696`） |
| image layout／barrier | libvulkan が vkCmdPipelineBarrier を encode（`commands-generated.inc:1516`）。kernel に case 126 が無く EINVAL（`cmdbuf.c:615-641`）。layout 追跡無し |
| fence／semaphore | kernel の fence は seqno ベース（`sync.c:91-180`）、opcode 35-38 のみ。vkWaitForFences は client 側 polling（`sync.c:334-337`）。semaphore／event／query は関数はあるが dispatch されない。vkQueueSubmit の semaphore は parse して無視（`cmdbuf.c:562-587`）。libvulkan は `GPU_FENCE_*`／`GPU_JOB_*` ioctl も使う（`sync.c:455-688`、`external-fence.c`）。Vulkan stream での i915 job ops との関係は未追跡 |
| command buffer → GEN 命令 | kernel `vk/cmdbuf.c`。draw は PIPELINE_SELECT、STATE_BASE_ADDRESS（0 埋め）、3DSTATE_VS/PS、3DPRIMITIVE を出す（`cmdbuf.c:750-778`、`pipe.c:629-662`）。bind-vertex、render pass begin/end、push constant は no-op（`cmdbuf.c:679-747`）。vkCmdBeginRenderPass(133) は dispatch されない |
| submit | kernel `i915_vk_queue_submit`（`cmdbuf.c:781`）、RCS0 のみ |
| 完了待ち | kernel `i915_vk_fence_wait` が `retire_waitq` で sleep（`sync.c:128`）。decode は `drv_gpu_complete(completion, 0)` で同期完了（`i915.c:1242`）。userland は `GPU_COMMAND_WAIT` と reply trailer の polling |
| present／WSI | libvulkan が `GPU_DISPLAY_*` ioctl 上に swapchain を実装（`wsi-display.c`）。kernel `wsi.c:46` は EINVAL、`i915_vk_route` は wsi へ route しない。`display.c` は stub（1920×1080 固定、flip は no-op、`display.c:25-59`）。i915 の ops 表は present／display／scanout が NULL（`i915.c:577-584`）、driver は "no display" と log（`i915.c:268`） |

## 5. vk/ が依存する legacy の service（parity 側が同じ vk/ を動かすなら提供が必要なもの）

宣言は `src/drivers/gpu/i915/internal.h:399-429`。

- `drv_i915_gem_create(device, bytes, &obj)`、`drv_i915_gem_destroy`（`gem.c:32, 87`）。
- `drv_i915_gem_bind_vm(struct i915_ppgtt *, obj)`、`drv_i915_gem_unbind_vm`（`gem.c:175, 207`）。`obj->va`、`obj->run.paddr`、`obj->bytes` が要る。
- `drv_i915_request_alloc(engine, session, completion, &req)`、`drv_i915_request_queue`、`drv_i915_request_kick`（`request.c:64, 118, 139`）。vk 側は `request->context`、`->batch`、`->batch_va` を直接書き、kick 後に `->seqno` を読む。
- 直接触る struct field: `device->mutex`、`device->irq_lock`、`device->retire_waitq`、`device->engines[I915_ENGINE_RCS0].completed_seqno`／`.index`、`session->vm`、`session->contexts[]`、`session->objects`、`object->slot`。
- kernel primitive: `kern_pmem_to_kernel`、`kern_io_write_barrier`、`waitq_sequence`／`waitq_sleep`、`kern_calloc`／`kern_free`。
- `i915.c` の glue: `drv_i915_vk_attach`(400)、`drv_i915_vk_open`(711)、`drv_i915_vk_close`(745)、`i915_get_capset`(1133)、`i915_blob_create`(984、resource slot id を割当)、`i915_resource_map`(1051)。加えて `drv_gpu_register` の ops 表（`i915.c:563-596`）と IRQ retire 経路（`engine.c:295`、`request.c:329`）。
- `CONFIG_DRIVER_PCI_I915_PARITY` では attach が公開前に return する（`i915.c:271-280`）→ `/dev/gpuN` も vk attach も無い。

## 6. CPU fallback

`vk/` にも libvulkan にも CPU で描画する経路は見つからなかった。CPU 複製は、GPU が描いた image の present 時転送だけ（`wsi-display.c:1326`、`GPU_RESOURCE_WRITE`）。README も「coherent mapping の代わりに CPU copy を使わない」と書いている。

## 7. PIPELINE_SELECT

`vk/linux/3dstate-gen12.inc:25` が `0x6904U`、3D／GPGPU の dword を静的検査（:59-62）。Vulkan executor での使用は `vk/pipe.c:657` の 1 箇所（`i915_vk_cmd_draw` が `cmdbuf.c:763` から呼ぶ）。legacy `selftest.c` は 362、1011、1529、1552。`parity/eu_test.c:39` は同じ符号化の自前定義。

## 8. 後の設計への含意（提案であり決定ではない）

- 通常 client 経路（開く・使う・閉じる）は **GPU core（`src/drivers/gpu/gpu.c`）の既存 ioctl 契約**が入口として既にある。表示側に別の似た入口を作らず、parity の device を `drv_gpu_register` の ops 表へ繋ぐのが自然。present／display／scanout の ops が現在 NULL なので、LCD 実装はこの ops の形に合わせて作れる。
- vk/ executor が legacy の object／request／session に直接依存している（§5）。parity の VM／context／request を同じ形で提供する薄い層にするか、vk/ 側を parity の API へ向け直すかは、LCD 受入後の接続設計で決める。
- session 単位でない object 表、bind の offset 未検査は、正式なアプリ接続の前に直す項目。

> **訂正（E-106、2026-09-18）**: libvulkan は標準 Vulkan を実装する userland ライブラリで、Venus からは protocol の番号だけを借りている。「Venus wire protocol の client」という表現は誤解を招くため、設計の確認と実装範囲の数は `libvulkan-executor-scope.md` を参照。
