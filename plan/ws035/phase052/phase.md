<!-- awesome-plan project=zedbsd record=ws035p052 -->

# ws035-p052: 2 つのモードの核（全画面の直接 scanout と Vulkan の合成）

Phase ID: `ws035-p052`
Parent: [WS035](../ws.md)
Status: in-progress（q457-i01）
Phase disposition: normal
Queue: q457-i01（前の試行 sq001-i01 は uncleared、下の「試行の記録」）
設計: [compositing-design.md](../compositing-design.md)（2026-09-25 承認）の D0・D1・D2（GPU の画像）・D3（sampling の fence）・D4（1 回目）・D6・D10（枠だけ）

## 目的

`zwl` に合成の核を入れる。ウィンドウモード（背景→窓を下から順に Vulkan で描き、`VK_KHR_display` の swapchain で出す）と
全画面モード（最前面の全画面の窓の画像をそのまま scanout）の 2 つを持ち、窓の状態で切り替える。

## 受け入れ

1. QEMU（Venus）で Wayland の client 2 つ（窓）を、背景の上の別の位置に同時に出す。画面の読み取りで、背景の色と
   2 つの窓の位置・大きさ・内容が期待と合う。
2. 1 つの窓を全画面にすると全画面モードに入り、その client の画像がそのまま scanout される（合成の copy 0。
   zwl の記録で present が client の buffer の resource で、画面が client の画像と一致）。解除でウィンドウモードに戻り、
   背景と 2 つの窓が再び出る。
3. client の画像の import は buffer ごとに 1 回（`wl_buffer` の作成時）。定常状態の commit に import が無い（記録の数）。
4. frame ごとの submit に fence を付け、fd を poll に入れて完了を知る（CPU で wait しない）。完了した frame が
   sampling した buffer のうち、新しい commit に置き換わったものへ `wl_buffer.release` を送る。
5. 窓ごとの「描き方」の枠（D10）がある（既定は不透明の quad。効果は無し）。
6. 新しい C は `plan/coding-style.md` の全文に従い、`style-check.py` の指摘 0。amd64 の build は warning 0。
7. モードの切替の時間と、ウィンドウモードの 1 frame の時間を測って記録する。

## 設計（実装の決定）

- **Vulkan の client**: zwl は `libvulkan` を使う（instance: `VK_KHR_surface`・`VK_KHR_display`、device: `VK_KHR_swapchain`、
  `VK_KHR_external_fence`・`_fd`）。出力は display plane の surface と FIFO の swapchain。描画は 1 本の render pass、
  textured quad（頂点 4 つを vertex shader が push constant から作る）、pipeline は不透明と alpha の 2 本。
- **client の画像の import**: `wl_buffer` の作成時に 1 回。Vulkan の sampling 用の VkImage・VkDeviceMemory・VkImageView・
  descriptor set と、全画面モードの scanout 用の `GPU_RESOURCE_IMPORT` を両方作る（設計 D2 の「全画面モードの import も 1 回」を、
  fd を受けた時点にまとめた）。
- **import の経路（回避策）**: WSI が送る fd は `GPU_RESOURCE_EXPORT` の画像の fd で、libvulkan の公開 API の
  `vkAllocateMemory`（`VkImportMemoryFdInfoKHR`、OPAQUE_FD）はこの種類の fd を受けない（`VULKAN_MEMORY_FD_SCHEMA` の fd だけ）。
  libvulkan は範囲外なので、zwl は `gpu-share-test` と同じく libvulkan の object を静的に link し、内部の
  `vulkan_wsi_shared_image_import` で import する。[secondary-deferred.md](../secondary-deferred.md) に記録した。
- **同期**: frame ごとに VkFence（OPAQUE_FD で export）を付け、submit の後に `vkGetFenceFdKHR` の fd を poll に入れる。
  signal で、その frame が保持していた buffer の hold を外す（外れて hold 0 になったものに release）。
- **モード**: 最前面の窓が全画面の状態で、その buffer が scanout できる（GPU の画像、linear、出力と同じ大きさ）とき全画面モード。
  入るとき `vkDeviceWaitIdle` → swapchain と surface を破棄（libvulkan が lease を返す）→ zwl が `GPU_DISPLAY_CLAIM` → client の画像を present。
  出るとき `GPU_DISPLAY_RELEASE` → surface と swapchain を作り直す。
- **configure**: ウィンドウモードの窓は最初 `0×0`（client が決める）、state は activated。`set_fullscreen` で出力の大きさと
  fullscreen、`unset_fullscreen` で全画面の前の大きさに戻す。
- **配置**: 新しい窓は出力の中央に置き、窓の数に応じて 32 px ずつずらす（cascade、D6）。
- **焦点**: この Phase では最前面の窓（focus・移動・resize は p011）。pointer の座標は出力の座標から窓の位置を引いて送る。
- **試験の client**: `wltest` は常に全画面を求めるので、窓の試験には新しい `zdtest`（`userland/base/zdtest`、wltest からの派生）を使う。
  `--size`・`--pattern`・`--fullscreen-at`・`--unfullscreen-at` を持つ。

## 試験

- build: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws035-sq`（warning 0）。
- QEMU: [zdesktop-guest.sh](../tests/zdesktop-guest.sh)（Venus、host は Lavapipe）、画面は [zdesktop-shot.py](../tests/zdesktop-shot.py)。
- 画面の判定: [zdesktop-check.py](../tests/zdesktop-check.py) で背景と窓の位置の色を照合する。

## 試験環境（2026-09-25 に用意）

このホスト（centris）で Venus を動かす。GPU の無いホストなので、host の Vulkan は Lavapipe、GL の readback は
vgem の render node の上の zink（Lavapipe）を使う。

- `sudo apt-get install virgl-server`、`sudo modprobe vgem`、`/dev/dri/renderD128` を 0666。
- Debian の virglrenderer では libvulkan が physical device を 1 つも見つけない（strict queue と native quiescence が無い）。
  WS014 が作った strict queue の virglrenderer（Venus host の `dependencies/q312-quiesce/install`）を
  `build/ws035-sq-venus/install` へ写して使う（`LD_LIBRARY_PATH` と `RENDER_SERVER_EXEC_PATH`）。
- llvmpipe の GL では blob の scanout の readback で QEMU が SIGSEGV（`egl_fb_read`）。`MESA_LOADER_DRIVER_OVERRIDE=zink`
  と `LIBGL_ALWAYS_SOFTWARE=1` で動いた。
- QMP の `screendump` は GL の scanout で `no surface` になるので、VNC（`display=venus`）の RFB で撮る（WS014 と同じ方法）。
- 確認: vkdemo の立方体と、zwl＋wltest の画面を撮れた。
- guest の memory は 8 GiB。

Venus の host（chaos）の iGPU の切替は、作業者の判断で行わなかった（自動の許可が得られなかった）。

## 試行の記録

- sq001-i01（2026-09-25、secondary queue、サブエージェント）: uncleared。試験環境（上の節）と import の回避策の判断（[secondary-deferred.md](../secondary-deferred.md)）を残したが、zwl の実装は commit されていない（`userland/base/zwl` に合成の source は無い、`zdtest` も無い）。secondary queue は後に廃止（2026-09-26 のユーザーの規則: サブエージェントを使わない）。
- q457-i01（2026-09-26、メインセッション）: 実行中。

## 結果

（実行中）
