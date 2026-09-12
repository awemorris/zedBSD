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

## K：struct drv_gpu_opsの関数ポインタ案

上の275関数のK欄を、カーネルが提供する共通操作へまとめた案。操作は`struct drv_gpu_ops`の関数ポインタメンバーとして定義する案で、GPU操作を個々の`drv_gpu_*`グローバル関数にはしない。device登録・解除は`drv_gpu_register()`/`drv_gpu_unregister()`で行う。具体driverの実装関数はstaticとし、メンバーへ設定する。既存実装や確定済みヘッダーではない。ユーザー空間がカーネル関数を直接リンクして呼ぶ意味ではなく、`/dev/gpu0`のopen/close/ioctl/mmap/poll等の入口から呼び出す想定。GPUサブシステムがVFS入口と共通検証を持ち、登録されたinterfaceを通じてdriverを呼ぶ。ioctl番号、構造体の実際のレイアウト、既存device/VFSフックへの適合は次の設計事項とする。

**この節はすべてKのcallback候補。** Vulkanのinstance、pipeline、descriptor、command buffer等のユーザー側管理は前の表のUに残す。Vulkanと同じ個数のioctlを作るのではなく、必要な操作をこの単位で提供する。

### 共通の引数・戻り値規約

- 以下は簡略化したC風シグネチャ。`s`は`struct drv_gpu_session *`で、openした接続の所有権・handle表・権限を保持するカーネル内オブジェクト。ユーザーがそのアドレスを指定することはできない。
- 戻り値は原則`int`のカーネルエラーコード（成功0）、値は`out_*`へ返す案。`VkResult`への変換はUが行う。エラー番号・符号は現行カーネルの規約へ合わせ、ここでは固定しない。`close`だけは後始末の完遂を責務とする`void`案。
- `*_id`はsession所有の型付きhandleを表す固定幅整数の案。generation等で古いhandleの再利用を検出する。CPU物理アドレス、kernel pointer、host Vkハンドルとは別物。
- `req`/`out`は下の表で要素を示す要求・結果構造体。内部関数へ渡す時点では、入口処理でユーザーからコピー・検証済みのカーネル領域。wire形式ではsize/version、固定幅整数、配列のcount/offsetを使い、生のポインタやpNextを送らない。
- 配列照会は`capacity`と実際の`count`を分け、不足容量時の再照会を可能にする。入力・出力の長さ、個数の上限、整数overflow、handle所有権を入口と該当操作で検証する。
- `timeout_ns`は非負の相対時間、0はpoll。無期限待機は別flagとし、device lost・接続終了時に待機者を起こす。clockとtimeout精度はABI詳細で固定する。
- 非同期操作の成功は受付成功。結果の`completion_id`には完了の種類を持たせる。transport受付/応答、GPU実行完了、表示進行、画像再使用可能を相互に代用しない。

### 接続・能力・実行context

| drv_gpu_opsのメンバー案 | 入力 → 出力 | ドライバが実装する責務 | 対応する主なvk API／利用箇所 |
| --- | --- | --- | --- |
| `int (*open)(device, credentials, out_session)` | device・呼出元資格 → session | 接続とhandle表を作成し、render/display権限を分ける。VFS openから利用 | `vkCreateDevice`、physical device列挙の接続 |
| `void (*close)(s)` | session → なし | 新規操作を止め、waitを解除。進行中処理とscanoutの参照を保護し、所有資源・表示権を回収 | `vkDestroyDevice`、プロセス終了 |
| `int (*get_info)(s, query, out_info)` | query種別・容量 → version、device ID、heap、queue、制限、状態等 | Kが所有する実情報とdriver能力を返す。Vulkan feature全体の合成・対応宣言はU | `vkGetPhysicalDevice*`、`vkEnumeratePhysicalDevices` |
| `int (*get_capset)(s, req, out_capset)` | capset ID/version・容量 → 対応capsetデータ | virtioのprotocol/backend能力を照会。表示能力とは分ける | Venus選択、device拡張・feature照合 |
| `int (*context_create)(s, req, out_context_id)` | backend/protocol、flags → context handle | 隔離された実行/転送contextを作成。未対応protocolを拒否 | `vkCreateDevice` |
| `int (*context_destroy)(s, context_id)` | context handle → 状態 | context受付を終了し、進行中参照を処理して回収 | `vkDestroyDevice` |
| `int (*queue_create)(s, req, out_queue_id)` | context、実行種別、要求flags → queue handle | scheduling/transportのqueueを確保。複数VkQueueとの対応はUと協議し固定 | `vkCreateDevice`、`vkGetDeviceQueue*`の準備 |
| `int (*queue_destroy)(s, queue_id)` | queue handle → 状態 | 新規submitを止め、未完了・待機者を解決してqueue参照を解放 | `vkDestroyDevice` |

### resource・メモリ

buffer/imageの論理オブジェクトとVkDeviceMemoryの結合はUまたはhost backendが管理し、ここではKのbacking/resourceを扱う。`VkBuffer`、`VkImage`、Kのresourceを常に一対一とはしない。

| drv_gpu_opsのメンバー案 | 入力 → 出力 | ドライバが実装する責務 | 対応する主なvk API／利用箇所 |
| --- | --- | --- | --- |
| `int (*resource_create)(s, req, out_resource)` | size、backing種別、usage、必要なら2D format/extent/stride → resource ID・割当情報 | RAM/共有blob/2D resource等を確保。mapping/scanout可能性を検証し、過剰割当を防止 | `vkAllocateMemory`、swapchain作成、2D bring-up |
| `int (*resource_destroy)(s, resource_id)` | resource ID → 状態 | 公開handleを無効化。実際のbackingはGPU/scanout参照消滅後に回収 | `vkFreeMemory`、関連resource解放 |
| `int (*resource_get_info)(s, resource_id, out_info)` | resource ID → size、配置、commit量、mapping条件等 | 実際のK resource状態を返す。Vulkan画像layoutがhost所有ならその照会はtransport経由 | `vkGetDeviceMemoryCommitment`、memory/layout照会の一部 |
| `int (*mapping_create)(s, req, out_mapping)` | resource、offset/length、保護属性 → mapping token | mmap用の所有権付きtokenを発行。任意の物理範囲をmapさせない | `vkMapMemory*` |
| `int (*mapping_map)(s, token, vm_request, out_mapping_ref)` | token・VFS/VMからの要求 → mapping参照 | mmapフックでユーザーaddress spaceに適切なcache属性・保護を設定。ユーザーVAを決めるのはVM | `vkMapMemory*`、共有command領域 |
| `int (*mapping_destroy)(s, mapping_id)` | mapping ID → 状態 | 対応mapping/tokenを解除し参照を回収。実mapの寿命はVFS/VMと連携 | `vkUnmapMemory*`、プロセス終了 |
| `int (*resource_cache_sync)(s, req)` | resource、範囲、CPU→device/device→CPU → 状態 | 必要なcache/共有メモリ可視性処理。coherentなら合法なno-opも可能。GPU実行待機は別 | `vkFlushMappedMemoryRanges`、`vkInvalidateMappedMemoryRanges` |
| `int (*resource_bind)(s, req, out_binding_id)` | context、resource、範囲、用途、必要ならVA条件 → binding ID | contextへのresource attach、必要なGPU VA bindingと範囲検証。hostのVk bindとは区別 | `vkBind*Memory*`のK支援、submitで使うresource登録 |
| `int (*resource_unbind)(s, binding_id)` | binding ID → 状態 | 実行中参照と整合を保ってcontext/VA結合を解除 | resource/contextの後始末 |
| `int (*resource_get_address)(s, req, out_address)` | binding、address種別 → GPU/opaque address | KがVAを所有するbackendのみ。Venusでhostが所有するアドレスはhost照会で取得し、K resource IDと混同しない | `vkGetBufferDeviceAddress`、capture address系 |
| `int (*resource_transfer)(s, req, out_completion_id)` | resource、方向、矩形/範囲、offset/stride → transfer完了ID | 2D backingとhost resource間の転送・必要なflushを実行。host Vulkanの画像コピー全般とは区別 | 2D scanout準備、WSIコピー経路 |

### transport・実行・同期

`transport_send`はhost操作の転送、`submit`は実行とその依存関係の受付。実装上同じvirtqueue等を使っても、呼出側へ返す完了の意味は分ける。両者の処理を重複して送らない。

| drv_gpu_opsのメンバー案 | 入力 → 出力 | ドライバが実装する責務 | 対応する主なvk API／利用箇所 |
| --- | --- | --- | --- |
| `int (*transport_send)(s, req, out_request_id)` | context、登録済みcommand/reply resourceの範囲、protocol → request ID | Venus等のhost操作を送信。外枠・resource参照を検証しhost応答と対応付ける。GPU実行完了は保証しない | pipeline/descriptor/query等のhost処理、`vkCmd*`記録の転送 |
| `int (*transport_receive)(s, req, out_reply)` | request ID、容量、timeout → 応答状態・返信長 | 応答の到着とサイズを確認し、指定reply領域/結果を返す。blocking/非blockingを区別 | host能力照会、object作成結果、cache/queryデータ取得 |
| `int (*submit)(s, req, out_completion_id)` | queue、command範囲、resource参照、wait/signal依存 → GPU完了ID | 参照を保持して実行要求を送る。hostでの実行完了を追跡しdevice lostを通知。Vk submitをUがencodeする場合はそのpayloadを一度だけ送信 | `vkQueueSubmit*`、記録済み`vkCmd*`の実行 |
| `int (*queue_wait_idle)(s, queue_id, timeout_ns)` | queue、timeout → 状態 | そのqueueの受付済みGPU処理の完了を待つ。device全体の待機はUがqueueを集約可能 | `vkQueueWaitIdle`、`vkDeviceWaitIdle` |
| `int (*sync_create)(s, req, out_sync_id)` | binary/timeline、初期状態 → sync ID | K待機・GPU/host連携が必要な同期オブジェクトを作成 | fence/semaphore作成、必要なevent連携 |
| `int (*sync_destroy)(s, sync_id)` | sync ID → 状態 | handleを無効化し、進行中submit・waitの参照を安全に解決 | fence/semaphore/eventの解放 |
| `int (*sync_get)(s, sync_id, out_state)` | sync ID → 通知状態・timeline値 | K/hostで観測した実行同期状態を返す | `vkGetFenceStatus`、`vkGetSemaphoreCounterValue`、event状態 |
| `int (*sync_wait)(s, req, out_result)` | sync/value配列、all/any、timeout → 到達結果 | 同期条件の待機・起床、timeout/device lostを処理 | `vkWaitForFences`、`vkWaitSemaphores` |
| `int (*sync_signal)(s, req)` | sync ID、値 → 状態 | host側からsignal可能な同期だけを更新。GPU完了を表すfenceの偽造signalは許可しない | `vkSignalSemaphore`、host event set |
| `int (*sync_reset)(s, req)` | sync ID配列 → 状態 | reset可能な同期状態を更新。timelineを任意に巻き戻さない | `vkResetFences`、host event reset |
| `int (*completion_wait)(s, req, out_result)` | completion/request ID、要求する完了種別、timeout → 結果 | transfer、GPU、display等を型付きで待機。未対応の表示観測をGPU fenceで代用しない | transport・transfer・submit・presentの共通待機 |

VenusのVkFence/VkSemaphoreをそのままK同期へ一対一に移すことは前提にしない。U/hostの同期とKの完了通知を結び付ける必要がある場合だけ上記sync操作を使い、対応表・通知の順序・循環待機防止をprotocol設計で定める。`vkCmdSetEvent`等のGPU命令をhost側`sync_signal`で代替しない。

### display・present・通知

| drv_gpu_opsのメンバー案 | 入力 → 出力 | ドライバが実装する責務 | 対応する主なvk API／利用箇所 |
| --- | --- | --- | --- |
| `int (*display_get_info)(s, req, out_info)` | display/plane/modeの照会種別、ID、容量 → 情報・世代 | display、mode、planeと組合せ制約を列挙。切断・再接続で古いIDを検出 | `vkGet*Display*`、`vkGet*Surface*`、device-group present照会 |
| `int (*display_acquire)(s, req, out_lease_id)` | display ID、制御要求 → lease ID | display制御の排他的所有権を許可。render権限だけでは取得不可 | direct-display WSIの制御取得（Vulkan関数との一対一対応なし） |
| `int (*display_release)(s, lease_id)` | lease ID → 状態 | 所有権を返却し、必要ならconsole/bootfb providerへ復帰 | `vkReleaseDisplayEXT`、close |
| `int (*display_test)(s, req, out_constraints)` | lease/display、mode/plane、format/extent/配置 → 可否・制約 | 表示構成が実現可能か確認。実表示は変更しない | `vkCreateDisplayModeKHR`、surface/swapchain作成前の確認 |
| `int (*display_present)(s, req, out_present)` | lease、resource/binding、mode/plane、領域、render待機依存 → present ID・画像解放ID | render依存とbuffer可視性を満たし、必要なmode変更とscanoutを行う。古い画像が再使用可能になった時点を通知 | `vkQueuePresentKHR`、`vkAcquireNextImage*`の画像寿命連携 |
| `int (*display_get_progress)(s, req, out_progress)` | present ID/counter種別 → 対応する進行状態・counter | backendが実際に観測できるdisplay完了・counterだけを返す | `vkWaitForPresent*`、`vkGetSwapchainCounterEXT` |
| `int (*display_set_power)(s, req)` | lease/display、電源状態 → 状態 | 対応する出力の電源制御。未対応backendでは機能を公開しない | `vkDisplayPowerControlEXT` |
| `int (*event_subscribe)(s, req, out_subscription_id)` | 対象device/display、event mask → subscription ID | hotplug/device lost/実表示event等の購読を登録。アクセス権とbackend対応を検証 | `vkRegisterDeviceEventEXT`、`vkRegisterDisplayEventEXT` |
| `int (*event_unsubscribe)(s, subscription_id)` | subscription ID → 状態 | 購読と参照を解放 | event監視終了、close |
| `int (*event_read)(s, req, out_events)` | 容量、非blocking指定 → event配列・sequence | read/ioctl等からeventを取得。欠落・overflowを検出可能にし再照会へ誘導 | WSIの状態更新、fence通知との連携 |
| `int (*poll)(s, requested_events, out_ready)` | VFS poll要求 → ready mask | event/応答の読み出し可能性・device lost等を通知。GPU完了そのものとは区別 | Uの待機ループ、VFS poll |

display presentでは、受付、画像解放、表示進行を別々に扱う。`vkAcquireNextImage*`が必要とする画像再使用可能性を、単なるhost応答で判断しない。`vkCreateSharedSwapchainsKHR`等で複数出力を支える場合は複数plane/outputを一要求で表現できるかと原子性を追加設計する。初期virtio-gpuで証明できないpresent mode・counter・電源拡張はadvertiseしない。

### 機能を選択した場合だけ追加するKインタフェース

| drv_gpu_opsのメンバー案 | 入力 → 出力 | ドライバが実装する責務 | 対応する主なvk API／条件 |
| --- | --- | --- | --- |
| `int (*sparse_bind)(s, req, out_completion_id)` | queue、resource範囲と疎なbinding配列、同期依存 → 完了ID | sparse mappingの検証・更新・同期。Venusではhost処理とK backingの責務を分ける | `vkQueueBindSparse`。sparse対応を選択した場合のみ |
| `int (*resource_export)(s, req, out_share_handle)` | resource、範囲、権利 → 移譲可能handle | 他接続/プロセスへの共有権限を制限 | 外部memory拡張等。現在の275関数の対象外、将来候補 |
| `int (*resource_import)(s, req, out_resource_id)` | share handle、要求権利 → 当sessionのresource ID | 型・権利を確認して共有参照を作成 | 外部memory拡張等。現在の275関数の対象外、将来候補 |

GPU reset、割込み処理、PCI attach/detach、DMA map/unmapはこれらを支えるドライバ内部処理であり、ユーザー向け操作として無条件には公開しない。device lost時の復旧は全sessionの隔離・参照回収と整合させる。既存HALの責務変更はこの表では行わない。

### 初期bring-upで具体化する範囲

最初はopen/close、info、resource/mapping/cache、2D transfer、display情報・所有権・present、必要な完了/event通知を具体化する。Venusのcontext/transport/submit/sync群は描画経路の段階で詳細化する。capabilityを公開する場合に必要な機能と、後続の任意機能を区別し、全関数を最初から実装する計画にはしない。

この表はKの操作候補を一覧化したもの。正式なC型、ioctl構造体・サイズ・番号、同期state machine、2D/Venusの具体的なprotocol対応は未確定。既存275関数のU/K表と本節の対応をレビューしてからp001のABI設計を確定する。

## interface構造体とPCI経由の登録

2026-09-12ユーザー判断: GPU操作を`struct drv_gpu_ops`の関数ポインタにまとめ、GPUサブシステムへ登録する。PCI接続GPUの登録開始・解除はPCI側のライフサイクルから行う。個別GPU driverが独立した初期化経路からGPUサブシステムへ直接登録する方式にはしない。

### 構造体の形

以下は抜粋した型の模式図。完全な定義は上の44メンバー表をもとに作成し、ここではABIを固定しない。

```c
struct drv_gpu_ops {
    uint32_t version;
    uint32_t size;
    int (*open)(struct drv_gpu_device *, const struct credentials *,
        struct drv_gpu_session **);
    void (*close)(struct drv_gpu_session *);
    int (*get_info)(struct drv_gpu_session *,
        const struct gpu_info_request *, struct gpu_info_result *);
    int (*resource_create)(struct drv_gpu_session *,
        const struct gpu_resource_request *, struct gpu_resource_result *);
    int (*submit)(struct drv_gpu_session *,
        const struct gpu_submit_request *, struct gpu_submit_result *);
    int (*display_present)(struct drv_gpu_session *,
        const struct gpu_present_request *, struct gpu_present_result *);
    /* 残るメンバーは上表参照。型名も仮称。 */
};
```

driverは`static const struct drv_gpu_ops`を定義し、`.submit = virtio_gpu_submit`等のstatic実装関数を設定する。GPUごとの可変状態はinterfaceへ格納せず、登録するdevice instanceのprivate dataに保持する。複数のGPU instanceで同じimmutableなinterfaceを共有できる。sessionから所属device/private dataを参照できるようにする。

これはカーネル内のdriver contractであり、ユーザー向けioctl ABIではない。ユーザー空間へ関数ポインタを渡さない。`version/size`はdriver contractの整合確認用で、wire ABIのversionとは別。必須callbackとcapabilityを対応付け、任意callbackがNULLなら該当機能を公開しない。GPUコアが提供できる共通処理はコアで実装し、個別driverへhandle表やVFS処理の重複実装を要求しない。

### 登録・公開・解除の責務

| 担当 | 操作と引き渡すもの | 責務 |
| --- | --- | --- |
| 個別GPU driver | PCI driver descriptor、staticなGPU interface、deviceごとの初期化結果 | PCI ID matchとハードウェア初期化を提供し、初期化済みinstanceとinterfaceをPCI側へ引き渡す。GPU登録を別のグローバル初期化から開始しない |
| PCIサブシステム側の連携処理 | attach成功後にdriver instanceのops/private dataを取得 | `ops + private data`を`drv_gpu_register()`へ渡し、返されたdevice handleを保持する。GPUコアへPCI型は渡さない。登録契機とrollback/detach順序を所有する |
| GPUサブシステム | ops検証、device instance生成 | interfaceのversion/必須callbackを検証し、安定したinstance参照と`/dev/gpuN`を公開する。VFS/権限/handle管理を共通化し、必要な操作をcallbackへdispatchする |
| PCIサブシステム側の連携処理 | detachまたは登録失敗 | GPUコアへ停止・unregisterを依頼する。新規open/submit停止、既存参照の処理、DMA停止とdevice資源解放の順序を調整する |
| GPUコアと個別driver | unregister/quiesce/最終回収 | device lostを通知しwaitを解除、使用中callbackとsession参照を安全に処理する。interface/private dataを利用中に破棄しない。物理切断時も無効MMIOへアクセスしない |

順序案は `PCI match → driver attach/初期化 → PCI側がGPU登録 → /dev/gpuN公開`。公開前の失敗はGPU登録とハードウェア初期化を巻き戻す。detachでは公開停止・処理停止を先に行い、使用中参照とDMAの安全を確保してからBAR/IRQ/private dataを解放する。

q305ではPCI側が既存の汎用service callbackから通常のGPU register/unregisterを呼ぶ方式に改める。GPUコアの公開ヘッダはPCIへ依存せず、GPU専用service tableとregistration wrapperを持たない。非PCI driverも同じops/登録APIを利用できる。device台数はGPUコアと共通cdev/devfsの動的registryで扱い、VFS mount時に既存登録を消さない。

## p002の実装済み機能とq305の登録契約

44 callbackの表は将来機能を含む案のまま保持する。p002で実装したversion 1は次の5 member。完全なcontract・所有権・検証結果は[p002本文](https://github.com/awemorris/zedBSD/issues/383)に掲載する。

| member | p002での実装境界 |
| --- | --- |
| open / close | 必須。open descriptionごとのbackend session。最終closeで全資源を回収 |
| get_info | 必須。device情報とresource上限。coreがversion/size/capabilityを確定 |
| resource_create / resource_destroy | capabilityと対で任意。session所有の世代handle、失敗rollback、close cleanup |

q305の公開APIは`drv_gpu_register(ops, private_data, **device)`と`drv_gpu_unregister(device)`。PCI側の通常service callbackがこれらを呼び、hardware detach前に解除する。使用中は非公開化後EBUSYとしてhandleとhardwareを保持する。mmap/submit/fence/display/Venusはp003への不足で未実装。p001全体はplanningのまま。

## p003の実装済みGPU契約（q306 / 2026-09-12）

この節は [GPU内部ヘッダー](../../include/drivers/gpu.h)、[GPU UAPIヘッダー](../../include/uapi/gpu.h)、[GPUコア](../../src/drivers/gpu/gpu.c) にある現行実装の記録。前の275関数のU/K分担表と44 callback候補表の意味は変更しない。p002末尾の未実装項目はq305完了時点の記録であり、p003で具体化した範囲を以下に示す。実装の存在、ホストの単体試験、QEMUでの実描画確認は別の結果として扱い、p003の完了は実行証拠で判断する。

### versionと登録の互換性

| 境界 | 現在値 | 維持したもの／今回追加したもの |
| --- | --- | --- |
| K内の `struct drv_gpu_ops` | `DRV_GPU_INTERFACE_VERSION = 2`、`size = sizeof(struct drv_gpu_ops)`、`reserved = 0` | p002の5 memberに任意の6 memberを追加。version 1の関数表をversion 2として解釈せず、全in-tree driverを同じヘッダーでbuildする |
| U/K間の固定幅要求 | `GPU_ABI_VERSION = 1` | 既存の `GPU_GET_INFO`、`GPU_RESOURCE_CREATE`、`GPU_RESOURCE_DESTROY` と56/32/16 byteの各layoutを維持し、別番号の6 ioctlを追加 |
| device登録・解除 | `drv_gpu_register(ops, private_data, **device)` / `drv_gpu_unregister(device)` | 通常の動的登録、複数deviceによる不変ops共有、PCI非依存、EBUSY時の所有権維持はq305のまま |
| sessionのresource管理 | `GPU_SESSION_RESOURCE_MAX = 32` | STORAGEとBLOBが同じ32 slotを共有。coreが型・byte長・世代付きhandleを保持し、別sessionや破棄済みhandleを拒否 |

UAPIのversion 1維持は、K内の古い関数表も受け入れるという意味ではない。登録時は内部versionとsizeを厳密に確認する。各ioctlでも要求ごとのversionとsizeを厳密に確認し、未知ioctlや未対応機能は `EOPNOTSUPP`。任意機能の有無は `GPU_GET_INFO` のcapabilitiesから確認する。

### p003で追加した任意callback

以下の `device` は登録時の `private_data`、`session` は `open` が生成したbackend固有状態、`object` はcoreが所有handleから解決したbackend資源を表す。いずれもK内部のポインタであり、Uから指定しない。`int` の結果は成功0または正のerrno。正確な型はヘッダーを正とする。

| memberと型 | capability | K側の実装責務と返却時点 |
| --- | --- | --- |
| `int (*get_capset)(void *device, void *session, struct gpu_capset *request)` | `GPU_CAP_CAPSET = 2` | coreがゼロ初期化したinline領域へcapsetを返す。coreは返却bytesが要求capacity以下か確認し、version/size/selectorを元の要求に固定してcopyout |
| `int (*blob_create)(void *device, void *session, const struct gpu_blob_create *request, void **object, uint32_t *resource_id)` | `GPU_CAP_BLOB = 4` | sessionのblobを確保し、opaque objectと非zeroのprotocol resource IDを返す。coreが先に確保したhandleと結び付ける。確保失敗はbackendが回収し、返却copyout失敗はcoreが `resource_destroy` を呼ぶ |
| `int (*resource_read)(void *device, void *session, void *object, uint64_t offset, void *buffer, uint32_t bytes)` | `GPU_CAP_TRANSFER = 8` | 所有resourceからcoreのkernel bufferへcopyを完了して戻る。その後coreがUへcopyoutする。Uの生ポインタは受け取らない |
| `int (*resource_write)(void *device, void *session, void *object, uint64_t offset, const void *buffer, uint32_t bytes)` | `GPU_CAP_TRANSFER = 8` | coreがcopyin済みのkernel bufferを所有resourceへcopyし、入力bufferを使い終えてから戻る |
| `int (*command)(void *device, void *session, const void *buffer, uint32_t bytes)` | `GPU_CAP_COMMAND = 16` | 検証・copyin済みの有限command streamをbackendへ渡す。戻った後coreのbufferは破棄されるため、遅延使用に必要な内容はbackendが保持する。成功は受付を表し、Vulkan実行完了ではない |
| `int (*present)(void *device, void *session, void *object, const struct gpu_present *request)` | `GPU_CAP_PRESENT = 32` | core検証済みのSTORAGE画像についてbackendが表示所有権を調停し、scanoutへ提示する。blobの直接presentや汎用swapchainの画像解放通知を提供する契約ではない |

`open` / `close` / `get_info` は引き続き必須。`resource_create` は `GPU_CAP_RESOURCE = 1` と一致させる。`resource_destroy` はRESOURCEまたはBLOBのどちらかを公開する場合に必須となり、両方の資源に共通の最終回収callbackとして使う。TRANSFERはread/writeの両方が必要。各任意bitとcallbackの有無が一致しない関数表、未知のcapability bitは登録を拒否する。

coreはcallbackをspinlock内で呼ばず、同じopen descriptionでは1 ioctlだけを受け入れる。別session間の並行実行はbackendが共有状態を保護する。明示破棄、返却失敗のrollback、最後のcloseで同じ資源回収経路を使い、全resource_destroy後にcloseを呼ぶ。backendがtimeout後のDMAを保持する場合も、その所有権はbackendに残し、reset確認前に再利用・解放しない。

### p003で追加したUAPI

全要求はUが `version = GPU_ABI_VERSION` と自身の正確な `size` を設定する。`address` は64 bit整数として符号化したUのbuffer位置であり、Kが表現幅、末尾のoverflow、copyin/copyout時の領域と権限を検証する。callbackへ渡すのはKのcopyのみ。以下のsizeとioctl番号はILP32/LP64共通。

| ioctl | 要求型・size・番号 | Uが指定する内容／受け取るもの | Kの共通検証と権利 |
| --- | --- | --- | --- |
| `GPU_GET_CAPSET` | `struct gpu_capset`、280 byte、`0xc1184703` | capset_id、capset_version、capacity → bytesと `data[256]` | capacityは1..256、入力bytesは0。inline出力をゼロ初期化し、backendの返却長を確認 |
| `GPU_BLOB_CREATE` | `struct gpu_blob_create`、40 byte、`0xc0284704` | bytes、blob_id、flags → handleとresource_id | open時の書込み権利、device上限、空きslot、入力handle/resource_id=0。flagsは `GPU_BLOB_MAPPABLE` 以外のbitを拒否。返却失敗はrollback |
| `GPU_RESOURCE_READ` | `struct gpu_transfer`、40 byte、`0x80284705` | handle、offset、address、bytes。address先へ読み出す | open時の読出し権利、reserved=0、bytesは1..65536、同じsessionのhandle、資源内の全範囲を確認 |
| `GPU_RESOURCE_WRITE` | `struct gpu_transfer`、40 byte、`0x80284706` | handle、offset、address、bytes。address先から書き込む | open時の書込み権利とREADと同じ長さ・所有権・範囲検証。copyin失敗時はbackendを呼ばない |
| `GPU_COMMAND` | `struct gpu_command`、24 byte、`0x80184707` | address、bytes、flags=0 | open時の書込み権利、bytesは4..65536かつ4の倍数。KはVulkan構造体やVkResultを解釈せず、有限の転送bufferを渡す |
| `GPU_PRESENT` | `struct gpu_present`、48 byte、`0x80304708` | handle、offset、width、height、stride、format、frame | open時の書込み権利。width/heightは1..4096、strideはwidth×4以上、formatはBGRA8888=1またはRGBA8888=2。同じsessionのSTORAGEだけを認め、offsetからstride×height全体が資源内か確認 |

`GPU_RESOURCE_READ` のioctl自体は `_IOW`。固定要求をUからKへ渡し、実データは要求のaddress先へ別のcopyoutで返すためである。BLOBの破棄にも既存 `GPU_RESOURCE_DESTROY` を使い、新しいdestroy ioctlは増やさない。初期のroot限定open、open時の権利を後から拡張しない制約、切断時の `ENODEV` とpollの `POLLERR/POLLHUP` は維持する。`frame` は試行識別用の値であり、fenceや表示完了counterではない。

### この契約を使うVenusの実装範囲

Uの `venus-frame` がVulkan object、wire format 1のcommand、返信、VkResultとVkFenceを扱う。KのVenus backendはPCI、virtqueue、openごとのrenderer context、capset 4、HOST3D blob、copy転送とscanoutを扱う。返信用blob_id=0とVkDeviceMemoryの非zero blob_idは区別し、Kのopaque handle、protocol resource ID、UのVk object IDも別の名前空間として扱う。

今回の `command` は候補表のtransportを具体化した有限転送口であり、Vulkan関数ごとのioctlを設けたものではない。virtqueue受付、Venus返信、Vulkan fence完了、実画面の取得を別々に確認する。Uは返信末尾のmarkerを別readで確認してから本文を読み、Vulkan fence完了後にreadback blobの画素を取り出す。検証した同じ画素をSTORAGEへcopyしてpresentする。

実装対象は256×192の2帯をVulkan clear/copyで生成する独立クライアントと、その表示・回収経路。汎用 `libvulkan.so` / ICD、全Vulkan API、Vulkan WSI拡張、device mmap、zero-copy swapchain、汎用sync object、vblank/present完了通知、cursor/hotplug、native i915はこの実装済み一覧に含めない。44 callback案の `submit` / `display_present` / event群などがすべて現行ヘッダーに入ったという意味でもない。

transportと観測の詳細は [Venus transport資料](venus-transport.md)、実行手順は [リモート検証README](tests/README-venus-remote.md)、Uの対応commandと出典は [クライアントREADME](../../userland/gpu/venus/README.md) を参照する。QEMUでVulkan描画を確認できたかどうかはp003の試行記録を正とし、この契約表だけで完了を宣言しない。

## p005の3D描画経路と共有Uクライアント（q307）

`userland/base/vkdemo/` は自作のvertex/fragment shader、テクスチャ付き非等辺直方体、depth、時刻のpush constantを使う有限のgraphics client。p003のwire/reply/bootstrapを `userland/gpu/venus/client.[ch]` に共通化し、caller所有のsession構造体へ通信状態を保持する。scene、pipeline、descriptor、画像・buffer・fenceの寿命とframeループは各アプリが所有する。`venus-frame` もこの共通clientを使う。

[追加APIの表](phase005/api-coverage.md) はformat照会、shader module、graphics pipeline、image view/sampler、descriptor、renderpass/framebuffer、vertex/descriptor binding、draw、push constant、texture upload、fence/pool再利用を記録する。これらのVulkan状態と符号化はUに属する。Kは既存のcapset/blob/read/write/command/presentを提供する。GPU ioctlやHALの追加はこのデモの実装前提にしていない。

API表の275関数、44callback候補を、今回の有限clientがすべて実装したという意味へ変更しない。対象rendererのwire commandを直接符号化するクライアントであり、汎用libvulkan.so/loader/ICDの公開関数提供は後続課題。現行UAPI v1、K内部ops v2、通常の動的register/unregisterは維持する。実測したAPI不足・画像結果・完了判定は [p005](phase005/phase.md) とその結果を正とする。
