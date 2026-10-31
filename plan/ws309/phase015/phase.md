<!-- awesome-plan project=zedbsd record=ws309-p015 -->

# ws309-p015 game mode の V2: display の swapchain の image を client に貸す（libvulkan の export、compositor の受け渡し）

- 親: [WS309](../ws.md)、設計 [design.md](../design.md) §6（J1 の決定「V2 swapchain 共有 (推し)」）。Queue: q1662-i10（P1）。
- 状態: in-progress（2026-10-31 14:50 P2 q1782 で着手）。前: planned（2026-10-30 判断 1〜4 がユーザーの回答で決まった: 1 (a)・全画面と窓の切り替えは compositor の都合で app は気づかない、2〜4 は推し。旧: uncleared、判断待ち）。
- 依存: p013（cleared、T4 と暫定の ioctl）。

## 今（main 868318312 の後、branch `agent/p1-ws309i`）

- ゲームモードは T4 が backend の `GPU_DISPLAY_PRESENT` で client の buffer を flip する（p013、決定 8 の暫定）。
- libvulkan の WSI（`userland/desktop/libvulkan/wsi.c` 1615 行・`wsi-wayland.c` 1424 行）: display の swapchain の image は libvulkan が作り、外へ出す口は無い。
  `VK_EXT_swapchain_maintenance1`・present fence・present_wait も無い（design §6 の調べ）。

## 要る判断（Q1・ユーザーへ、実装の前）

1. **client 側の形**: (a) client の libvulkan の Wayland WSI（`wsi-wayland.c`）が、game mode の間だけ compositor から貸された image を swapchain の image として使う
   （app は今の Vulkan の WSI のまま、変更なし）、(b) 新しい protocol を app が直に使う（mview などの app を書き直す）。推し (a)（Vulkan display を OS の正式 API にする決定 8 の趣旨、
   app の変更が無い）。ただし Linux・FreeBSD の client（Mesa の WSI）は (a) にならず、今の flip の経路（V1 か暫定）に残る。
2. **compositor と client の間の protocol**: 貸す image の fd・形式・数、acquire（空いた image の知らせ）と release、client の描画の完了の fence を運ぶ private の Wayland
   protocol を新しく作るか（`keiland_game_surface_v1` の類）。新しい protocol は製品の判断（ユーザー）。
3. **libvulkan の口**: display の swapchain の image の memory を `vkGetMemoryFdKHR` で出す（`VkExternalMemoryImageCreateInfo` を `VkSwapchainCreateInfoKHR` の pNext で受ける、標準に
   無い組み合わせ）か、`VK_KHR_swapchain` の外の libvulkan の独自の関数か。design §6 は前者（標準の型だけ）を推す。driver（venus・amdrdna・nvrtx・i915・mali・powervr）ごとに
   swapchain の image の確保が export できる memory かの調べが要る（P2・P5・P6 の driver と重なる、Q1 に知らせる）。
4. **画面から外れた事の知り方**: design §6 のとおり「同じ image を再び acquire できた事」で知る（T4 が acquire を同期で待つ）。present fence は使わない（レビュー 6）。

## 実装の手順の案（判断の後。各段で build・host・QEMU）

1. libvulkan: display の swapchain の作成で external memory の印を受け、image の memory を fd で出す（venus から）。host の libvulkan の単体試験。
2. protocol の XML と compositor の受け口（game mode に入った client に swapchain の image を貸す、release・acquire の event）。T4 は acquire（同期）→ client に知らせ → client の
   commit の fence → `vkQueuePresentKHR`（queue の lock の下、J2）。
3. client の libvulkan の Wayland WSI: game mode の event で貸された image を import して swapchain の image にする（判断 1 の (a) の時）。
4. 暫定の `GPU_DISPLAY_PRESENT` の経路を外す（Guardrail の例外を閉じる）。QEMU（Venus）の `scanout-guest.sh` を V2 で、実機（RDNA・RTX・VF2）は Q1 の枠で。

## ユーザーの決定（2026-10-31）

- 2026-10-31 ユーザーから、game mode は文字通りの zero copy の direct scanout にする（client は compositor と共有する image に直に描き、WSI の present の copy をしない）、
  VF2 だけでなく同じ形の全部の GPU で直す、今すぐ着手してよい、という趣旨の指示を受けた。
- 2026-10-31 ユーザーから、compositor の image の形式は compositor が決める形でよいか、と問われ、Q1 が「よい（game mode で貸す image は compositor が display の都合で
  形式・並びを決め、client は compositor が示す surface の形式から選ぶだけ）」と答えた。
- Q1 が code で確かめた事: libvulkan の `swapchain_direct_possible`（userland/desktop/libvulkan/wsi-swapchain.c）は VK_KHR_display の primary plane の chain
  だけを direct にし、Wayland の chain は GPU に関わらず present ごとに共有の image へ copy する（`present_copy_shared`）。よって VF2・RTX・RDNA・i915・Pi・Venus の
  全部の Wayland の client が copy している。
- 2026-10-31 ユーザーから、compositor の image（compositor 自身の display の chain も、game mode で貸す image も）はその GPU の最も速い image にする、
  という趣旨の指示を受けた（Q1 経由）。描けて display が scanout でき、画質を大きく落とさない物のうち、（形式・並び・tiling・圧縮）の組から最も速い並びを選ぶ。
  形式の色の深さは client・compositor の物より落とさない（RGB565 などへ落とさない）。決め打ちの linear でなく、driver ごとの好みの一覧を並びの欄の後ろに置く。
- 2026-10-31 ユーザーから、RTX の block-linear は必須で実装する、という趣旨の指示を受けた（Q1 経由）。分担（Q1 の決定）: P2（p015）が汎用の経路
  （protocol の並びの欄、libvulkan の export・import と並びの選び、compositor の貸し出し、driver の好みの口の libvulkan 側）、P3（WS291 p023）が uapi の追加・
  `gpu-share.h` の op・`gpu.c` の core と nvrtx の全部（block-linear の色の配置、NVC57E の block-linear scanout）。P2 は src/drivers/gpu/nvrtx/ と P3 の 3 file を直さない。

## 実装（2026-10-31 P2 q1782-i01、worktree `p2-ws309v2`）

### 形（Q1 のレビュー済み、2026-10-31）

- 判断 1 (a)・2・3・4 のとおり。app は変えない。Window mode の振る舞いは今のまま（窓の間は今の copy の経路）。
- libvulkan（判断 3）: `VkSwapchainCreateInfoKHR` の pNext の `VkExternalMemoryImageCreateInfo`（OPAQUE_FD）で display の chain を「出せる」印にし、
  `vkGetMemoryFdKHR` は `memory = VK_NULL_HANDLE` と pNext の `VkMemoryDedicatedAllocateInfo{image = swapchain の image}` で、direct（linear で display が
  そのまま見せる）chain の image の fd を返す。fd は image の capability（`GPU_RESOURCE_EXPORT`、image の記述つき。`device_id` を 0 にして出し直すので Venus の
  「最初の記述と同じ」の検査に合う）。標準の型の標準に無い組み合わせで、libvulkan の実装の判断（README の非適合点に書く、未了）。
- protocol（判断 2）: `kl_game_v1`（global、版 1）と `kl_game_surface_v1`。request: `chain(format, usage, width, height, count)`（client の chain、count 0 は貸せない）、
  `tag(wl_buffer, serial, index)`、`decline(serial)`。event: `image(serial, index, fd)`、`lend(serial, count, width, height, format, usage, layout)`、`withdraw(serial)`。
  libwayland に server の作る new_id が無いので、client が貸された fd から `kl_gpu_buffer_v1.create_buffer` で wl_buffer を作り tag で名付ける
  （compositor は普通の client の buffer として import するので、withdraw の後もそのまま合成できる）。貸した buffer は compositor が持った状態で始まり、
  `wl_buffer.release` で client に渡る。`layout` は kernel の tiling の値（`GPU_IMAGE_*`）か 0（各 image の kernel の記述に任せる）。今は 0 を送り、client は import の
  kernel の記述で linear を確かめる（P3 の OPTIMAL が入れば、その値が import の記述で届く）。
- client の WSI: lend が今の chain に合う時（format の channel の並び・extent・usage・count・linear）、present は `VK_SUBOPTIMAL_KHR`（Mesa が dmabuf feedback で
  するのと同じ）。app が作り直した chain は貸された image を import（app の format のまま、UNORM か SRGB の同じ byte）して swapchain の image にし、copy しない
  （direct）。合わない chain は decline。withdraw の後、貸された chain は普通の buffer として合成され、`VK_SUBOPTIMAL_KHR` で自分の image の chain へ戻る。
  app の VkImage の memory を付け替えないので nvrtx の「2 回目の bind を断る」に当たらない。
- compositor: 入り方は今と同じ（window の swapchain を閉じる change）の後、display の thread が game の display chain を作る（FIFO、display の mode、client の
  count ＋ 2、usage は COLOR・TRANSFER_SRC/DST・SAMPLED のうち surface が許す物、出せる印、各 image の fd）。T4（present の thread）が acquire と fence の待ち
  （display が image を離した）で image を client に渡し、client の commit（acquire fence の済み、kwl_fence_ready）の後に `vkQueuePresentKHR`（queue の lock）、
  続けて前に出ていた image が戻る（再び acquire できる、判断 4）のを待つ。client が 2 秒のうちに貸された image へ描かない時・decline の時は合成へ戻る。
  出る時は withdraw、T4 の job の終わりの後に display の thread が game chain を壊し、window mode が今のとおり開き直す。
- 暫定の経路の撤去（手順 4）: `kl_backend_scanout_*`（`keiland-backend-display.h`・drm の stub）、zedBSD の buffer の記録（`kwl_gpu_buffer_record`）、
  `scanout-zedbsd.c` の build を外した（file の削除は Q1 の OK 待ち）。Guardrail の例外の文と `plan/tools/gpu-boundary/v1-check.sh` の例外の記述は Q1 に頼んだ。

### P3（WS291 p023）との約束（Q1 の決定、2026-10-31）

- 並びの名: `gpu_image_descriptor.tiling` の族の値、`reserved` → `layout_parameter`。好みの ioctl `GPU_IMAGE_LAYOUTS`（_IOWR('G', 46)、in: format・width・height・
  uses（RENDER|SAMPLE|SCANOUT|SHARE）・display_id、out: 最大 16 の {tiling, parameter, flags}、速い順、色の深さを落とさない）、既定は LINEAR、`GPU_CAP_IMAGE_LAYOUTS`。
- libvulkan（P2）: display の chain の image を作る時に、renderer の RENDER+SAMPLE の一覧と display の SCANOUT の一覧に共にある最初の並びを選ぶ（別の device なら
  両方の fd。VF2 は別の device なので LINEAR のまま）。LINEAR 以外は `VK_IMAGE_TILING_OPTIMAL` と私用の pNext（sType 0x7a650001、body [u32 tiling][u32 parameter]、
  `GPU_CAP_IMAGE_LAYOUTS` の context だけに送る）。import は記述の usage のまま作る。P3 の core が main に入ってから繋ぐ（未了）。
- 未了: 上の libvulkan の選び・私用の pNext・OPTIMAL の import（P3 の core の後）。nvrtx の本当の zero copy は P3 の block-linear の配置と scanout の後。

### driver ごとの direct（linear）の display の chain の調べ（2026-10-31、読むだけの調べ）

| driver | direct の display の chain | 制限 |
| --- | --- | --- |
| venus | 可 | 16〜4096 の大きさ、linear の機能は host の driver 次第。同じ context への 2 回目の import の host の受け入れは未確かめ（QEMU で確かめる） |
| amdrdna | 可 | BGRA だけ、pitch 256。GOP の stride と合わない時 present が失敗する |
| nvrtx | 可 | pitch-linear（256）。深さの target のある pass は block-linear の影へ描き pass の終わりに全画面の copy（P3 の block-linear で解く） |
| i915 | 可 | display の BLOB の経路が panel の back buffer へ GPU で copy する（本当の zero copy でない、残り） |
| mali | 可 | extent・pitch・byte の順・16 B・4 GiB 未満が合う時だけ flip、他は CPU の copy |
| powervr（VF2） | 可 | dc8200 を取った時・zero copy が有効・pitch と extent が合う時だけ flip、他は CPU の copy。render と display が別の device |
| bcm2711（Pi 4） | 可（4K は不可） | display の resource は 32 MiB まで（4K は copy の経路）、連続の memory が要る。Pi 4 はこれまで game mode に入らなかった（backend が ENOTSUP）。V2 では content=video の窓が入るため window の chain を閉じて開き直す（ws236-p011 で firmware の decoder が止まった事がある、未確かめ） |
| ghci | 不可 | `share_import` が SCANOUT を断る（copy の経路のまま） |

### 確かめ（2026-10-31 15:3x まで）

- build（`ZEDBSD_CONFIG=plan/ws309/tests/config-amd64-p1.mk BUILD=build/p015`: libwayland-client.so・libvulkan.so・bin/wayland）warning 0。
- `plan/tools/gpu-boundary/v1-check.sh` PASS（135 source）、`plan/ws214/tests/vulkan-overlay-host.sh` PASS、`plan/ws217/tests/host-scanout-rules.sh` PASS。
- commit: 09527b021（libvulkan・libwayland）、9b09b8bb7（compositor・backend・libvulkan の fallback）。
- 未実施（15:3x 時点）: QEMU（Venus）、VF2、RTX、BUG-375 の往復。

### QEMU（Venus）の確かめ（2026-10-31 16:0x〜16:4x）

- 足した物（8d7f92a09・0b48946c0・a3e740841）: window の display の chain を「出せる」印で作り、開いた時に image 0 の fd の export を一度試す（`KWL STARTUP step=output-swapchain ... lendable=N`）。
  出せない display（の chain）は game mode に入らない（window の swapchain を閉じない。Pi 4 の ws236-p011 の心配への Q1 の指示: driver の名でなく能力で決める）。
  libvulkan: display の primary plane の surface の usage に SAMPLED を足した（Venus の display の surface が COLOR・TRANSFER だけで、game chain が作れなかった）。
  `GPU_IMAGE_LAYOUTS` の配線（display の node に SCANOUT を問い、renderer の RENDER|SAMPLE|SHARE と共にある最初の並び。LINEAR が先なら LINEAR）、LINEAR でない image は
  `VK_IMAGE_TILING_OPTIMAL` と私用の record（0x7a650001）、client の import は `GPU_CAP_IMAGE_LAYOUTS` の時だけ LINEAR でない物を受け、受けられない時は自分の image へ戻って decline。
  adoption の待ちを 5 秒に（Venus で client の作り直しと import に 0.4〜2.7 秒かかった）。
- 環境の事（code の結果でない）: host の負荷の高い時に guest の NVMe の時間切れで UFS が読み取り専用になった（1 回目）。この worktree に `build/ws035-sq-venus` が無く、
  `VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install`（読むだけ、Q1 の OK）で動かした。ws309 の forge の image は greeter が display を持つので、
  道具の想定の menu の image（`plan/tools/titlebar/build-menu-image.sh build/p015menu`）を使った。
- `GUEST_RUNTIME=build/p015run VENUS_RENDERER=… plan/tools/scanout/game-mode-cycle.sh build/p015menu/hdd-image.img /tmp/p2/cycle5 6`（menu の image は a3e740841 ＋ main の merge）:
  6 回とも `KWL SCANOUT direct=1 ... images=6 format=44`、`lend images=0x3f`（6 枚を client へ）、貸した image への present が 78〜106 frame（`direct=0 reason=no-window ... frames=N`、
  N は client が貸された image に描いて compositor が `vkQueuePresentKHR` した数）、毎回 `KWL MODE window` で window mode へ戻った。`KWL FAILED`・`MODE_ERROR` 0、pointer で出た回 0。
  道具の判定は FAIL: 1 回目の mview の起動（model の読み込み）が約 50 秒かかり、道具の 40 秒の待ちの後に終わったため 1 回目の no-window・window が 2 回目に数えられた（2〜6 回目は全部の数が 1）。
  log: build/review/ws309/p015-cycle5-kwl.log・p015-cycle5.txt。画面の PNG は Venus の VNC が直の scanout を映さない（道具の注のとおり）ので判定に使わない。
- 手で 1 回（m8、spin 12 s）: `frames=184` の貸した image の present、mview の SPIN fps 18.8（QEMU の lavapipe、参考）。
