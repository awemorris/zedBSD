<!-- awesome-plan project=zedbsd record=ws014-p003 -->

# WS014 p003: QEMU＋Venusの自動デバッグループ

<!-- awesome-plan-current:start -->
Status: cleared
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Last Queue: q306 / q306-i01 cleared
Active Queue: none
<!-- awesome-plan-current:end -->

## q306完了: p003 cleared（2026-09-13 JST）

Venus専用driver、GPU任意callback/UAPI、独立したVulkan clear/copy/fence/readbackクライアント、build→転送→新規QEMU→実画面照合の有限ループを実装・検証した。2D/Vulkanともframe1とframe2の全49,152RGB pixelが赤緑／青黄の独立期待値に一致。Vulkan fence完了とGPU readback全画素も確認した。最終sourceからのbuildは実測済みkernel/clientとバイト一致する。

QEMU10のGL scanoutはQMP screendumpで取得できないため、QMPは制御とconsole取得、画面はegl-headlessのreadbackをVNC Unix RAWで取得。hostmemは現行amd64 MMIO窓に合わせ8MiB。HALはユーザーが具体差分を許可した8accessorのみ変更した。

q306はfinished、q306-i01/p003はcleared、active Queueはなし。p002 cleared、p001/p004 planning、WS014 incompleteを維持。次はユーザーが追加したp005（テクスチャ付き回転直方体デモ）、その後p004。native i915は別WS029であり今回未実行。一般のlibvulkan.so、全Vulkan適合、汎用WSI/mmap/zero-copyは未実装。

詳細と実測hashは[p003](https://github.com/awemorris/zedBSD/issues/384)。local evidenceはplan/ws014/phase003/results.md、evidence/、queue履歴はplan/history/queue-q306.md。GitHubは計画・証拠本文を同期し、source/資料のgit add/commit/pushはユーザーが行う。未コミットのファイルを公開済みリンクとして扱わない。

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

<details>
<summary>計画・実行途中の履歴（旧停止条件は解消済み）</summary>

# WS014 p003: QEMU＋Venusの自動デバッグループ



## q306: Venus実装・リモートQEMUループ（ユーザー実行指示）

2026-09-12、ユーザーがp002をレビューし「これはOK」と受け入れ、p003完了までの環境確認・実装・QEMU実行を指示した。対象はユーザー提供の `awe@10.0.10.25`。実装は `src/drivers/gpu/venus/` に置き、Venus用のdriverとして構成する。汎用virtio-gpu driverへの抽象化を要求しない。q306 / q306-i01はこの単一Phaseだけを選択する。p002はclearedを維持し、ユーザーのcommit `3236b560` に取り込まれている。

環境確認済み: Debian 13.6 / Linux 6.19.13、Intel Iris Xe 8086:46a8 / i915、Mesa ANV 25.2.6 / Vulkan 1.4.318、QEMU 10.0.11、virglrenderer 1.1.0。KVM API 12、renderD128、udmabufの利用権、Venus/blob/hostmem/egl-headlessオプション、外部メモリ等の必要候補featureを確認。実際のVenus描画はこれから検証する。Intel ICDを指定し、ソフトウェアrendererの誤認を防ぐ。ホスト上の専用作業ディレクトリと使い捨てimageを使用する。

受け入れはzedBSDゲスト内の最小2DとVulkanテスト描画、frame更新のQMP取得・期待画像照合、変更→再build→再起動→新frame照合の再現可能なループ。serial/QMP/QEMU・rendererログ、source/image hash、起動引数、選択したGPUとversionを試行単位で保存する。ホスト単独のvkcubeやcommand提出ログだけではclearしない。全Vulkan適合・物理表示timingは受け入れ外。

既存PCI/DMAと通常のGPU ops登録を使用し、Venus内にPCI virtqueue、capset/context/blob、command/reply転送、完了確認、scanoutを実装する。必要なGPU callback/UAPIを実利用から追加する。初期経路はkernel所有のメモリと検証付きcopy ioctlを候補とし、ユーザー空間がVulkan command/応答を扱う。base systemは独立実装とする既存方針を維持し、上流実装を無断で取り込まない。対象subset・不足API・ownership/versionへの影響を資料へ記録する。

見積枠は240 active minutes、120分ごとに成果・境界を点検する。ユーザーは今回p003完了までの継続を指示済み。同じ失敗状態に対する無変更再試行は3回までとし、各起動・pollにtimeoutを設け、証拠に基づいて修正する。HAL責務/hal.hは変更せず、必要な判断が実際に発生した場合にのみ確認する。C規約全文、意味のある限定test、make -j16対象build、QEMU実測、差分レビューを適用する。p004・ネイティブi915は実行対象に追加しない。git add/commit/pushはユーザーが行う。

<details>
<summary>q305までの計画・引継ぎ（履歴、現行は上記q306）</summary>

Combined ID: `ws014-p003`
Primary Milestone: MG006

## 目標・依存

p002のGPUフレームワークにvirtio-gpuとユーザー空間Vulkan/Venus/WSIを接続し、Linux i915＋ANVホストのQEMUで画面取得→失敗切り分け→修正→再実行の有限ループを成立させる。p002の必要出力を確認してから開始する。hostの成立確認は前準備にできるが、guest framework接続の依存は逆転させない。

## 受け入れ

最小2DとVulkanテスト描画の画面・frame番号を取得し、古いframeを成功にしない。serial/QMP/rendererの証拠を試行IDで保存し、テスト画像またはコードの変更→再build/再起動→期待画像確認を再現可能にする。ホストのLinux vkcubeだけではclearしない。API不足を修正・整理して資料と実装の一致を確認する。対象version/featureと非対応範囲を明記し、全Vulkan適合は別の判断とする。

## 手順・制約・未決定

以下の資料を本Phaseの詳細手順とする。対象ホストの接続、GPU世代/version、backendと必要なユーザー空間移植量は未確認。Queue選択時に試行上限を定め、停止境界の証拠を残して再開可能にする。実表示timingの保証は今回の画面取得の受け入れ外。

# QEMU＋Venusの画面取得と自動デバッグ計画

2026-09-12 / WS014 p003の資料。ユーザーが計画への記録を指示した。まだホスト接続・動作確認・ループ実行はしていない。

## 構成と受け入れ

LinuxホストのIntel GPU（i915カーネルdriver＋ANVユーザー空間Vulkan driver）でQEMU＋virglrenderer/Venusを動かし、zedBSDのvirtio-gpuとVulkan/WSI表示経路を反復検証する。これはホストがi915を使う構成であって、zedBSDのi915実装ではない。後者は次の独立WSへ進む。

最初にhostのGPU PCI ID、kernel/i915、ANV/Mesa、QEMU、virglrenderer、Vulkan feature/拡張、render node権限、KVMと共有blob/mapping条件を確認し、実際に成立したversionと起動引数を固定する。i915の存在だけではVenus動作を保証しない。WSL2は今回の基準環境とせず、Linuxホストを使用する。

画面取得はQEMUのegl-headless＋QMP screendumpを第一候補とする。egl-headlessはscanout texture/DMA-BUFを取り込みCPU側surfaceへreadbackする経路を持つ。QMPでdevice/headを指定しPNGまたはPPMを保存する。実QEMU buildのEGL/GBM/Pixman/画像形式対応を確認する。これは公式実装から成立を見込んだ構成で、対象ホストで動作確認した事実ではない。

## 有限な反復手順

1. 最小2Dの色・図形・frame番号を変更し、captured imageも変わることを確認する。初期画面や古いsurfaceを撮っていないか、GPU/head、サイズ、上下方向、色順を確認する。
2. GPUフレームワークへvirtio-gpu backendを接続し、2D resource/backing/scanout/transfer/flushを動かす。
3. Venusのcontext/capset/blob/transport/submit/syncとユーザー空間Vulkan実装を接続する。既存Mesa/Venusを使う場合のDRM依存やguest WSI移植を含める。ホストでvkcubeが動くだけをzedBSDの成果としない。
4. zedBSDでテスト描画→シリアルのframe/present進行記録→QMP画面取得→期待色/図形/番号照合を実行する。描画提出通知の直後に固定sleepで成功判定せず、期限内に該当frameが取得画像へ現れることを確認する。
5. 成否にかかわらずsource/image hash、起動設定、画面、serial、QEMU/rendererログを同じ試行IDで保存する。失敗時は停止境界を調べ、有限Queueの範囲で修正・再ビルド・再起動する。試行回数/時間上限はQueue選択時に定め、無限反復しない。
6. 既知パターンは数値/画像比較、未知の表示崩れは保存画像とログをエージェントが確認する。ホストでコマンドを実行し成果物を取得できる接続が必要。必要ならQMP入力操作とGDB/traceを追加する。

## 判定の範囲

目標は画面更新・Vulkan描画・scanoutを自動で観測し、修正→再検証を繰り返せること。物理モニタのvblank時刻、first-pixel-out、実表示遅延をscreendumpで検証したとはしない。readbackが負荷・timingに影響するので性能測定とも分ける。host GPU処理を含むsnapshot復元を前提にせず、最初はQEMUの再起動を使う。

virtio-gpu基本2Dは複数scanout、cursor、display変更通知、RAM→host画像転送、fence付きcommand完了を提供する。EDID/blob等はfeature交渉による。基本2Dにはhost画像→ゲストRAMの読み戻し命令がなく、Vulkan描画はVenus等の別機構。汎用の物理display電源制御、精密vblank counter、複数出力atomic modeset等を基本機能として仮定しない。hostへのGPU readbackとQEMUの画面キャプチャを混同しない。

## API不足の取り扱い

275 Vulkan関数のU/K表と44 callback案を出発点に、p002/p003で不足・過剰・重複を洗い出して同じ資料へ反映する。例えばcursor操作、画像解放通知と実行完了の違い、PCIからGPUへのdescriptor引渡し、Venus共有メモリ/同期を確認する。44件を凍結した必須関数数とはしない。

変更ごとに症状/必要な利用者、変更前後のcontract、U/K責務、必須/任意callback、構造体とversionへの影響、既存利用箇所・回帰結果を記録する。Phase依存や受け入れに影響する変更はWS/Phaseへ同期する。独立した新目標やHAL責務変更はこの計画だけで追加しない。

## 公式資料

- [Venus要件・QEMU構成](https://docs.mesa3d.org/drivers/venus.html)
- [QEMU virtio-gpu](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
- [QMP screendump](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-screendump)
- [egl-headlessのreadback実装](https://github.com/qemu/qemu/blob/master/ui/egl-headless.c)
- [Virtio GPU仕様ソース](https://github.com/oasis-tcs/virtio-spec/blob/master/device-types/gpu/description.tex)

2026-09-12参照。開発時は実際に用いるリビジョンを記録する。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーは計画・GitHub公開を指示した。まだ有限Queue、実行範囲と調査上限は選択していない。コード実装/build/QEMUは未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。

## q304からの実装引継ぎ

[p002](https://github.com/awemorris/zedBSD/issues/383)はGPUフレームワーク受け入れをcleared。本文の実装契約を接続元とする。callbackはopen/close/get_infoと任意resource_create/destroyの5 member。PCI service staging、deferred devfs公開、root open、session/世代handle、EBUSY解除の参照寿命が実装済み。

次に必要なmmap、context/capset/blob、DMA/transport/submit/fence、scanout/present、display権限等は未実装。既存44 callback案と実装済みsubsetを混同せず、具体的なvirtio/Venus利用から補完する。現行VFSにはcdev mmap dispatchがない。HAL責務の変更をこの引継ぎで許可しない。

ホストテストとamd64 buildの成功は実GPU描画やVenusの証拠ではない。p003はplanning、実行Queueは未選択。p002の規約確認は実施済みだが、p004では後続変更も含めて再度最終確認する。

<!-- q305-dependency:start -->
p002の通常GPU登録APIへの修正（q305）はcleared。drv_gpu_register(ops, private_data, **device)/unregister(device)と動的cdev/devfsを利用する。GPUヘッダにPCI公開service/publishはない。使用中EBUSYと解除成功時handle消費の契約を守る。詳細・証拠は[p002](https://github.com/awemorris/zedBSD/issues/383)。mmap/submit/fence/displayを実利用から補う。p003はplanning・未実行・Queueなし。
<!-- q305-dependency:end -->

</details>

## q306 HAL変更の許可待ち（2026-09-12）

ユーザーが「HALの改変には許可が必要です」と明示した。既存宣言の実体補完も含め、HALの全変更に適用する。エージェントが責務変更を伴わないMMIO補完を許可不要と解釈したのは誤り。追加したsrc/hal/amd64/asm.cの8 accessorを取り消し、元のソースへ戻した。具体差分を `plan/ws014/phase003/amd64-mmio-proposal.patch` に保存し、適用・検証の許可を質問中。未許可の候補を用いた追加build/QEMU試験は停止し、独立したdriver/client/loopの確認を続ける。p003はin-progressのまま、clearedではない。

候補はhal.h宣言済みのMMIO read/write8/16/32/64のamd64実装のみ。hal.hや責務の変更はないが、許可は必要である。候補適用時のamd64 kernel/image linkは成功したが、実QEMUでGPU登録にまだ失敗しており、描画成功は確認していない。候補のbuild結果を受け入れ済み実装と混同しない。

前準備はKVM/ANV/QEMU環境、既存kernelの起動・QMP画面取得、TTY履歴のread-only取得まで成立した。GPU core拡張・ユーザー空間クライアント・Venus backendの限定compile/host testsが進んでいる。現行U/K契約は下記資料に記録する。イメージ転送は、ユーザーが10.0.10.25を私有サーバーとして機密データも含め明示許可済み。

## 現行transport設計（q306、実装・検証中）

# WS014 p003: Venus transport and capture contract

2026-09-12 / q306。実装と実測に合わせて更新中。通常のGPUデバイス登録APIを使い、ハードウェア側は `src/drivers/gpu/venus/` に閉じる。

## U/Kの分担

UはVulkanのinstance/device/image/buffer/command/fence、Venusのwire形式と返信結果を扱う。KはPCI/virtqueue、context、capset、blob、検証済みバッファの転送、resource所有権、scanoutを扱う。KはVulkan commandを解釈しない。

初期クライアントは独立実装の `venus-frame`。限定したVulkan commandで最適配置の画像をclearし、coherentなbufferへcopyしてVkFence完了を待つ。VkDeviceMemoryをHOST3D blobとして読み出し、実際のRGBA画素を全件検証して2D resourceへ転送・表示する。一般的なlibvulkan.so、全Vulkan適合、直接scanoutする高速WSI、device mmapを提供した意味ではない。

## GPU API

既存GPU_ABI_VERSION=1と従来3ioctlのlayoutを保持する。kernel内drv_gpu_opsはmember追加のためversion2へ変更し、全in-tree利用側を同時にbuildする。通常のregister/unregisterは維持する。

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

remote harnessは起動ごとに新しいディレクトリ・使い捨てimage・OVMFvarsを生成する。Intel ICDを指定し、QMPの画面を全画素で独立した期待色と照合する。kern_logはdebugcon、通常TTY出力は同じvmunixのELF symbolとLOAD segmentから得たvt_history物理アドレスをQMP pmemsaveで読み出して保存する。HALやTTYへ試験専用ログ経路を追加しない。symbol/実イメージのkernel hashを対応させ、以前のログや画像を再利用しない。

## 環境と実装中に判明した不足

ホストはユーザー指定のawe@10.0.10.25。Intel Iris Xe / i915、ANV25.2.6、QEMU10.0.11、virglrenderer1.1.0、Linux6.19.13。実環境preflightとQEMU起動・QMP画面取得は確認済み。Venus描画の成否はPhaseの試行結果で追記する。

amd64にはhal.h宣言済みのMMIO accessorの実体がなく、初回Venus linkで未定義を検出した。補完候補はsrc/hal/amd64/asm.cの既存read/write8/16/32/64。ユーザーがHALの全改変には許可が必要と明示したため、追加を取り消して元へ戻し、amd64-mmio-proposal.patchを提示して許可を待つ。公開宣言・責務を変えない補完も事前許可の対象。候補でのbuild成功と適用許可を区別し、許可後に適用・実行検証する。

ユーザーはOSイメージ転送と、この私有サーバーへの機密データ転送を明示的に許可した。初回の既存image転送は自動承認レビューが拒否し実行されなかった。許可後に使い捨てコピーの転送を再開した。

## 一次資料

- [Venus protocol wire仕様](https://gitlab.freedesktop.org/virgl/venus-protocol/-/blob/ca19b6358d7cc491bc3e4de76f04c6700876a8fa/docs/VK_EXT_command_serialization.txt)
- [virglrenderer 1.1.0](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/1aeaf5e10a9c89096e96d09599aa419d5c50712f)
- [QEMU virtio-gpu](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)

wire定義の参照と、上流実装のbase systemへの取り込みを区別する。クライアント実装・対応commandの出典詳細は `userland/gpu/venus/README.md`。

## q306 HAL変更の承認・再開（2026-09-12）

ユーザーが提示済み差分に「許可します。」と回答した。`plan/ws014/phase003/amd64-mmio-proposal.patch` の8個のamd64 MMIO read/write accessorの適用・検証を許可されたため、同一差分を適用し、build/QEMU検証を再開する。hal.hやHALの責務は変更しない。直前の「HAL変更の許可待ち」は解消済み。今後の別のHAL変更には、その具体差分に対する事前許可を引き続き必要とする。

PCI BARのcapability部分だけをmapして失敗する問題をdriver側で修正し、register BARを一度だけ全体mapして各capabilityに範囲を渡す。driver単体・ASan/UBSan試験は通過済み。実際のVulkan描画は引き続き未検証で、p003/q306はin-progress。

## q306 実行診断と画面取得方式の更新（2026-09-12）

HALの提示差分はユーザー承認済み。修正後のamd64 image buildと実QEMUのGPU登録・capset4照会が成功した。q306-2d-003/004ではzedBSDの2Dクライアントが全画素/FNV検証とpresent成功マーカーまで到達したが、QMP screendumpは継続してno surfaceを返す。

QEMU v10.0.11公式実装を確認した結果、GL scanoutはSCANOUT_TEXTUREとなり、QMPが呼ぶqemu_console_surface()はNULLを返す。egl-headlessは別途pixman surfaceへ実際のGL画像をreadbackしており、VNCはそのsurfaceを参照する。このためQMPは起動制御・console/log取得を維持し、描画画像はQEMU標準のVNC Unix socket経由で取得する方式へ更新する。外部TCPポートは使わない。実画像の全ピクセル・独立期待値・試行/frame/hash照合という受け入れは維持し、表示成功マーカーだけではclearしない。QEMUやゲスト画像を改造して成功画面を作る方式ではない。

ホストにはvirgl-serverが欠落していたため、Debian公式virgl-server_1.1.0-2_amd64.debを専用rootのdependencies配下へ展開した。システムのdpkg状態は不変。RENDER_SERVER_EXEC_PATHで指定しbinary/packageのhashと起動確認を記録する。

q306-venus-001はcapset4/wire1照会後、返信blob確保付近でENOMEMとなる。Vulkanコマンドの実行成功はまだ未確認。p003/q306はin-progressのまま、driverのHOSTVISIBLE mappingと有限VNC captureを修正・検証する。

根拠: https://github.com/qemu/qemu/blob/v10.0.11/ui/console.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/ui-qmp-cmds.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/egl-headless.c 、https://github.com/qemu/qemu/blob/v10.0.11/ui/vnc.c 。

</details>
