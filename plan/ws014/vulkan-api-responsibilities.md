# Vulkan APIごとのユーザー空間・GPUドライバ責務案

更新: 2026-09-12 / WS014 p001の設計資料。実装済み機能一覧でもABI確定版でもない。

## 対象と出典

- Vulkan **1.0〜1.4の全コア関数 234 件**と、下記の表示関連拡張の関数 41 件、計 **275 関数**。一関数一行。各コア節はそのversionで追加された関数。
- 関数集合はKhronos公式[VK XMLレジストリ](https://github.com/KhronosGroup/Vulkan-Headers/blob/ee2ec5fd83dafce291024683b50dc89219333076/registry/vk.xml)から抽出。commit `ee2ec5fd83dafce291024683b50dc89219333076`、XML SHA-256 `cf31c965cf6e788697139601da0c7e02a75a9b6c7ac764e7641f5521ffd9da06`。API名の出典: Copyright 2015–2026 The Khronos Group Inc., Apache-2.0 OR MIT。
- コア抽出は `feature` の `api` に `vulkan` を含み、`number` が1.0〜1.4である要素の `require/command`。api条件を適用し名前で重複排除する。レジストリのBASE/COMPUTE/GRAPHICS分割も含める。Vulkan SCは除外。
- コアへ昇格した関数のKHR/EXT別名は、選択した拡張内のものを除いて重複掲載しない。全ベンダー拡張、ray tracing/video、他OS専用surface、外部memory/fence/semaphore fd入出力拡張は今回の一覧の対象外。これらは必要になった時に別途評価する。全拡張込みのVulkan関数全集ではない。
- 関数追加のない表示関連拡張（present ID、colorspace、surface maintenance等）は行を持たない。採用時は構造体・feature・拡張依存条件を別途確認する。

## 読み方と構成

`アプリ/DE → ユーザー空間Vulkan実装 → /dev/gpu0 → zedBSD GPUドライバ → virtio-gpu → host renderer/GPU`

**libvulkan.so欄は、ローダーだけでなくICD・WSIを含むユーザー空間実装全体を表す。** 標準的なKhronos構成ではlibvulkan.soはローダー、ICDは別.soとなる。zedBSDで一体化するか分離するかは未決定。WSIはその表示部分であり、別デーモンを要求しない。[Loader/Driver Interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)

GPUドライバ欄は**zedBSDゲストのカーネル側**に必要な支援を示す。GPUで実行される全処理をカーネルに実装する意味ではない。pipeline compiler、descriptor encoding、Vulkan object管理はユーザー空間またはhost rendererへ置ける。分担はVulkan仕様が強制するものではなく、この構成に対する設計提案。

| 責務 | 意味 |
| --- | --- |
| U | ユーザー空間のVulkan実装（loader/ICD/WSI）が行う処理 |
| K | zedBSDカーネルのGPUドライバが提供する機能 |

分類はUとKのみとし、各関数の両欄に具体的な責務を記す。両方に仕事がある場合は両欄に記載し、片側の処理が不要なら「不要」とする。キャッシュ可能性、command記録、host転送は実装方法であり、独立した責務分類にはしない。ドライバからの情報取得はK、取得結果のVulkan形式への整形・実装能力との照合はU。キャッシュしても元の情報取得の責務は変わらない。

K欄はその操作を成立させるカーネル側機能を示し、各vk呼出しで必ずioctlするという意味ではない。記録時にUだけで処理し、事前のbacking確保や後のsubmitでKを使う場合も、具体的な時点を本文で区別する。

Venusではユーザーだけで完結するnative ICDの処理もhost転送を要する場合がある。host操作の転送も、関数ごとに一回の往復を必要とするわけではない。共有ring/メモリと通知を使う設計も可能。guestのVkハンドル、kernel resource ID、hostのVkハンドルは混同しない。[Mesa Venus](https://docs.mesa3d.org/drivers/venus.html)

カーネル境界を越える全経路で、ユーザー入力の長さ・offset・所有resource・権限をカーネル境界で検証する。Vulkan validation layerの有無に依存しない。ユーザーポインタやpNext連鎖、callbackポインタを生のままカーネル/hostへ渡さず、検証可能な転送形式へ変換する。

**本表に存在することは対応宣言ではない。** feature/拡張の任意性と、選択versionで必須となる要件は仕様に従う。関数を省略しただけの実装をVulkan 1.4対応とは呼ばない。表の「任意feature」はそのfeatureを有効化・公開した場合の支援を示す。

[Command buffers](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)、[Memory](https://docs.vulkan.org/spec/latest/chapters/memory.html)、[WSI](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)を意味論の参照先とする。表の具体的な配置はzedBSD向けの判断であり仕様の引用ではない。

## Vulkan 1.0 コア（137関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkCreateInstance` | instance、要求version/拡張、dispatchを構築 | デバイス列挙が必要ならアクセス可能なGPU情報を供給 |
| `vkDestroyInstance` | instanceとdispatch等を解放 | 開いたGPU接続があればclose時に所有資源を回収 |
| `vkEnumeratePhysicalDevices` | physical device/groupハンドルと一覧を構築 | アクセス可能なGPU、識別情報・group構成を取得 |
| `vkGetPhysicalDeviceFeatures` | feature bitを実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceFormatProperties` | format別の利用機能を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceImageFormatProperties` | 画像条件別の対応可否・上限を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceProperties` | device識別・limitsを実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceQueueFamilyProperties` | queue familyの種別・数・粒度を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceMemoryProperties` | memory heap/typeを実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetInstanceProcAddr` | version/有効拡張に応じて関数ポインタを返す | 不要。関数ポインタをカーネルへ渡さない |
| `vkGetDeviceProcAddr` | version/有効拡張に応じて関数ポインタを返す | 不要。関数ポインタをカーネルへ渡さない |
| `vkCreateDevice` | feature/拡張とqueue構成を選択しdeviceを構築 | GPU接続、隔離context、queue/transportと権限を確保 |
| `vkDestroyDevice` | ユーザーdeviceとbackendオブジェクトを解放 | context・queue・所有メモリを安全に回収 |
| `vkEnumerateInstanceExtensionProperties` | 実装・ローダー・layer情報を列挙。device layer列挙は旧API | 不要 |
| `vkEnumerateDeviceExtensionProperties` | backend能力とユーザー実装を突き合わせ拡張を列挙 | capset/対応機能取得を支援。問い合わせ結果はキャッシュ可 |
| `vkEnumerateInstanceLayerProperties` | 実装・ローダー・layer情報を列挙。device layer列挙は旧API | 不要 |
| `vkEnumerateDeviceLayerProperties` | 実装・ローダー・layer情報を列挙。device layer列挙は旧API | 不要 |
| `vkGetDeviceQueue` | 作成済みqueue構成からハンドルを返す | 新規queue作成は通常不要。CreateDevice時のqueueと対応 |
| `vkQueueSubmit` | command bufferとwait/signal依存を収集しsubmitを構成 | 参照資源/権限検証、送信、実行依存、完了通知。Venusではhost submitへ転送 |
| `vkQueueWaitIdle` | 対象queueの未完了処理を待つ | queueの実行完了を観測・待機。単なる転送完了と区別 |
| `vkDeviceWaitIdle` | device内の対象queueの完了を集約 | queue完了待機・device lost通知 |
| `vkAllocateMemory` | memory typeと割当方針、VkDeviceMemoryを管理 | backing/resource、GPU VA、必要な共有領域を確保・隔離 |
| `vkFreeMemory` | memoryハンドルと割当情報を解放 | backing/VA/mappingを回収。使用中資源の安全性を確保 |
| `vkMapMemory` | offset/sizeとmapping状態を管理しユーザーVAを返す | 所有メモリのmmap、アクセス属性と保護を設定 |
| `vkUnmapMemory` | mapping状態を解除 | mappingを解除または保持方針に従って管理 |
| `vkFlushMappedMemoryRanges` | non-coherent範囲のhost書込をdevice可視へ反映 | 必要なcache clean/共有メモリ同期。coherentなら追加処理不要の場合あり |
| `vkInvalidateMappedMemoryRanges` | non-coherent範囲のdevice書込をhost可視へ反映 | 必要なcache invalidate/共有メモリ同期。GPU完了待機とは別 |
| `vkGetDeviceMemoryCommitment` | 遅延割当memoryのcommit済み量を返す | 対応heapの実commit量を取得。通常の空きVRAM照会ではない |
| `vkBindBufferMemory` | bufferとmemory/offsetの結合を管理 | 必要なGPU VA/resource結合、範囲・所有権を検証 |
| `vkBindImageMemory` | image/planeとmemory/offsetの結合を管理 | 画像resourceのbacking結合、配置条件と所有権を検証 |
| `vkGetBufferMemoryRequirements` | buffer条件からsize/alignment/memoryTypeBitsを算出・取得 | backend条件を供給。照会だけで物理割当しない |
| `vkGetImageMemoryRequirements` | image形式・tiling等から割当要件を算出・取得 | backendの画像配置/割当条件を供給 |
| `vkGetImageSparseMemoryRequirements` | sparse imageのblock/miptail等の要件を返す | sparse対応の場合だけ粒度・結合制約を供給（当該feature対応時） |
| `vkGetPhysicalDeviceSparseImageFormatProperties` | sparse image format・block条件を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkQueueBindSparse` | sparse bindingと同期依存をまとめる | sparse mapping更新、範囲検証、queue同期。対応をadvertiseする場合に必要（当該feature対応時） |
| `vkCreateFence` | binary/timeline等の状態とハンドルを作成 | backend同期オブジェクトまたは共有状態/待機機構を確保 |
| `vkDestroyFence` | 同期ハンドルを解放 | 関連するbackend同期資源・待機参照を回収 |
| `vkResetFences` | 対象fenceを未通知状態へ戻す | backend同期状態をreset。実行中の仕事の取消ではない |
| `vkGetFenceStatus` | fenceの通知状態を返す | 実行完了状態の読出しまたは共有状態を供給 |
| `vkWaitForFences` | wait-all/any、timeoutを処理 | fence通知の待機・起床とdevice lost伝達 |
| `vkCreateSemaphore` | binary/timeline等の状態とハンドルを作成 | backend同期オブジェクトまたは共有状態/待機機構を確保 |
| `vkDestroySemaphore` | 同期ハンドルを解放 | 関連するbackend同期資源・待機参照を回収 |
| `vkCreateQueryPool` | query種別・数と結果領域を構成 | 必要な結果buffer/計測機能、backend query poolを支援 |
| `vkDestroyQueryPool` | query poolと結果管理を解放 | 結果領域/backend資源を回収 |
| `vkGetQueryPoolResults` | availability/stride/結果幅等に応じて結果を返す | 結果の可視性と、WAIT指定時の完了観測を支援 |
| `vkCreateBuffer` | bufferのサイズ・usageとハンドルを構成 | backend resourceが必要なら作成。memory確保・bindとは区別 |
| `vkDestroyBuffer` | bufferハンドルを解放 | backend resource/VA参照を解放。別memoryオブジェクトは自動freeしない |
| `vkCreateImage` | 画像のformat/extent/tiling/usageと配置情報を構成 | 必要ならbackend image/resourceを作成。memory確保・bindとは区別 |
| `vkDestroyImage` | imageと配置管理を解放 | backend image/resource参照を回収 |
| `vkGetImageSubresourceLayout` | 対象subresourceのoffset/pitch等を算出・取得 | backendのlayout情報を供給。汎用linear配置と仮定しない |
| `vkCreateImageView` | imageのformat/aspect/subresource viewを構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyImageView` | imageのformat/aspect/subresource viewの管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateCommandPool` | queue family/flagsとcommand storageのpoolを管理 | 必要なcommand backing/host pool用の共通機構 |
| `vkDestroyCommandPool` | poolと配下command bufferを解放 | backing/host pool回収を支援 |
| `vkResetCommandPool` | 全command bufferをresetし再利用する | 必要に応じhost reset/共有backing管理 |
| `vkAllocateCommandBuffers` | primary/secondary command bufferを割当 | backing/host object作成を必要に応じ支援 |
| `vkFreeCommandBuffers` | 指定command bufferをpoolへ返却 | backing/host object解放を必要に応じ支援 |
| `vkBeginCommandBuffer` | 記録状態へ移行しusage/inheritanceを設定 | 記録自体にGPU実行は不要。Venusではhost記録開始を転送可 |
| `vkEndCommandBuffer` | 記録を終えsubmit可能な形式へ確定 | GPU実行はしない。Venusではhost記録終了/結果転送 |
| `vkResetCommandBuffer` | 記録済み内容を破棄し初期状態へ戻す | 必要に応じhost reset/command backing再利用 |
| `vkCmdCopyBuffer` | buffer間コピー範囲をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyImage` | image間コピー領域/layoutをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyBufferToImage` | buffer→imageコピー領域/pitchをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyImageToBuffer` | image→bufferコピー領域/pitchをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdUpdateBuffer` | bufferへの即値更新データを記録時に取り込むをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdFillBuffer` | buffer充填範囲と32bit値をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPipelineBarrier` | stage/access依存、layout/ownership遷移をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBeginQuery` | query計測開始をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdEndQuery` | query計測終了をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdResetQueryPool` | query範囲のGPU resetをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdWriteTimestamp` | 指定stageでのtimestamp書込をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyQueryPoolResults` | query結果のbufferへのコピーをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdExecuteCommands` | secondary command buffer参照と実行順をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCreateEvent` | host/device eventオブジェクトを作成 | 必要ならGPU可視状態領域/backend eventを用意 |
| `vkDestroyEvent` | eventを解放 | 関連共有領域/backend eventを回収 |
| `vkGetEventStatus` | event set/reset状態を返す | 必要ならGPU可視状態を取得 |
| `vkSetEvent` | host側からeventをset | 共有状態の更新・可視性、backendへの反映 |
| `vkResetEvent` | host側からeventをreset | 共有状態の更新・可視性、backendへの反映 |
| `vkCreateBufferView` | texel bufferのformat/range viewを構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyBufferView` | texel bufferのformat/range viewの管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateShaderModule` | SPIR-V等のshader中間表現とmetadataを構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyShaderModule` | SPIR-V等のshader中間表現とmetadataの管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreatePipelineCache` | cache初期データを読み込み、互換性・保存領域を管理 | 専用ioctl不要。Venusではhost cache操作の共通転送 |
| `vkDestroyPipelineCache` | cache objectを解放 | Venusではhost解放操作を共通転送 |
| `vkGetPipelineCacheData` | 保存可能なcacheデータをserializeして返す | Venusではhostから結果を受信。専用cache ioctlは必須でない |
| `vkMergePipelineCaches` | 互換cacheを宛先へ統合 | Venusでは統合操作を共通転送 |
| `vkCreateComputePipelines` | compute shader compilation/link、pipeline生成。Venusではhostで生成 | コード/状態bufferのbacking・転送・隔離を支援 |
| `vkDestroyPipeline` | pipelineと関連ユーザー/backend資源を解放 | コード/状態buffer参照等を必要に応じ回収 |
| `vkCreatePipelineLayout` | descriptor set layoutとpush constant範囲を構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyPipelineLayout` | descriptor set layoutとpush constant範囲の管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateSampler` | filter/address/LOD等のsampling状態を構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroySampler` | filter/address/LOD等のsampling状態の管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateDescriptorSetLayout` | binding/type/countのlayoutを構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyDescriptorSetLayout` | binding/type/countのlayoutの管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateDescriptorPool` | descriptor割当領域とpool制約を管理 | 必要なGPU可視backing確保・host pool転送を支援 |
| `vkDestroyDescriptorPool` | poolと配下descriptor setを解放 | backing/host object回収を支援 |
| `vkResetDescriptorPool` | 配下setを無効化しpoolの割当管理をreset | 必要ならbackend pool reset/参照解除を支援 |
| `vkAllocateDescriptorSets` | poolからsetを割当、layoutとdescriptor格納先を関連付け | 必要なbacking/host割当操作の共通支援 |
| `vkFreeDescriptorSets` | 許可されたpoolのsetを解放 | 必要なhost解放操作/参照解除を支援 |
| `vkUpdateDescriptorSets` | descriptor write/copyを展開しresource参照を書き込む | GPU可視backing・可視性またはhost転送を支援。描画submitではない |
| `vkCmdBindPipeline` | graphics/compute pipeline選択をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindDescriptorSets` | descriptor setとdynamic offsetをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdClearColorImage` | color imageのclear値・範囲をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDispatch` | compute workgroup数をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDispatchIndirect` | compute引数bufferとoffsetをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetEvent` | device event setと依存stageをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdResetEvent` | device event resetと依存stageをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdWaitEvents` | event待機とmemory依存をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPushConstants` | push constantの値を記録時に取り込むをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCreateGraphicsPipelines` | graphics状態、shader compilation/link、pipeline生成。Venusではhostで生成 | コード/状態bufferのbacking・転送・隔離を支援。カーネルにshader compilerは不要 |
| `vkCreateFramebuffer` | render passに対応するattachment view集合を構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyFramebuffer` | render passに対応するattachment view集合の管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkCreateRenderPass` | attachment/subpass/dependencyのrender pass記述を構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyRenderPass` | attachment/subpass/dependencyのrender pass記述の管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkGetRenderAreaGranularity` | render pass/attachment条件から推奨描画粒度を返す | backendのtile/描画粒度条件を供給 |
| `vkCmdSetViewport` | viewport配列をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetScissor` | scissor矩形配列をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetLineWidth` | 線幅をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthBias` | depth bias係数をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetBlendConstants` | blend定数をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthBounds` | depth bounds範囲をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetStencilCompareMask` | stencil比較maskをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetStencilWriteMask` | stencil書込maskをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetStencilReference` | stencil参照値をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindIndexBuffer` | index buffer/offset/typeをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindVertexBuffers` | vertex buffer/offset配列をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDraw` | 頂点・instance数と開始位置をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDrawIndexed` | index付き描画範囲をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDrawIndirect` | 間接描画引数buffer/strideをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDrawIndexedIndirect` | index付き間接描画引数をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBlitImage` | 画像blit領域とfilterをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdClearDepthStencilImage` | depth/stencil clear値・範囲をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdClearAttachments` | attachment clearと矩形をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdResolveImage` | multisample resolve領域をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBeginRenderPass` | render pass/framebuffer/clear値で開始をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdNextSubpass` | 次subpassへ移行をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdEndRenderPass` | render pass終了をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |

## Vulkan 1.1 コア（28関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkEnumerateInstanceVersion` | ユーザー実装が提供するinstance API versionを返す | 不要 |
| `vkBindBufferMemory2` | bufferとmemory/offsetの結合を管理 | 必要なGPU VA/resource結合、範囲・所有権を検証 |
| `vkBindImageMemory2` | image/planeとmemory/offsetの結合を管理 | 画像resourceのbacking結合、配置条件と所有権を検証 |
| `vkGetDeviceGroupPeerMemoryFeatures` | heap別のdevice間アクセス能力を返す | 実際のpeer memory制約を供給 |
| `vkCmdSetDeviceMask` | device groupの実行maskをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkEnumeratePhysicalDeviceGroups` | physical device/groupハンドルと一覧を構築 | アクセス可能なGPU、識別情報・group構成を取得 |
| `vkGetImageMemoryRequirements2` | image形式・tiling等から割当要件を算出・取得 | backendの画像配置/割当条件を供給 |
| `vkGetBufferMemoryRequirements2` | buffer条件からsize/alignment/memoryTypeBitsを算出・取得 | backend条件を供給。照会だけで物理割当しない |
| `vkGetImageSparseMemoryRequirements2` | sparse imageのblock/miptail等の要件を返す | sparse対応の場合だけ粒度・結合制約を供給（当該feature対応時） |
| `vkGetPhysicalDeviceFeatures2` | feature bitを実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceProperties2` | device識別・limitsを実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceFormatProperties2` | format別の利用機能を実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceImageFormatProperties2` | 画像条件別の対応可否・上限を実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceQueueFamilyProperties2` | queue familyの種別・数・粒度を実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceMemoryProperties2` | memory heap/typeを実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceSparseImageFormatProperties2` | sparse image format・block条件を実装能力と照合して返す。pNextの各要求を処理 | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkTrimCommandPool` | 未使用のpool内部領域を返却 | 返却対象のbacking/host領域回収を支援 |
| `vkGetDeviceQueue2` | 作成済みqueue構成からハンドルを返す | 新規queue作成は通常不要。CreateDevice時のqueueと対応 |
| `vkGetPhysicalDeviceExternalBufferProperties` | 外部buffer handleの互換性とimport/export能力を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceExternalFenceProperties` | 外部fence handleのimport/export能力を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkGetPhysicalDeviceExternalSemaphoreProperties` | 外部semaphore handleのimport/export能力を実装能力と照合して返す | device/capset/backend情報を供給。実装できない機能はadvertiseしない |
| `vkCmdDispatchBase` | compute base groupとgroup数をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCreateDescriptorUpdateTemplate` | descriptor更新データの展開templateを構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroyDescriptorUpdateTemplate` | descriptor更新データの展開templateの管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |
| `vkUpdateDescriptorSetWithTemplate` | templateに従いdescriptor更新データを展開 | descriptor backingの可視性またはhost転送を支援 |
| `vkGetDescriptorSetLayoutSupport` | layoutの実装上の対応可否・上限を返す | backend制約を供給。専用ioctl不要 |
| `vkCreateSamplerYcbcrConversion` | YCbCr変換のformat/model/range状態を構築。Venusではhost object作成をencode | 専用のVkオブジェクトABIは必須でない。backing/転送/隔離を共通機構で支援 |
| `vkDestroySamplerYcbcrConversion` | YCbCr変換のformat/model/range状態の管理とbackend objectを解放 | 必要なbacking参照・転送資源の回収 |

## Vulkan 1.2 コア（13関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkResetQueryPool` | hostから指定query範囲を未使用状態に戻す | 必要に応じ共有結果状態/backend query poolを更新 |
| `vkGetSemaphoreCounterValue` | timeline semaphoreの現在値を返す | timeline値の観測を供給 |
| `vkWaitSemaphores` | timeline値とwait-all/any・timeoutを処理 | 指定値到達の待機と起床 |
| `vkSignalSemaphore` | hostからtimeline値をsignal | 同期値更新と依存するGPU/host待機者への通知 |
| `vkGetBufferDeviceAddress` | bufferに対応するGPUアドレスを取得 | 隔離GPU VAを割当/保持。CPU物理アドレスを公開しない |
| `vkGetBufferOpaqueCaptureAddress` | capture/replay用のopaque addressを返す | 対応時のみ再現可能なVA/resource識別を支援（当該feature対応時） |
| `vkGetDeviceMemoryOpaqueCaptureAddress` | capture/replay用のopaque addressを返す | 対応時のみ再現可能なVA/resource識別を支援（当該feature対応時） |
| `vkCmdDrawIndirectCount` | 描画引数bufferとGPU生成draw countをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdDrawIndexedIndirectCount` | index描画引数とGPU生成draw countをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCreateRenderPass2` | 拡張可能なrender pass記述を構築・backendへ反映 | 共通backing/転送機構。render pass自体をカーネルAPI化する必要なし |
| `vkCmdBeginRenderPass2` | render pass/framebuffer/clear値で開始（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdNextSubpass2` | 次subpassへ移行（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdEndRenderPass2` | render pass終了（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |

## Vulkan 1.3 コア（37関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkGetPhysicalDeviceToolProperties` | 接続されたtool/layerの情報を返す | 通常不要 |
| `vkCreatePrivateDataSlot` | アプリ用private dataの格納slotを作成 | 不要 |
| `vkDestroyPrivateDataSlot` | private data slotと付随データを破棄 | 不要 |
| `vkSetPrivateData` | Vulkan objectに64bitのアプリデータを関連付ける | 不要 |
| `vkGetPrivateData` | objectに関連するアプリデータを返す | 不要 |
| `vkCmdPipelineBarrier2` | stage/access依存、layout/ownership遷移（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdWriteTimestamp2` | 指定stageでのtimestamp書込（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkQueueSubmit2` | command bufferとwait/signal依存を収集しsubmitを構成 | 参照資源/権限検証、送信、実行依存、完了通知。Venusではhost submitへ転送 |
| `vkCmdCopyBuffer2` | buffer間コピー範囲（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyImage2` | image間コピー領域/layout（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyBufferToImage2` | buffer→imageコピー領域/pitch（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdCopyImageToBuffer2` | image→bufferコピー領域/pitch（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkGetDeviceBufferMemoryRequirements` | buffer条件からsize/alignment/memoryTypeBitsを算出・取得 | backend条件を供給。照会だけで物理割当しない |
| `vkGetDeviceImageMemoryRequirements` | image形式・tiling等から割当要件を算出・取得 | backendの画像配置/割当条件を供給 |
| `vkGetDeviceImageSparseMemoryRequirements` | sparse imageのblock/miptail等の要件を返す | sparse対応の場合だけ粒度・結合制約を供給（当該feature対応時） |
| `vkCmdSetEvent2` | device event setと依存stage（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdResetEvent2` | device event resetと依存stage（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdWaitEvents2` | event待機とmemory依存（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBlitImage2` | 画像blit領域とfilter（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdResolveImage2` | multisample resolve領域（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBeginRendering` | dynamic renderingのattachment/領域をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdEndRendering` | dynamic rendering終了をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetCullMode` | culling modeをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetFrontFace` | 表面の頂点順をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetPrimitiveTopology` | primitive topologyをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetViewportWithCount` | viewport数と配列をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetScissorWithCount` | scissor数と配列をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindVertexBuffers2` | vertex buffer/offset配列とsize/strideをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthTestEnable` | depth test有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthWriteEnable` | depth write有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthCompareOp` | depth比較演算をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthBoundsTestEnable` | depth bounds test有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetStencilTestEnable` | stencil test有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetStencilOp` | stencil演算と比較をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetRasterizerDiscardEnable` | rasterizer discard有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetDepthBiasEnable` | depth bias有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetPrimitiveRestartEnable` | primitive restart有効/無効をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |

## Vulkan 1.4 コア（19関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkMapMemory2` | offset/sizeとmapping状態を管理しユーザーVAを返す | 所有メモリのmmap、アクセス属性と保護を設定 |
| `vkUnmapMemory2` | mapping状態を解除 | mappingを解除または保持方針に従って管理 |
| `vkGetDeviceImageSubresourceLayout` | 対象subresourceのoffset/pitch等を算出・取得 | backendのlayout情報を供給。汎用linear配置と仮定しない |
| `vkGetImageSubresourceLayout2` | 対象subresourceのoffset/pitch等を算出・取得 | backendのlayout情報を供給。汎用linear配置と仮定しない |
| `vkCopyMemoryToImage` | host memoryからimageへコピー。format/layout/pitchを処理 | 必要なmapping/cache同期・backend host-copyを支援。queue記録APIではない |
| `vkCopyImageToMemory` | imageからhost memoryへコピー。layout/pitchと可視性を処理 | 必要なmapping/cache同期・backend host-copyを支援 |
| `vkCopyImageToImage` | host操作としてimage間コピーを実行/依頼 | backend host-copyやmapping/cache同期を支援 |
| `vkTransitionImageLayout` | host操作としてimage layout遷移を実行/依頼 | 必要なbackend layout更新/cache処理を支援。vkCmdPipelineBarrierとは区別 |
| `vkCmdPushDescriptorSet` | push descriptorのwrite内容をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPushDescriptorSetWithTemplate` | templateから展開したpush descriptorをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindDescriptorSets2` | descriptor setとdynamic offset（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPushConstants2` | push constantの値を記録時に取り込む（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPushDescriptorSet2` | push descriptorのwrite内容（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdPushDescriptorSetWithTemplate2` | templateから展開したpush descriptor（拡張可能な構造体形式）をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetLineStipple` | line stipple係数/patternをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdBindIndexBuffer2` | index buffer/offset/typeとsizeをcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkGetRenderingAreaGranularity` | render pass/attachment条件から推奨描画粒度を返す | backendのtile/描画粒度条件を供給 |
| `vkCmdSetRenderingAttachmentLocations` | color attachment location対応をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |
| `vkCmdSetRenderingInputAttachmentIndices` | input attachment index対応をcommand bufferへencode。Venusではhost記録用にserialize | 共通command backing/転送・隔離を支援。submit後の実行はGPU/host renderer。個別Vk ioctl不要 |

## VK_KHR_surface（5関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkDestroySurfaceKHR` | surface metadataと参照を解放。swapchainの寿命と区別 | 必要なら表示所有権/接続参照を解放 |
| `vkGetPhysicalDeviceSurfaceSupportKHR` | queue familyとsurfaceのpresent可否をWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` | 画像数・extent・usage等のsurface制約をWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |
| `vkGetPhysicalDeviceSurfaceFormatsKHR` | surfaceが受け付けるformat/colorspaceをWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |
| `vkGetPhysicalDeviceSurfacePresentModesKHR` | present modeをWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |

## VK_KHR_swapchain（9関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkCreateSwapchainKHR` | present mode/format/extentを選択し画像pool・状態を構成 | scanout可能なbacking、表示権限と切替機構を確保 |
| `vkDestroySwapchainKHR` | swapchainと画像の管理を解放。表示中の参照を適切に扱う | scanout参照を安全に外し資源を回収。display mode復帰を必要に応じ支援 |
| `vkGetSwapchainImagesKHR` | swapchain所有imageハンドルを列挙 | 通常追加操作なし |
| `vkAcquireNextImageKHR` | 再使用可能なimageを選択しtimeoutとsemaphore/fence通知を処理 | presentation側の画像解放を通知し、描画側同期との連携を支援 |
| `vkQueuePresentKHR` | 描画完了semaphoreを待つ要求とpresentを構成。display surfaceのmodeも扱う | buffer可視性、表示所有権、必要なmode切替とscanout、画像解放を支援 |
| `vkGetDeviceGroupPresentCapabilitiesKHR` | device groupのpresent mask/modeを返す | 実際のgroup間共有・表示制約を供給 |
| `vkGetDeviceGroupSurfacePresentModesKHR` | groupとsurfaceのpresent modeを返す | group/surface表示制約を供給 |
| `vkGetPhysicalDevicePresentRectanglesKHR` | deviceがpresentできるsurface矩形を返す | 表示領域/group配置を供給 |
| `vkAcquireNextImage2KHR` | 再使用可能なimageを選択しtimeoutとsemaphore/fence通知を処理 | presentation側の画像解放を通知し、描画側同期との連携を支援 |

## VK_KHR_display（7関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkGetPhysicalDeviceDisplayPropertiesKHR` | display識別/物理寸法/解像度等をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetPhysicalDeviceDisplayPlanePropertiesKHR` | planeと現在のdisplay対応をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetDisplayPlaneSupportedDisplaysKHR` | planeが接続可能なdisplayをVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetDisplayModePropertiesKHR` | display modeとrefresh rateをVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkCreateDisplayModeKHR` | 指定modeの可否を確認しmode objectを作成 | mode timing/extentの実現可否を供給。この呼出しで表示切替しない |
| `vkGetDisplayPlaneCapabilitiesKHR` | mode/planeのextent・alpha制約をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkCreateDisplayPlaneSurfaceKHR` | display/mode/plane/transformを保持するsurfaceを作成 | 必要な構成能力照会。実際のmode切替はpresent時 |

## VK_KHR_display_swapchain（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkCreateSharedSwapchainsKHR` | 複数の共有可能なdisplay swapchainを協調作成 | 複数出力で使える画像backingとscanout制約を支援（当該拡張を公開する場合） |

## VK_KHR_get_surface_capabilities2（2関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkGetPhysicalDeviceSurfaceCapabilities2KHR` | pNextを含むsurface制約をWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |
| `vkGetPhysicalDeviceSurfaceFormats2KHR` | 拡張形式のformat/colorspaceをWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |

## VK_KHR_get_display_properties2（4関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkGetPhysicalDeviceDisplayProperties2KHR` | 拡張形式のdisplay情報をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetPhysicalDeviceDisplayPlaneProperties2KHR` | 拡張形式のplane情報をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetDisplayModeProperties2KHR` | 拡張形式のmode情報をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |
| `vkGetDisplayPlaneCapabilities2KHR` | 拡張形式のmode/plane制約をVulkan objectへ対応付けて返す | 出力・mode/planeの実能力を取得。virtioで存在しない制御は捏造しない |

## VK_EXT_direct_mode_display（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkReleaseDisplayEXT` | direct displayの制御を返却し状態更新 | 表示権限の解放とOS側provider復帰を実行 |

## VK_EXT_display_surface_counter（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkGetPhysicalDeviceSurfaceCapabilities2EXT` | surface counterを含むsurface制約をWSI能力と照合して返す | scanout/backend能力を取得。host Vulkan能力だけでguest表示対応を宣言しない |

## VK_EXT_display_control（4関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkDisplayPowerControlEXT` | display電源状態の要求を変換 | 対応出力の電源制御。virtio backendの実能力がなければ拡張を公開しない（当該拡張を公開する場合） |
| `vkRegisterDeviceEventEXT` | device event要求と通知用fenceを関連付ける | 対象device event（display hotplug等）と通知連携（当該拡張を公開する場合） |
| `vkRegisterDisplayEventEXT` | display event要求と通知用fenceを関連付ける | first-pixel-out等、要求された実表示eventを通知（当該拡張を公開する場合） |
| `vkGetSwapchainCounterEXT` | 指定surface counterの値を返す | 対応vblank等のcounterを取得。virtqueue応答数で代用しない（当該拡張を公開する場合） |

## VK_KHR_present_wait（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkWaitForPresentKHR` | present IDを追跡し所定の表示完了/timeoutを処理 | 実際のpresentation進行を観測。GPU描画完了だけを返さない（当該拡張を公開する場合） |

## VK_KHR_present_wait2（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkWaitForPresent2KHR` | present IDを追跡し所定の表示完了/timeoutを処理 | 実際のpresentation進行を観測。GPU描画完了だけを返さない（当該拡張を公開する場合） |

## VK_EXT_headless_surface（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkCreateHeadlessSurfaceEXT` | 画面出力を伴わないheadless surfaceを作成 | 物理scanout操作不要。表示bring-upの代替証拠にはしない |

## VK_EXT_swapchain_maintenance1（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkReleaseSwapchainImagesEXT` | 取得した未present画像をswapchainへ返し再取得可能にする | 必要なら取得/画像参照状態を更新。画像resourceそのものは破棄しない |

## VK_KHR_swapchain_maintenance1（1関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkReleaseSwapchainImagesKHR` | 取得した未present画像をswapchainへ返し再取得可能にする | 必要なら取得/画像参照状態を更新。画像resourceそのものは破棄しない |

## VK_EXT_acquire_drm_display（2関数）

| vk API | U：libvulkan.so側で実装する部分 | K：GPUドライバ側に必要な機能 |
| --- | --- | --- |
| `vkAcquireDrmDisplayEXT` | DRM fdからdisplay制御を取得するAPI。DRMなし案では公開しない | DRM互換fd/connector意味論が必要。独自fdをそのまま渡す設計にはしない（DRMなし案では対象外、比較用） |
| `vkGetDrmDisplayEXT` | DRM fd/connector IDをVkDisplayへ対応付け。DRMなし案では公開しない | DRM互換connector識別が必要（DRMなし案では対象外、比較用） |

## virtio-gpuでの実装順序への反映

1. 2D出力では、device/capability取得、resource/backing確保・mapping、scanout・転送/flush、完了通知、close時回収を先に確認する。この段階はVulkan APIの実装完了ではない。
2. 描画にはVenus等のbackend context/capset、command transport、共有メモリ、同期を追加する。初期backendにないsparse/device group/外部共有等を先に作り込まない。
3. WSIはguestのdisplay/plane情報とpresentable imageを接続する。host Vulkanのsurface/swapchainをそのままguest画面と同一視しない。
4. transport応答、GPU実行完了、presentation進行、image再使用可能時点を分ける。virtio-gpuのfence応答だけでvblank/first-pixel-out/present-waitの意味論を満たしたとしない。FIFO等のpresent modeを含め、backendが供給しないtimingはWSIで実現可能かを検討し、適合を確認するまでadvertiseしない。

ioctlの粒度はこの表の関数数から決めない。device/context、resource/mapping、transfer/submit、sync/wait、display/presentの共通操作へ束ねる案と、必要な一部をVk操作へ近づける案をp001で比較する。共有ring/通知にする部分も含め、まだ公開ABIとして固定しない。

## 検証範囲

固定したXMLの対象関数集合とMarkdownの関数行を照合し、欠落・重複・空欄がないことを確認する。これは資料の網羅性確認であり、driver動作、Vulkan conformance、QEMU表示の検証ではない。実装Queueは開始していない。
