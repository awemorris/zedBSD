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

### 再開点（2026-10-31 16:39 P2、Q1 の指示でラップアップ: host の再起動）

- 状態: in-progress（未 cleared）。branch `agent/p2-ws309v2`。main の merge は 0747be69a（6dcfbfaef = P3 の I1 core を含む main）。
- 済み・確かめ: libvulkan の export・Wayland の lend・kl_game protocol・compositor の貸し出し・T4・暫定の経路の撤去（scanout-zedbsd.c の git rm、v1-check の例外を外した）、
  lend の能力の確かめ（window の chain の export）、`GPU_IMAGE_LAYOUTS` の選び。build（amd64・VF2）warning 0、v1-check PASS、vulkan-overlay-host PASS、host-scanout-rules PASS、
  QEMU の game-mode-cycle 6 回の実の往復（上の節。道具の判定は 1 回目の時間の都合で FAIL）。
- 途中（この commit、未 build・未確かめ）: compositor の側の LINEAR でない取り込み（P3 の design §9 の 8）。`include/libc/vulkan/vulkan_zedbsd.h`（新、VkImageLayoutZEDBSD、
  sType 0x7a650001）、libvulkan の vkCreateImage が record を覚え GPU_CAP_IMAGE_LAYOUTS の無い device では断る、`dedicated.c` が LINEAR でない記述に OPTIMAL・record・usage を
  確かめる、`gpu-zedbsd.c` の decode が LINEAR でない記述を受ける、`gpu-buffer-zedbsd.c` の buffer_image が OPTIMAL と record と記述の usage。
  build が host の負荷で止められ、まだ通していない。次: build（下）と `plan/tools/gpu-boundary/run-dedicated-host.sh`（LINEAR でない case を足す）。
- 未実施: VF2（password 待ち、Q1 がユーザーに聞く）、RTX（10.0.10.3 は Q1 に聞いてから）、scanout-guest.sh（forge の image は greeter が display を持つ）、
  QEMU の道具の PASS の取り直し、Pi 4 の game mode（未確かめ、能力の確かめで入らない形）、nvrtx の block-linear（P3）。
- 再開の command:
  1. build: `make -j16 ZEDBSD_CONFIG=plan/ws309/tests/config-amd64-p1.mk BUILD=build/p015 build/p015/dynamic/libvulkan.so build/p015/bin/wayland`、
     `sh plan/tools/gpu-boundary/v1-check.sh`、`sh plan/ws214/tests/vulkan-overlay-host.sh /tmp/p2/oh`、`sh plan/tools/gpu-boundary/run-dedicated-host.sh`。
  2. QEMU: `plan/tools/titlebar/build-menu-image.sh build/p015menu`（flock /tmp/zedbsd-image-build.lock）、
     `GUEST_RUNTIME=$PWD/build/p015run VENUS_RENDERER=/home/awe/zedBSD-claude1/build/ws035-sq-venus/install plan/tools/scanout/game-mode-cycle.sh build/p015menu/hdd-image.img OUT 6`
     （host の負荷の低い時に。1 回目の mview の起動が遅い時は道具の 40 秒の待ちを超える）。手での 1 回は build/p015save/run-m.sh（guest の /tmp に置き `sh /tmp/run-m.sh NAME`）。
  3. VF2（Q1 の割り当てと password の後）: 前（main）= `VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=14C19FA80F86 OLD=p2old TIMING=1 KEEP=1 COPIES=1 SPIN=12 sh plan/ws291/tests/vf2-mview.sh OUT-c1 build/tmp/p015base/build/vf2t`
     と COPIES=13 を別の run（BUG-375 の前の注意どおり 1 つずつ）、後 = 同じ command の BUILD を build/p015vf2t（どちらも `-DPOWERVR_SUBMIT_TIMING=1`）。最後は KEEP を外し元の kernel に戻して cksum で照合。
     WSI の copy の無い事は kernel の `powervr: timing: transfer:` の数（前後で比べる）と compositor の `KWL SCANOUT direct=0 ... frames=N` で見る。
     注: build/p015vf2t は 0b48946c0 の時点（SAMPLED の直し a3e740841 と adoption 5 秒の前）なので、作り直す:
     `make -j8 ZEDBSD_CONFIG=plan/ws291/tests/config-vf2.mk BUILD=build/p015vf2t ZEDBSD_TEST_CPPFLAGS=-DPOWERVR_SUBMIT_TIMING=1 build/p015vf2t/vmunix build/p015vf2t/bin/wayland build/p015vf2t/bin/mview build/p015vf2t/dynamic/libvulkan.so build/p015vf2t/dynamic/libwayland-client.so`。
- 保存: QEMU の log は build/p015save/（cycle5）と build/review/ws309/。消してよい物（Q1 へ）: build/tmp/p015base（基準の tree、VF2 の後）、build/p015img・build/p015run。

## q1782-i02（2026-10-31 16:58〜、P2、同じ worktree、main ea4317337 を 1405718fc で merge）

- 2026-10-31 ユーザーから、RTX と VF2 で zero copy を今入れる、VF2 の後に板を Pi 4・Pi 5 に替えて確かめる、という趣旨の指示を受けた（Q1 経由）。
  受け入れの 4 つ: RTX で動き確かめる、VF2 で動き確かめる、Pi 4・Pi 5 の build を用意する、VF2 の合格で main に merge。

### 段 1・2（build・host・QEMU）

- 2af4234dd: LINEAR でない取り込みの build を通した（`resources.c` に `<uapi/gpu.h>`）。`gpu-zedbsd.c` は tiling 0 を断る。host の試験に block-linear の case を足した
  （`run-dedicated-host.sh` 9 case・`-I$repo`、`run-gpu-zedbsd-host.sh` 4 case。後者は 10-13 から Vulkan の header の `//` で `-std=c89` が通らず壊れていたので c99 にした）。
  確かめ: amd64 の libvulkan・libwayland-client・wayland warning 0、v1-check・vulkan-overlay-host・host-scanout-rules・dedicated-host・gpu-zedbsd-host PASS、nvrtx-vk 94/0、vk-d3 225/0。
- f95abf380: `game-mode-cycle.sh` の待ち 40 秒 → 120 秒（1 回目の mview の model の読み込み）。libvulkan の診断 `VULKAN_ZEDBSD_WSI_LOG=1`（窓・display の chain ごとに
  `VULKAN WSI chain=N lent= direct= images= tiling= parameter=`、終わりに `presents= copies=`）。
- QEMU（Venus、`build/p015menu`、`game-mode-cycle.sh … 6`）: **PASS**。6 回とも direct=1・lend images=0x3f・貸した image への present 94〜106・window へ戻る、FAILED/MODE_ERROR 0。
  log: build/review/ws309/p015-cycle-i02*。

### 段 3（RTX 2070、10.0.10.3、`plan/ws309/tests/rtx-p015.sh`、17:15〜17:37）

- kernel 2917063051（main ea4317337 と branch で同じ）。腕: base = main の 14 file、new = branch（`nvrtx.disp.bl` 無し）、bl = branch と `nvrtx.disp.bl=1`。証拠 build/review/ws309/rtx-p015-i02/。

| fps | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| base（main） | 746.0 | 110.6 | 198.5 | 21.08 | 156.3 | 50.06 |
| new（LINEAR を貸す） | 508.9 | 112.0 | 68.4 | 7.42 | 154.2 | 49.24 |
| bl（block-linear を貸す） | 526.5 | 110.9 | 192.3 | 19.61 | 153.9 | 49.87 |

- bl の腕: game chain `lent=1 direct=1 tiling=0x10001`（NV block-linear）、WSI の copy 0（subs-100 1919 present、big-1000 1223 present）、kernel `block-linear scanout on`・
  `first flip 1920x1080`、nvrtx の quads（5 秒ごと）の定常は 0（base は約 1000・600、new は 61000〜69000）。game ↔ window の往復は 3 腕とも 3/3（direct=3 left=3 window=3、failed 0、refused 0）。
- new の腕の低下は、LINEAR の貸した image に nvrtx が pass ごとに block-linear の影から copy するため。よって lend の規則を足した（下）。
- big-100 は貸す 2 腕で約 30% 低い（60 Hz を大きく超える mailbox の run、frame ごとの release の往復が見込み、未分析）。
- 戻し: 14 file を kept の cksum に、NVMe の vmunix 1463007375、cfg の nvrtx.disp.bl 0 行、Debian（RTX 4 function vfio-pci）。script の expected の行の誤りを d90bf2d15 で直した。
- lend の規則（d90bf2d15 の次）: display の chain の image は、renderer の最も速い並び（一覧の先頭）の時だけ外に出す（`vulkan_wsi_layout_choose` の fastest、
  `vulkan_wsi_swapchain_image_memory` が遅い chain を断る → 能力の確かめが lendable=0）。nvrtx の bl=0 は全画面を window mode の合成にする。VF2・Pi・Venus は LINEAR が先頭なので貸す。
  host 試験 PASS、amd64・RTX の build warning 0。その RTX の再計測（new・bl の 2 腕）は Q1 の指示（ユーザーの目視を先に）で new の腕の再起動の途中で止めた（未計測）。
- 目視の準備（17:4x）: branch の 14 file と kernel、`nvrtx.disp.bl=1`、desktop で `block-linear scanout on`・`first flip`（window mode の chain も block-linear）、lendable=1。
- ユーザーの目視（2026-10-31 18:1x、Q1 経由）: **不合格**。画は出るが block ごとに崩れてずれる（文字・icon が小さな block に刻まれ、desktop の window mode の chain も同じ）。
  写真 /home/awe/zedBSD-claude1/build/review/ws291/p023-bl-visual-1.jpg。Q1 の決定: nvrtx の既定は変えない（`nvrtx.disp.bl` は既定 0 のまま）、lend の規則は残す
  （bl=0 の RTX は貸さない）。block-linear の scanout の直しは P3（WS291 p023）。機械は branch と `nvrtx.disp.bl=1` のまま P3 へ渡した（deploy と戻しの手順は Q1 に送った）。
  よって RTX の文字通りの zero copy は、mview の側（lent=1・tiling 0x10001・copy 0・quads 0）は確かめたが、画面の正しさは未達（P3 の scanout 待ち）。→ P3 の直しの後の 2 回目の目視で合格（下の節）。

### RTX の 2 回目の目視と既定の変更（2026-10-31 18:3x〜19:07）

- P3 の直し（effb3d149、block-linear の kind の ctxdma）の kernel 3845779468 と branch の 14 file・`nvrtx.disp.bl=1` で、ユーザーの 2 回目の目視は**合格**（desktop が正常、Q1 経由）。
- Q1 の許可（P3 の領域の 1 つの既定の変更）で `nvrtx.disp.bl` の既定を 1 にした（16cd986f0、`stage.c` の token を `nvrtx.disp.bl=0` で切る形に、`share.c`・`nvrtx-node.h`・
  `nvrtx-private.h` の comment）。その前に main f42bb6d3b を merge（cf75c4ec8）。確かめ: RTX の組（plan/ws291/tests/config.mk）と config/ci/config-amd64.mk の vmunix warning 0、nvrtx-vk 94/0。
- 再計測（boot の parameter 無し、`rtx-p015.sh` の base:main ea4317337 の file と kernel 2917063051、new:branch と kernel 3084829296、証拠 build/review/ws309/rtx-p015-i02c/）:

| fps | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| base | 686.5 | 112.0 | 199.2 | 20.21 | 159.3 | 50.26 |
| new（既定で block-linear） | 543.3 | 108.2 | 188.4 | 19.91 | 155.6 | 49.96 |

  - new: kernel `block-linear scanout on`・`first flip 1920x1080`（cfg の行 0）、lendable=1、game chain `lent=1 direct=1 tiling=0x10001`、copies 0（1850・1223 present）、
    nvrtx の quads の定常 0（base は 975・589）。往復 3/3（両腕、failed 0、refused 0）。
  - 画面の撮影（keiland-shot、desktop・Notes・browser）: base との差は 3 枚とも右上の時計の 55 画素だけ（09:49 と 09:59、他は同一）。keiland-shot は GPU で読むので scanout の正しさはユーザーの目視が根拠。
  - desktop（frame-ops）: launch 63→64、type 61→61、output 67→67、drag 61→60、home 63→63、wiseview 65→66 fps（後退なし）。
  - big-100 の差（1.46 → 1.84 ms）の段: acquire 0.03→0.08、record 0.67→0.65、submit 0.32→0.33、present 0.04→0.05、fence 0.36→0.55、段の外 約 +0.15。
    release の往復（acquire）は 0.05 ms だけで、主は fence（前の frame の GPU の完了の待ち）と段の外。前の見込み（release の往復が主）は誤り。原因は未特定。
- 戻し: 14 file を元の cksum に、NVMe の vmunix 1463007375、cfg の nvrtx.disp.bl 0 行、Debian（RTX 4 function vfio-pci）。

### 段 4（VF2、kei@10.0.30.2、`plan/ws309/tests/vf2-p015.sh`、18:18〜18:25）

- password は env だけで渡した（file・log に無し）。timing の kernel を作り直した（build/p015vf2t、6f9cbfef8、`-DPOWERVR_SUBMIT_TIMING=1`、vmunix 1381433670、warning 0）。
  base = build/tmp/p015base/build/vf2t（この worktree の 16:0x の基準の tree の timing の build、kernel 3302329568。その compositor は game mode を backend の errno=21 で断り合成する）。
  c1・c13 はそれぞれ別の compositor（root、自分の socket）。証拠 build/review/ws309/vf2-p015-i02/。

| | c1 fps | c13 fps | kernel の transfer（run の合計） | WSI の chain |
| --- | --- | --- | --- | --- |
| base | 73.58 | 41.89 | c1 963、c13 518（frame ごと） | （診断の無い libvulkan） |
| new | **141.25** | **52.50** | c1 161、c13 154（最初の 1 期間だけ、game の期間は 0 行） | lent=1 direct=1 tiling=0x1、copies 0（1680・621 present） |

- frame ごとの GPU の copy（transfer の job）は無い: new の c13 の 2 期間目に transfer の行が無い（base は 2 期間とも 306・212）。最初の期間の 154 は window mode の合成の frame
  （chain=1 の presents 10 copies 10）と model の upload の見込み（内訳は未確かめ）。
- c13 は 52.50 fps で ws291-p020 の目標（50）を越えた（direct の 42.35 から）。
- game ↔ window の往復（BUG-375 の道、1 つの compositor で 8 回）: new は 8/8 `MVIEW DONE`、direct=8 left=8 window=9、refused 0、failed 0、EBUSY 0。base は game mode に入らず（refused 7）。
  script の「compositor alive」の確かめは user の ps が root の process を見ないので 0 と出た（誤り、`kill -0` に直した）。8 回目の後の log は通常の PERF の行で終わる。
- 戻し: 板の kernel 2825517684（vmunix.p2old と同じ）を戻し、読み戻して一致、板は zedBSD で起動。

### Pi 4・Pi 5 の用意

- `plan/ws309/tests/config-rpi4-p015.mk`（CI の Pi 4 に mview・Ethernet の DHCP・root の SSH の鍵）、Pi 5 は `plan/ws291/tests/config-rpi5.mk`。image: build/p015rpi4/hdd-image.img、
  build/p015rpi5/hdd-image.img（e0a6a4f40 で作り直した: Pi 4 sha256 ac9cce142c4a97d9…、Pi 5 e55f31fceae1b6b8…）。mview は arm64 の package の一覧に無いので target を名指して build（qs40 の model は script が送る）。
- 試験の script `plan/ws309/tests/pi-p015.sh`（PI=address PROGRAMS=build、compositor を自分の socket で、mview の game を CYCLES 回、WSI の log、VIDEO で videoplayer の全画面 15 秒と
  firmware の decoder の kernel の行）。動画は build/p015video/high-{1080,720}p30.mp4（ffmpeg の testsrc2、H.264 High）。

### 再開点（2026-10-31 18:3x、P2 q1782-i02）

- 済み: QEMU PASS、VF2 PASS（c13 52.5 fps、copy 0、往復 8/8、板を戻した）、RTX は貸し出しの側（block-linear の lend、copy 0、quads 0、往復 3/3）まで。branch の先頭 e0a6a4f40 を Q1 の merge へ。
- 未達: big-100 の貸す時の約 20% の低下（fence の待ちと段の外、原因は未特定）、Pi 4・Pi 5（板の入れ替え待ち）、
  Pi 4 の全画面の動画で firmware の decoder が止まらないかの確かめ。
- Pi の手順: 板が guest の鍵の root で zedBSD を動かしていれば、
  `PI=<addr> PROGRAMS=build/p015rpi4 VIDEO=build/p015video/high-1080p30.mp4 sh plan/ws309/tests/pi-p015.sh OUT`（Pi 5 は `PROGRAMS=build/p015rpi5`、VIDEO 無しでよい）。
  そうでなければ上の image を card（Pi 5 は NVMe）に書く（Pi 5 の 1080p は `plan/ws291/tests/rpi5-image-boot.sh`）。合否: probe の lendable=1、game の 3 回とも `lent=1 … copies=0`、
  compositor の direct=1・window へ戻る・refused 0。Pi 4 の動画: PLAYBACK failed・came back unread・not answered・no firmware decoder が 0、shown が止まらない。止まる時は Pi 4 の
  content=video を game mode から外し、ここに記録する。
- 消してよい物（Q1 へ）: build/p015rtxd、build/tmp/rtx-p015-i02b、build/tmp/p015base-ea（RTX の後）、build/tmp/p015base（VF2 の後）、build/p015img・build/p015run。
