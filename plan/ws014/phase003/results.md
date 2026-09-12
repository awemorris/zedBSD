# WS014 p003 / q306 実行結果

2026-09-13 JST。p003の有限受け入れを達成。GPU core、`src/drivers/gpu/venus/` の専用backend、独立した `venus-frame` とリモートQEMUループを実装した。ソースと資料のgit add/commit/pushはユーザーが行う。GitHub Issueには本文・証拠の要点とhashを掲載し、未コミットのローカルファイルを公開済みリンクとして扱わない。

## 実画面の検証

| 試行 | 経路 / frame | 独立期待値 | 全49,152 RGB pixel | QEMU実行時間 |
| --- | --- | --- | --- | --- |
| q306-2d-005 | 2d / 1 | 赤・緑 | PASS | 3.642秒 |
| q306-2d-006 | 2d / 2 | 青・黄 | PASS | 3.692秒 |
| q306-venus-003 | venus / 1 | 赤・緑 | PASS | 4.133秒 |
| q306-venus-004 | venus / 2 | 青・黄 | PASS | 3.958秒 |

Vulkan試行ではVkImageをclearし、VkBufferへcopy、VkFenceのsignaledを確認後、HOST_VISIBLE|HOST_COHERENT memoryのblobをreadbackした。196,608 byteのRGBA全画素が一致（frame1 FNV-1a 60545dc5、frame2 25f95dc5）。同じデータを2D storageへcopyしてpresentし、QEMUのegl-headlessがGPU画像をreadbackしたVNC RAWフレームを別のRGB期待値で全件比較した。KはVulkan commandの内容を解釈しない。ホストだけのVulkanデモやsubmit成功を代用していない。

各試行は新しいQEMU process・使い捨てimage・OVMF vars・ログ・Unix socketを所有する。QEMU終了コード0と回収ログ/画像/hashの一致を確認した。PNGは保存した実PPMのRGBを変更せず可逆に形式変換したもの。

## 実環境

`awe@10.0.10.25` / Debian13.6、Linux6.19.13、Intel Iris Xe8086:46a8 / i915、Mesa ANV25.2.6 / Vulkan1.4.318、QEMU10.0.11、virglrenderer1.1.0。KVM、renderD128、udmabufとmemfd共有を使用。`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.json` でIntel Vulkan ICDを選択した。

ホストのlibvirglrenderer1には実行用serverが含まれていなかった。同版のDebian公式virgl-serverを専用作業ディレクトリへ展開し、RENDER_SERVER_EXEC_PATHで指定。system dpkg databaseは前後不変、依存解決とsocket起動確認を保存した。イメージを含むこの私有ホストへの転送はユーザー明示許可済み。

主なQEMU構成はKVM、1GiB memfd shared RAM、2CPU、OVMF、IDE使い捨てdisk、`virtio-vga-gl,venus=on,blob=on,hostmem=8M,max_outputs=1`、egl-headless/renderD128、QMPとVNCの専用Unix socket。正確な引数、firmware/server/client/imageのhashは各JSONに保存した。

## 実際に修正して再実行した問題

| 停止箇所 | 確認した原因 | 修正と後続証拠 |
| --- | --- | --- |
| 初期kernel link | 宣言済みamd64 MMIO accessorの実体なし | 8 accessorの具体差分を提示し、ユーザー許可後に同一差分を適用。最終差分をproposalと照合 |
| GPU登録なし | PCI capabilityの部分mapがwhole BAR再配置と不適合 | register BARを一度だけ全体mapし、checked viewを共有。実QEMUでcapset4登録 |
| renderer context初期化 | virgl_render_server欠落 | 専用rootへ公式同版packageを展開、環境path指定 |
| q306-2d-002/003/004のno surface | QEMU10はGL scanout時にQMP screendumpへsurfaceを返さない | QMPは制御/console取得、実frameはegl-headless→VNC Unix RAW。2D005/006で全画素一致 |
| q306-venus-001のENOMEM | 256MiB host-visible BARは既存amd64固定MMIO窓に入らず、不正な部分BAR再配置となる | hostmem8MiB、BAR全体一回mapとborrowed blob view。上限超過はmapping前に拒否 |
| q306-venus-002のcommand17 CS error | virglrenderer1.1.0はvkGetDeviceQueueを意図的に拒否 | vkGetDeviceQueue2+Venus timeline1へ変更、capset対応を確認。再build後venus003/004でfence/readback/実frame成功 |

初期bootstrap試行ではpayloadでないESPへの設定配置と通常TTYログの観測不足も判明した。wrapperはGPT CRCを検証し、vmunixを含む唯一のFATを選び、コピー内だけinit=/bin/shにする。埋込みkernelのhashとELFから求めたvt_history物理範囲を照合してQMP pmemsaveでconsoleを取得する。HAL/TTYに試験用のログ経路を追加していない。

この有限ループでは、実際の失敗ログ→コード修正→再build→別image転送→新規QEMU→実画面照合を実行した。修正前venus002と修正後venus003のclient/source/image hashは異なり、成功後はframe2の異なる色も再build・新規起動して確認した。

## 限定試験とレビュー

GPU coreの通常/ASan/UBSan試験、ILP32/LP64 UAPI layout・ioctl値、copy境界・権限・rollback・session所有権を確認。Venus backend/transportは本番ソースを有限peerへ接続し、shared BAR、blob、2session表示権、geometry、queue300回再利用、capabilitycycle、異常used descriptor、timeout中DMA保持、失敗resetと再cleanupを確認した。compiler analyzer、host C89/guest-header syntax、amd64 kernel/image buildとvmunix checker、git diff --checkも通過。C規約全文に対する変更部分のレビューを行った。

RFBは8つの有限fake Unix peer試験を通過し、分割受信、RGB変換、DesktopSize/LastRect、全画素coverage、範囲・名前・encoding不正、途中EOF、全体timeoutを確認した。再現コマンドはtests/README.mdとtests/README-venus-remote.mdにある。aggregate make checkは実行していない。

## 現行APIと範囲

通常のdrv_gpu_register/unregisterを維持し、PCI側の通常serviceがVenus opsを登録する。内部drv_gpu_opsはversion2、U/K固定ABIはversion1と既存3ioctlのlayoutを保持。任意のget_capset/blob_create/resource_read/resource_write/command/presentを追加した。275関数U/K表を保持したまま、同じ資料へ6callback/6ioctl、権限・上限・所有権・完了の意味を追記した。

この成果は限定Vulkan clear/copy/fenceと表示のデバッグ基盤。一般のlibvulkan.so/ICD、全Vulkan適合、汎用WSI/mmap/zero-copy、vblank精度、native i915を実装した意味ではない。現行amd64 MMIO窓に合わせhost-visible apertureは8MiB以下、単一scanoutの表示はsession所有。native i915は別WS029、最終API整理と規約確認のp004はこのQueueで実行しない。p001の未決定を自動clearせず、WS014はincompleteを維持する。

HALについては既存宣言の補完を含む全改変に事前許可が必要。今回の具体的な8 accessor許可を他のHAL変更へ一般化しない。

## 保存証拠のhash

### q306-2d-005

- image_sha256: `82bac50b6e4cf6944b7b27dd8f5c5eb2033d593545119f9fb8eb1fdb60c80653`
- kernel_sha256: `3a2dd491dd69dd1e27298eab563717c7d81fec78dbab98d3587b33bfc6fc9821`
- client_sha256: `3d73e9dbf6018eb2c38d544a56e8a58bcd62052ec2c3c31c1fb8c57045f66534`
- sources_sha256: `604ae122d36c54677d03dda9993f72f8a1991aeb167d3d719827047e7ae3eac1`
- ppm_sha256: `d1a5058dc4e70dd175ef1e8007d90321ff0a32ad94d8542bc119a26e59ea7d5a`
- png_sha256: `1d0a6f6d2bde5385a78aae2446ffbd837ce4651e8721b2f5fd7366f4b1a76066`

### q306-2d-006

- image_sha256: `0724e40847602424a5d12208e977ef8e6eedc0691f25f61a99c7fbc18ae85a97`
- kernel_sha256: `3a2dd491dd69dd1e27298eab563717c7d81fec78dbab98d3587b33bfc6fc9821`
- client_sha256: `75ae9dad7b630d582c0ad69a07fcf330063601dc226d6b87c5184d369529bd03`
- sources_sha256: `5fd4b9a92e02b1fc5090dedcb523adca3565ee6500e00110c267a31f88c34fee`
- ppm_sha256: `37f8e1718827f82188f76e644f0928ed78c5b6ae3318db9fb736bd0de0292503`
- png_sha256: `b7659e25ef0993a35e0dd530764727e76c1ee170c7350bfe41ba51ec6900f7df`

### q306-venus-003

- image_sha256: `6ef1d9dd24f9baa91f1de46759584e7871c1e280877426b972b1cc2540d70cee`
- kernel_sha256: `3a2dd491dd69dd1e27298eab563717c7d81fec78dbab98d3587b33bfc6fc9821`
- client_sha256: `75ae9dad7b630d582c0ad69a07fcf330063601dc226d6b87c5184d369529bd03`
- sources_sha256: `5fd4b9a92e02b1fc5090dedcb523adca3565ee6500e00110c267a31f88c34fee`
- ppm_sha256: `d1a5058dc4e70dd175ef1e8007d90321ff0a32ad94d8542bc119a26e59ea7d5a`
- png_sha256: `1d0a6f6d2bde5385a78aae2446ffbd837ce4651e8721b2f5fd7366f4b1a76066`

### q306-venus-004

- image_sha256: `bc504be97f016d0ac87a06ec5560679e8a2d4ad7f5c62d2b389b29e41d139989`
- kernel_sha256: `3a2dd491dd69dd1e27298eab563717c7d81fec78dbab98d3587b33bfc6fc9821`
- client_sha256: `75ae9dad7b630d582c0ad69a07fcf330063601dc226d6b87c5184d369529bd03`
- sources_sha256: `5fd4b9a92e02b1fc5090dedcb523adca3565ee6500e00110c267a31f88c34fee`
- ppm_sha256: `37f8e1718827f82188f76e644f0928ed78c5b6ae3318db9fb736bd0de0292503`
- png_sha256: `b7659e25ef0993a35e0dd530764727e76c1ee170c7350bfe41ba51ec6900f7df`

完全な実行JSON・console/renderer/guest/QMPログ・PNGはこのディレクトリのevidence/配下。元のbuild/transferログとPPMはplan/ws014/temp/remote/配下。GitHub本文へは結果とhashを同期し、ファイル公開はユーザーのcommit/push後となる。

最終のコメント6箇所修正後に同じ設定で再buildした。カーネルとvenus-frameは実測済みvenus004とバイト単位で同一。final-source-build.jsonに現行source hash・build引数・バイナリ一致を保存したため、追加の同一VM試験は行わない。
