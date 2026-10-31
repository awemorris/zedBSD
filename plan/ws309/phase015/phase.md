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
