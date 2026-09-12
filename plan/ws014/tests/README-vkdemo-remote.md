# vkdemoの3D描画検証

WS014 p005では `/bin/vkdemo` のvertex/fragment shader、texture、depth、時間による回転を、i915/ANVを使うQEMU/Venusで確認する。p003の2帯画像とは別の描画検証であり、既存のGPU UAPIと通常の動的driver登録を使う。

## 再現

```sh
python3 -B plan/ws014/tests/run-vkdemo-remote.py --attempt q307-vkdemo-NNN
```

`NNN` は未使用の試行名へ置き換える。同じ名前を再利用しない。既定hostは `awe@10.0.10.25`、remote rootは既存依存物を保持する `/home/awe/zedbsd-q306-venus`。ユーザーはこのprivate hostへのimage転送を明示許可済み。既存のprivateな `virgl-server 1.1.0-2` を `RENDER_SERVER_EXEC_PATH` で指定し、system packageの変更はしない。

wrapperは `config-vkdemo-amd64.mk` と `build/vkdemo-amd64` を使って `make -j16 disk-image` を実行する。arch image、data image、swapも専用build directoryへ指定する。起動用imageとOVMF変数は試行ごとの使い捨てcopy。GPTのCRCを検査し、実際にvmunixを持つFATを選び、そのcopyの設定だけを `init=/bin/sh` に変更する。通常の `config.mk` は変更しない。

QEMUはKVM、1GiBの共有memfd、virtio-vga-gl/Venus、8MiB host-visible aperture、egl-headless、host Intel ICDを使う。起動とキー入力、kernel symbolに対応するconsole memoryの取得はQMP。GL画像はegl-headless readbackからUnix VNC/RFB RAWで取得する。詳細は [p003手順](README-venus-remote.md) を参照。

## 六つの画像と通常実行

一つのprocess/contextで `--verify-session` を実行する。固定時刻0/1000/2500msの3枚と、単調時計を実際に進めた3枚を、同じrender関数・保持した資源で描画する。各PRESENTは完了したGPU readbackのRGB SHA256、時刻、frame、run tokenを出し、最大30秒だけstdinの改行ACKを待つ。hostはその間に実画面を取得し、hashと独立した期待値を照合してから次へ進める。固定画像の再送、前試行のconsole marker、時刻やframeの進まない結果は成功としない。

この確認後、同じVMで `--duration=2` の通常アニメーションを再openして実行する。時刻が進む複数のlive PRESENT、DONE、shell復帰を確認する。GPU資源は各processの最後にcloseで回収する。全体のVM・capture・通信待ちは有限とし、終了時はQEMU exit code 0も確認する。

## 独立した画像判定

320x240、背景RGB(16,24,40)、半幅(0.75,0.5,0.375)の直方体を使う。期待値はcamera rayを逆回転して直方体と交差させ、最も近い面のUVから64x64 textureのnearest texelを選ぶ。GPUの出力やguestのshaderを使って期待画像を作らない。fixture生成画像は検証器の試験専用で、GPU実行の証拠には使わない。

RGBを完全一致で比較し、幾何境界の0.30pixel以内とtexel境界の0.02texel以内だけを除外する。除外は画面全体の5%以下。面積、複数面、textureの色数も確認し、clearのみ、無地、未回転、誤UV、古いframeを拒否する。実画面の全RGB byteは、境界除外を行う前にGPU readback SHA256とも一致しなければならない。

```sh
python3 -B plan/ws014/tests/test-vkdemo-oracle.py
python3 -B plan/ws014/tests/test-vkdemo-shaders.py
sh plan/ws014/tests/run-vkdemo-cli-test.sh
python3 -B plan/ws014/tests/test-venus-rfb.py
```

## 成果物と判定範囲

`plan/ws014/temp/remote/<attempt>/result.json` にbuild/source/shader/image/toolのhashとremote結果を保存する。`evidence/` に6枚の実PPM、各oracle診断、console、QMP、kernel/rendererログが入る。取得後にローカルでもhash・画像判定を再実行する。再利用buildは明示的な `--skip-build` の場合だけで、その事実を結果に残す。

実測結果と完了判定は [p005本文](../phase005/phase.md) と実行結果を正とする。これは有限の独立Venus clientによるgraphics pipelineの検証であり、汎用libvulkan/ICDや全Vulkan適合試験ではない。shaderソース・生成SPIR-V・再生成手順は `userland/base/vkdemo/shaders/` に保存する。
