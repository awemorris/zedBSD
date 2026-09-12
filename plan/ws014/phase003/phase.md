<!-- awesome-plan project=zedbsd record=ws014-p003 -->

# WS014 p003: QEMU＋Venusの自動デバッグループ

<!-- awesome-plan-current:start -->
Status: planning
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Queue: none
<!-- awesome-plan-current:end -->

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
