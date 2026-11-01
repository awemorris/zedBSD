<!-- awesome-plan project=zedbsd record=ws342 -->

# WS342 Windows 版の libvulkan（libvulkan-win32）と Keiland の Windows の backend の compositor（libkeiland-backend-win32）

- Master: [plan/master.md](../master.md)（2026-11-02 07:18 作成）。Primary Milestone: MG006。
- Status: planning（着手しない。ユーザーとの議論が続く）
- 目標:
  - `userland/desktop/libvulkan-win32/`: Windows 版の libvulkan.dll。Linux の libvulkan-compat と同じく、Windows に入っている Vulkan（vulkan-1.dll）を呼ぶ橋渡し。fd の受け渡しを Windows の handle の受け渡し・共有（VK_KHR_external_memory_win32・external_semaphore_win32・external_fence_win32 の NT handle、DuplicateHandle）に置き換える。Vulkan の display の部分は Win32 の API（窓と VK_KHR_win32_surface の swapchain）で作る。Vulkan Video は system の Vulkan の Vulkan Video へそのまま渡す。
  - `userland/desktop/libkeiland-backend-win32/`: Keiland の compositor の Windows の backend。根の描画域を 1 つの窓にし、その窓は Windows の DWM を通して描かれる（根で合成して DWM にさらに合成させてよい）。

## ユーザーの決定

- （2026-11-02 07:18）ユーザーから、上の 2 つ（libvulkan-win32 と libkeiland-backend-win32）を作る WS を立てる、まだ議論が要るので着手はしない、という趣旨の指示を受けた。
- （2026-11-02 07:18）DWM に client の窓を直に合成させる道（D3D11・D3D12 の共有の texture を VK_KHR_external_memory_win32 で Vulkan に取り込んで描き、DirectComposition の visual として DWM に渡す）ができるなら、全画面の直の scanout はひとまず要らない。ただし口だけ残す（VK_EXT_full_screen_exclusive・独立 flip・VK_NV_acquire_winrt_display の類）、という趣旨の決定を受けた。
- （2026-11-02 07:18）Vulkan と D3D の間の同期（shared fence・keyed mutex）は Intel・NVIDIA・AMD の 3 社の driver に対応する、という趣旨の決定を受けた。

## 背景（2026-11-02 07:18 Q1 の見立て、未検証）

- Windows に dma-buf と sync_fd に当たる物は無い。NT handle の共有は同じ adapter（LUID）・同じ driver の中だけ。
- Windows の driver は VK_KHR_display をほぼ出さない。
- vulkan-1.dll と同じ名前は Khronos の loader とぶつかるので、名前は libvulkan.dll など別にする。
- 段の案: (1) libvulkan-win32、(2) 根の窓と Vulkan の swapchain の compositor、(3) D3D の共有の texture と DirectComposition で DWM に直に合成。

## 関係する WS

- [WS334](../ws334/ws.md)（OmniGrammaton の Windows 版、libkeiland の Win32 の実装）、[WS341](../ws341/ws.md)（仮想 GPU）、WS309（Keiland の compositor の thread と game mode）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws342-p001 | 調べと設計: handle の受け渡しの wire、同期（3 社）、DirectComposition の道、口だけ残す全画面の道、build（llvm-mingw）、試験の道（Wine・本物の Windows） | planning | ユーザーとの議論 |

## Resume point

- 2026-11-02 07:18 Q1: 作成。ユーザーとの議論の後に p001 を割り当てる。
