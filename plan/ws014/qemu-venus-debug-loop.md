# QEMU＋Venusの画面取得と自動デバッグ計画

2026-09-12 / WS014 p003の資料。

q306実行中の現行契約は [Venus transport](venus-transport.md)、再現コマンドは [リモート検証README](tests/README-venus-remote.md)、結果は [p003](phase003/phase.md) を参照する。Linux i915/ANVホストの環境・GPU登録・2D実画面の全件一致を確認済み。QEMU10.0.11はGL scanoutにQMP screendumpを使えないため、egl-headlessのreadback画像をVNC Unix RAWで取得する。QMPは起動制御・console取得を担当する。hostmemは現行amd64 MMIO窓に収まる8MiBを基準とする。Vulkan側の最終判定はPhaseの実行証拠で確定する。

以下は実行前の候補・受け入れ検討の履歴であり、取得方式と対応範囲は上記の実測済み契約を優先する。

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

2026-09-13 JST 最終結果: q306で2DとVenusのframe1/2を実行し、GPU readbackとVNC実画面が全件一致。p003の有限受け入れを達成。詳細はphase003/results.mdと証跡JSON。一般的なVulkan実装・native i915・p004の実行とは区別する。
