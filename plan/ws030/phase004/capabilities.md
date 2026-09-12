# q308 Vulkan 1.0 能力・制限の限定監査

2026-09-13。対象は現行 zedBSD 実装と virglrenderer 1.1.0 commit `1aeaf5e10a9c89096e96d09599aa419d5c50712f` の実 dispatch。`/tmp/q306-virglrenderer` の main working tree は根拠にしていない。これは source/peer/runtime 証拠の範囲を整理する監査であり、CTS 合格や任意機能全組合せの実機検証を意味しない。

## 確定した修正

`device.c` の native queue timeline は context ごとに ringIdx1..63。変更前 `physical_load_queues()` は native `queueCount` をそのまま公開し、例えば64個の同一family queueを実際には作れなかった。現在は `internal.h` の共通 `VULKAN_QUEUE_TIMELINE_COUNT=64` を参照し、全familyへ1個ずつ確保してから残りを順に配る。native family index/flags/timestamp/granularityは変更せず、公開queue合計を63以下にする。64/2/3は61/1/1。64以上のfamily、queueCount0は非互換として受理しない。これで公開した全familyの最大queue集合を単一deviceで予約できる。既存deviceが同じcontextの枠を使用中なら通常の資源不足として失敗する。独立discovery peer fixtureの更新は別担当。

## 55個の core feature が通る実装経路

`instance.c:physical_load` は全VkPhysicalDeviceFeaturesを型付き復号し、`device.c:device_validate` が要求bitを同じsnapshotと比較する。native CreateDeviceは実際の `pEnabledFeatures` を全フィールド転送し、未要求featureを一括enableしない。codec入力・出力両方に全55フィールドがある。

| feature群 | 具体的な入力・処理経路 | 監査結論 |
|---|---|---|
| robustBufferAccess | native device feature選択＋普通のdescriptor/buffer binding＋未変更SPIR-V | nativeのrobust処理を要求する経路がある |
| fullDrawIndexUint32, multiDrawIndirect, drawIndirectFirstInstance | CmdBindIndexBufferのindexType、CmdDraw/DrawIndexed/両Indirectの全count/stride/instance/index値 | 値を縮小・固定しない |
| imageCubeArray, textureCompressionETC2/ASTC_LDR/BC | Image/CreateView全type/format/flags/extent/layers、実format問い合わせ、buffer/image copy全region | 特定texture形式への固定がない |
| geometryShader, tessellationShader | 全stage配列、module/entry/specialization、tessellation control stageに応じたpatch state、通常renderpass | VS/FSだけへの限定がない |
| independentBlend, dualSrcBlend, logicOp | 全attachment blend record、logicOp有効時のoperation、SPIR-V output | attachment0だけへの固定がない |
| sampleRateShading, alphaToOne, variableMultisampleRate | multisample stateのsamples/mask/sampleShading/minSampleShading/alpha値、subpass全attachment descriptions | sample count1だけへの限定がない |
| depthClamp, depthBiasClamp, fillModeNonSolid, depthBounds, wideLines, largePoints, multiViewport | rasterization/depth/stencil state、dynamic viewport/scissor/depthbias/depthbounds/linewidth、SPIR-V PointSize | 動的/静的stateの選択を保持する |
| samplerAnisotropy | SamplerCreateInfoのanisotropyEnable/maxAnisotropyその他全sampler値 | native samplerへ転送する |
| occlusionQueryPrecise, pipelineStatisticsQuery, inheritedQueries | QueryPool全type/statistics、BeginQuery flags、availability付きresults、secondary inheritance/queryFlags/pipelineStatistics | result個数・64-bit/stride/availabilityを独立実装する |
| vertexPipelineStoresAndAtomics, fragmentStoresAndAtomics, shaderTessellationAndGeometryPointSize | native enabled bits＋SPIR-V全word＋全shader stage | guest側で命令・stageを置換しない |
| shaderImageGatherExtended, shaderStorageImageExtendedFormats/Multisample/ReadWithoutFormat/WriteWithoutFormat | descriptor全種類/format、native format回答、SPIR-V全word | scene専用image経路がない |
| shaderUniformBufferArrayDynamicIndexing, shaderSampledImageArrayDynamicIndexing, shaderStorageBufferArrayDynamicIndexing, shaderStorageImageArrayDynamicIndexing | 全descriptor array/binding/dynamic offset、SPIR-V全word | 1 descriptorだけの固定経路がない |
| shaderClipDistance, shaderCullDistance, shaderFloat64, shaderInt64, shaderInt16 | native feature選択＋SPIR-V全word＋specializationの原bytes | guest CPUの型幅へshader値を変換しない |
| shaderResourceResidency, shaderResourceMinLod | native feature選択＋SPIR-V全word、通常sparse resource path | wire上で関連命令を除外しない |
| sparseBinding, sparseResidencyBuffer/Image2D/Image3D/2Samples/4Samples/8Samples/16Samples/Aliased | native sparse format/memory requirements、Image/Buffer flags、QueueBindSparseのbuffer/opaque-image/tiled-image、NULL memory unbind、wait/signal/fence | sparseBindingだけ成功stubにする経路はない |

この分類で追加maskが必要と確定した VkPhysicalDeviceFeatures member は見つからなかった。これは「各familyの全フィールドが実native commandへ届く」というsource根拠であり、例えばBC/ASTC・float64・sparse等が対象ANVで実行検証されたという記録ではない。hostがfalseのbitはfalseのままで、アプリが要求すればFEATURE_NOT_PRESENTになる。

## native転送だけでは成立しない処理と公開方針

- `GetDeviceQueue` は pinned dispatchでfatalとなるため、内部は必須MESA timeline付き `GetDeviceQueue2` を使用し、公開呼出しは既存queueを返す。
- `QueueWaitIdle/DeviceWaitIdle` の pinned入口も使わず、実submit/fenceとWSI drainで完了を待つ。queue/fenceの無期限待ちと有限decoder deadlineを混同しない。
- native Map/Unmap/Flush/Invalidate dispatchはNULL。公開HOST_VISIBLEは真の共有mmapを持つHOST_COHERENT typeだけ。noncoherent typeはtype indexを保ってHOST_VISIBLE/HOST_CACHEDを除く。core必須のcoherent-host-visibleとdevice-local choicesが残らなければ物理デバイスを受理しない。
- surface/display/swapchainはlocal WSI。device extensionは実Kの DISPLAY|RESOURCE|TRANSFER がそろうときだけ公開する。外部memory/semaphore/fence、後続core、追加extensionは公開しない。
- `minMemoryMapAlignment=64` はlocal CPU viewの保証。4096 byteページ境界からmapし、返却pointerからoffsetを引いた値は64以上のalignmentを満たす。64はcoreの最低保証であり、上限ではない。

## 有限資源と必須limitの区別

- object registry、descriptor/pipeline/command arrays、GPU session resource tableは動的。旧32 resource/固定16KiB command上限をVulkan能力として残していない。大きいcommandはshared ExecuteCommandStreamsMESAへ切り替わる。96/97個や80KiBのpeer evidenceはこの経路の証拠で、core全上限の実測ではない。
- GPU backendの `max_resources=UINT32_MAX`、1blobとapertureは256MiB。HOST_VISIBLE allocationはページ丸めし、allocation時に実exportし、window不足はOUT_OF_DEVICE_MEMORY。これはnative heap全容量がguestから同時にCPU可視だという保証ではない。非HOST_VISIBLE native memoryはこのblob上限を課さない。公開heap sizeを空き容量と説明してはいけない。
- `maxMemoryAllocationCount`/`maxSamplerAllocationCount` はnative値を保つ。前者4096、後者4000のcore下限に逆らうlocal固定object数はない。型数32/heap数16はAPI自体の固定配列境界。coherent allocations4096個×1pageは16MiBなので256MiB窓という事実だけからこの下限違反とはいえない。最大寸法・全format・最大descriptor配列等の限界実測は未実施。
- dimensions/formats/samples/descriptors/shader limits、bufferImageGranularity、nonCoherentAtomSize、memoryTypeBits、sparse propertiesはnative typed回答。localでshaderやresource形状を書き換えないため、forwarded機能とのsource上の矛盾は見つからない。native1.1>=core1.0を前提とするが、host全limitのCTS検証をここで代替していない。
- 10秒decoder watchdogとclock停止時の有限pollがある。正当に非常に長いnative compilationでもtimeoutでcontext失効し得る実用上の限界として記録する。GPU fenceの標準timeoutを10秒へ切り詰める実装ではない。

## 根拠

公式API規則: [featureの公開とenable](https://docs.vulkan.org/spec/latest/chapters/features.html)、[core required limits](https://docs.vulkan.org/spec/latest/chapters/limits.html)、[memory allocationとmapping](https://docs.vulkan.org/spec/latest/chapters/memory.html)。

固定renderer実装: `/tmp/q308-virglrenderer-1.1.0/src/venus/vkr_physical_device.c:530`（native features/formats）、`vkr_device.c:127`（内蔵external-memory支援extension追加とnative create）、`vkr_queue.c:264`（固有ring）、`vkr_queue.c:359`（旧queue入口禁止）、`vkr_device_memory.c:451`（cache/map入口NULL）、`vkr_pipeline.c:11`（SPIR-V全word受理、4-byte境界検査）。

local: `userland/base/libvulkan/instance.c` physical_load/physical_load_queues、`device.c` device_validate/device_create_remote/queue_timeline_reserve、`codec.c` VkPhysicalDeviceFeatures/全型付き入出力、`commands-generated.inc` 全44recording、`pipeline.c` graphics/compute、`queue.c` queue_write_sparse、`query.c` availability付き結果、`memory.c` shared mapping/export、`context.c` direct/shared streamとdecoder deadline、`src/drivers/gpu/venus/internal.h:28` と `venus.c:468`（実K資源上限）。
