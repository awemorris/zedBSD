<!-- awesome-plan project=zedbsd record=ws035p052 -->

# ws035-p052: 2 つのモードの核（全画面の直接 scanout と Vulkan の合成）

Phase ID: `ws035-p052`
Parent: [WS035](../ws.md)
Status: cleared（q457-i01）
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
- q457-i01（2026-09-26、メインセッション）: cleared（下の結果）。

## 実装（2026-09-26、q457-i01）

- `userland/base/zwl/compose.c`・`compose.h`（新）: Vulkan の instance・device（`VK_KHR_display`・swapchain・外部 memory と fence の fd）、render pass（背景 0x203040 で clear）、pipeline 2 本（不透明・alpha、textured quad を push constant で置く）、sampler・descriptor pool・command buffer・書き出せる fence。display の surface と FIFO の swapchain は vkdemo の `display.c`（標準の display plane の選び方）をそのまま link して使う。frame は背景と窓を下から順に 1 本の command buffer で描き、fence を `vkGetFenceFdKHR` で fd にして event loop の poll に入れる（CPU で待たない）。frame は 1 つずつ。完了で、その frame が sampling した buffer の hold を外し（置き換わっていれば `wl_buffer.release`）、表示した surface の frame callback を送る。
- `userland/base/zwl/import.c`（新）: `wl_buffer` の作成時に 1 回だけ、linear の VkImage・OPAQUE_FD の `vkAllocateMemory`（buffer の fd の複製）・view・descriptor set を作り、GENERAL の layout へ 1 回移す。client の layout（memory type、行の幅、offset）が Vulkan の image と合うことを確かめる。
- `userland/base/libvulkan/memory.c`: `vkAllocateMemory` の OPAQUE_FD の import が、WSI が送る画像の capability の fd（`GPU_RESOURCE_EXPORT`）も受ける（`GPU_ALLOCATION_IMPORT` が EINVAL なら `GPU_RESOURCE_IMPORT` で、memory type と大きさを照合）。sq001 の回避策（zwl に libvulkan を静的に link し内部関数を呼ぶ）は要らなくなった（[secondary-deferred.md](../secondary-deferred.md) の 1 行目を解決）。
- `userland/base/zwl/display.c`: 1 pass ごとに commit を current にし（初めての画像で map、null で unmap）、最前面の窓が全画面で画像がそのまま出力にできる（出力と同じ大きさ、linear）なら全画面モード、それ以外はウィンドウモード。全画面へ入るときは frame の完了を待ってから swapchain と surface を破棄し、`GPU_DISPLAY_CLAIM`・`GPU_DISPLAY_PRESENT` で client の画像を直接出す（隠れた窓の frame callback は送る）。出るときは `GPU_DISPLAY_RELEASE` の後に swapchain を作り直す。窓の配置は中央から 32 px の cascade（8 で一巡）。Vulkan が使えないとき、または `--direct` では従来の直接表示。
- `userland/base/zwl/protocol.c`: 最初の configure は窓なら `0×0`（client が決める）と activated、全画面なら出力の大きさと fullscreen。`set_fullscreen` で位置と大きさを覚えて原点・出力の大きさ、`unset_fullscreen` で元へ（configure を送り直す）。ack_configure は最新以前の serial を受ける。
- `userland/base/zwl/seat.c`: pointer の座標は窓の位置を引いて送る。`objects.c`: buffer とともに Vulkan の import を解放、client の破棄の前に進行中の frame を終える。`main.c`: fence の fd の poll、`--direct`、`ZWL PERF compose`（frame の数、acquire・submit+present・全体の時間）。
- shader: `userland/base/zwl/shaders/quad.vert`・`quad.frag` と、SPIR-V を header にする `shaders/regenerate.py`（`glslc`・`spirv-val`）。build は生成済みの `shaders.h` を使う。
- 試験の client: 設計の `zdtest` の代わりに `wltest` に `--windowed`・`--size=WxH`・`--color=RRGGBB`・`--fullscreen-at=N`・`--unfullscreen-at=N` を足した（既定の動きは変えない。renderer の複製を避けるための技術的な判断）。configure で大きさが変わると swapchain を作り直す。
- 試験: `plan/ws035/tests/zdesktop-p052.sh`（下の手順を自動で）と `zdesktop-check.py`（画面の画素の照合）。
- build: `platform/amd64/vmunix.mk` に zwl の link の規則（libvulkan）。

## 検証（2026-09-26、QEMU 8 GiB 4 vCPU NVMe、Venus（host は Lavapipe））

| 受け入れ | 結果 |
| --- | --- |
| 1. 窓 2 つを背景の上の別の位置に | `zdesktop-p052.sh`: 赤 400×300 が (440,250)、緑 300×200 が (522,332) に重なって出る。背景・両方の窓・重なり・窓の外の 9 点が一致（`build/ws035-p052/windows.png`） |
| 2. 全画面モードで直接 scanout、解除で戻る | 緑の client が `set_fullscreen` → 全画面モード（画面の 4 点が緑、`fullscreen.png`）、log の `direct=1` の present 99 回（client の buffer の resource をそのまま）。`unset_fullscreen` → ウィンドウモード、同じ位置と大きさに戻る（`back.png` の 5 点）。既定の全画面の wltest は最初から全画面モードで、模様が一致（`default-fullscreen.png`） |
| 3. import は buffer ごとに 1 回 | Vulkan の import 12 回（swapchain の 3 枚 × 4 組: 赤、緑、全画面の緑、戻った緑）に対し frame 167。定常の commit に import は無い |
| 4. fence の fd を poll、完了で release | frame の完了（fence の fd が readable）で hold を外し `ZWL RELEASE` が続く。CPU の wait は client の破棄と終了のときだけ |
| 5. 窓ごとの描き方の枠（D10） | `enum zwl_draw`（不透明・alpha）を import ごとに持ち、pipeline を選ぶ。効果は無し |
| 6. 規約・warning | 新しい file（compose.c・import.c・compose.h）と変えた行の style-check 0（libvulkan の lock の区切りの段落は既存と同じ形で、道具の誤検出）。amd64 の build warning 0 |
| 7. 切替と frame の時間 | 切替: ウィンドウ→全画面 760〜790 ms、全画面→ウィンドウ 343〜389 ms（起動時の出力の作成 545〜581 ms）。ウィンドウモードの 1 frame は約 100 ms（swapchain の acquire 21〜23 ms、submit+present 45〜73 ms、記録ほか約 20 ms）。この環境（Venus を host の Lavapipe で描き、blob の scanout を zink で読む）では vkdemo も 8 fps 程度で、時間は libvulkan と Venus の present の経路にある。全画面モードの present の ioctl は約 24 ms |
| `--direct` | 従来の直接表示（60 frame、mode の切替なし） |
| boot test | PASS（`build/boot-test-amd64-ws035p052/login.png`、zdesktop の image） |

未確認: 切替の隙間に文字 console が一瞬出るか（設計 D0）。画面の読み取りの間隔（数百 ms）では捉えていない。実機（i915）は未実施。

## 結果（2026-09-26、cleared）

zdesktop（zwl）が 2 つのモードを持つ: ウィンドウモードは背景と窓を下から順に Vulkan で合成して `VK_KHR_display` の swapchain で出し、最前面の窓が全画面になると swapchain を破棄して client の画像をそのまま scanout する。client の画像は buffer ごとに 1 回 import し、frame の完了は fence の fd で知る。

残り: ウィンドウモードの frame の時間（QEMU で約 100 ms）は libvulkan と Venus の present の経路の費用で、実機か別の Phase で調べる（F-021）。切替の隙間の console の確認は p055 か実機で。
