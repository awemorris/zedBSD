# WS014 p003: Venus transport and capture contract

2026-09-12 / q306。実装と実測に合わせて更新中。通常のGPUデバイス登録APIを使い、ハードウェア側は `src/drivers/gpu/venus/` に閉じる。

## U/Kの分担

UはVulkanのinstance/device/image/buffer/command/fence、Venusのwire形式と返信結果を扱う。KはPCI/virtqueue、context、capset、blob、検証済みバッファの転送、resource所有権、scanoutを扱う。KはVulkan commandを解釈しない。

初期クライアントは独立実装の `venus-frame`。限定したVulkan commandで最適配置の画像をclearし、coherentなbufferへcopyしてVkFence完了を待つ。VkDeviceMemoryをHOST3D blobとして読み出し、実際のRGBA画素を全件検証して2D resourceへ転送・表示する。一般的なlibvulkan.so、全Vulkan適合、直接scanoutする高速WSI、device mmapを提供した意味ではない。

## GPU API

既存GPU_ABI_VERSION=1と従来3ioctlのlayoutを保持する。kernel内drv_gpu_opsはmember追加のためversion2へ変更し、全in-tree利用側を同時にbuildする。通常のregister/unregisterは維持する。 追加callbackとioctlの正確な型・番号・検証境界は [Vulkan責務資料のp003追記](vulkan-api-responsibilities.md#p003の実装済みgpu契約q306--2026-09-12) を参照する。

| 追加操作 | Kの責務 | Uへ返すもの |
| --- | --- | --- |
| get_capset | 有界のcapset照会 | 256byte以内のprotocol能力 |
| blob_create | session所有のHOST3D blob確保・map、失敗rollback | opaque handleとsession内protocol resource ID |
| resource_read/write | handle・種別・範囲・権限確認、最大64KiBずつcopy | 読出しデータまたは転送結果 |
| command | 最大64KiBのkernel copyをSUBMIT_3Dへ送る | virtqueue受付結果。Vulkan完了とは別 |
| present | storage resourceの範囲/geometryと表示所有権を確認 | scanout/transfer/flush結果 |

返信用blob_id=0は各sessionに新しく確保し、非zero blob_idはそのsessionのVkDeviceMemoryを一度だけexportする。user pointerは固定幅address欄から検証・copyし、driverへ直接渡さない。初期表示は256x192、RGBA8888、frame1=赤/緑、frame2=青/黄の左右2帯。

## 完了と観測

QEMU10のVenusはrender serverへの非同期送信。virtqueue usedや旧式fenceを返信/Vulkan完了に読み替えない。Uは返信末尾にEnumerateInstanceVersionの結果を置き、markerを別readで確認した後に本文を読む。GPU完了はVkFenceのstatusで別途待つ。renderer未対応のQueueWaitIdle/DeviceWaitIdle/map-memory commandは送信しない。

remote harnessは起動ごとに新しいディレクトリ・使い捨てimage・OVMFvarsを生成する。Intel ICDを指定し、egl-headlessがreadbackした画面をQEMUのVNC Unix socketからRAW取得して、全画素で独立した期待色と照合する。QEMU10.0.11はGL scanout時にQMP screendumpへsurfaceを返さないため、QMPは起動制御・console/log取得に使う。kern_logはdebugcon、通常TTY出力は同じvmunixのELF symbolとLOAD segmentから得たvt_history物理アドレスをQMP pmemsaveで読み出して保存する。HALやTTYへ試験専用ログ経路を追加しない。symbol/実イメージのkernel hashを対応させ、以前のログや画像を再利用しない。

## 環境と実装中に判明した不足

ホストはユーザー指定のawe@10.0.10.25。Intel Iris Xe / i915、ANV25.2.6、QEMU10.0.11、virglrenderer1.1.0、Linux6.19.13。実環境preflight、QEMU起動、2D画素とVNC実画面の全件一致を確認済み。Vulkan描画の成否はPhaseの試行結果で追記する。

amd64にはhal.h宣言済みのMMIO accessorの実体がなく、初回Venus linkで未定義を検出した。src/hal/amd64/asm.cのread/write8/16/32/64補完について、ユーザーがHALの全改変には許可が必要と明示したため、一度追加を取り消し、amd64-mmio-proposal.patchで具体差分を提示した。その後ユーザーの明示許可が届き、2026-09-12に承認された差分を適用済み。公開宣言・責務を変えない補完も事前許可の対象であり、今回の許可を将来のHAL改変へ一般化しない。候補でのbuild成功、適用許可、承認後の実行検証は区別する。Vulkan描画の実行結果はp003の試行記録で確定する。

実測ホストにはlibvirglrenderer1 1.1.0-2のみ導入され、virgl-serverが欠落していた。Debian公式の同版を専用ディレクトリへ展開し、RENDER_SERVER_EXEC_PATHで指定する。システムのパッケージ状態は変えず、配布物・binaryのhashと起動確認を保存する。

現在のamd64 HALのdevice mappingは0xf0000000..0xf1000000の固定窓。p003はQEMUのhostmem=8Mを基準とし、Venus driverは8MiB以下のhost-visible BARを一度だけ全体mapして各blobへ範囲確認したviewを渡す。8MiBを超えるBARはmapping前に拒否する。register BARとhost-visible BARのmappingはtransportが所有し、reset確認後に一度だけ解放する。blob自身は予約したextentとhost側map/unmapを所有する。256MiBの部分mapでBAR全体を不正な境界へ再配置する経路は使わない。より大きいapertureへの一般化には既存HAL/PCI mappingの検討が必要で、別のHAL改変を今回の許可へ含めない。

ユーザーはOSイメージ転送と、この私有サーバーへの機密データ転送を明示的に許可した。初回の既存image転送は自動承認レビューが拒否し実行されなかった。許可後に使い捨てコピーの転送を再開した。

## 一次資料

- [Venus protocol wire仕様](https://gitlab.freedesktop.org/virgl/venus-protocol/-/blob/ca19b6358d7cc491bc3e4de76f04c6700876a8fa/docs/VK_EXT_command_serialization.txt)
- [virglrenderer 1.1.0](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/1aeaf5e10a9c89096e96d09599aa419d5c50712f)
- [QEMU virtio-gpu](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)

wire定義の参照と、上流実装のbase systemへの取り込みを区別する。クライアント実装・対応commandの出典詳細は `userland/gpu/venus/README.md`。

2026-09-13 JST 最終結果: q306で2DとVenusのframe1/2を実行し、GPU readbackとVNC実画面が全件一致。p003の有限受け入れを達成。詳細はphase003/results.mdと証跡JSON。一般的なVulkan実装・native i915・p004の実行とは区別する。
