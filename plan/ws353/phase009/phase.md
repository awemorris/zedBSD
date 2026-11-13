<!-- awesome-plan project=zedbsd record=ws353-p009 -->

# ws353-p009: libvulkan の present の fence（VK_EXT_swapchain_maintenance1）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.5・§4.3・§4.2。

## 目的

compositor の別の thread（shown、T17）が window mode の frame の present の完了を待てる標準の口を足す。`vkWaitForPresentKHR` は swapchain の外部同期を要り、render が acquire・present している swapchain を別の thread が待てない（設計 §2.5）ので、fence で待てる `VK_EXT_swapchain_maintenance1` の present の fence にする（2026-11-13 q2056-i02 で present id・present wait から変えた）。

## 変える file

- `include/libc/vulkan/vulkan_core.h`: Khronos v1.3.269 の header から `VK_KHR_get_surface_capabilities2`・`VK_EXT_surface_maintenance1`・`VK_EXT_swapchain_maintenance1` の型・struct・enum・PFN・prototype を写す（版は `API-PROVENANCE.md` が固定している物と同じ。許諾は header のまま）。
- `include/libc/vulkan/API-PROVENANCE.md`: 足した extension の範囲を書く（入力の header の版・SHA-256 は今のまま）。`include/libc/vulkan/LICENSE-API`: 足した宣言が今の Apache-2.0 の範囲に入る事を確かめ、要れば範囲の記述を足す（レビュー L10）。
- `userland/desktop/libvulkan/`:
  - `wsi-swapchain.c`: `VkSwapchainPresentFenceInfoEXT` を present の job に持たせ、worker が native の present（display の kernel の present、Wayland の commit）の後に、成功・失敗のどちらでも fence を signal する。job が積まれずに捨てられる時（retire、device lost）も signal（lost）。同じ swapchain の fence は job の順。`vkQueuePresentKHR` が OOM で積めなかった時は fence を変えない。`VK_ERROR_OUT_OF_DATE_KHR`・`VK_ERROR_SURFACE_LOST_KHR` の時は積んだと見なし fence は signal する。`VkSwapchainPresentModesCreateInfoEXT`・`VkSwapchainPresentModeInfoEXT`（FIFO だけ）、`VkSwapchainPresentScalingCreateInfoEXT`（scaling 無し）、`VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT`（受けて、今どおり create で確保）、`vkReleaseSwapchainImagesEXT`（acquire して present していない image を available に）。
  - `swapchain_notify_locked`（wsi-swapchain.c:4830〜4840）の `vulkan_wake_notify(NULL, NULL)` を、その swapchain を待つ token だけを起こす形に（p003 の token の待つ物に swapchain を足す。cursor の acquire の待ちが render の present で起きない。設計 §4.2、レビュー M4）。
  - `wsi.c`・`wsi-display-control.c`: `vkGetPhysicalDeviceSurfaceCapabilities2KHR`・`vkGetPhysicalDeviceSurfaceFormats2KHR` と、`VkSurfacePresentModeEXT`・`VkSurfacePresentScalingCapabilitiesEXT`・`VkSurfacePresentModeCompatibilityEXT`（FIFO だけ）。
  - `device.c`・`instance.c`: extension の一覧と feature `swapchainMaintenance1`。
  - `api-commands.tsv`・`dispatch-table.inc`・`commands-generated.inc`・`exports.map`（`tools/maintain-dispatch.noct` の手順。共有の `build/NoctLang` は build しない。無ければ手で同じ形に書き Q1 に言う）。
- `VK_KHR_present_id`・`VK_KHR_present_wait` は足さない。

## 先に確かめる事

- v1.3.269 の header に 3 つの extension の宣言がある事（無ければ止まって Q1 に報告。版を上げるのは別の判断）。
- 仕様（Vulkan-Docs の chapters/VK_KHR_surface/wsi.adoc の common_present_fence と VkSwapchainPresentFenceInfoKHR、vkQueuePresentKHR の失敗の節）と、上の振る舞いが合う事。feature を出す時に extension の全部が要る事（一部だけは出さない）。

## 残す振る舞い

- FIFO の present と acquire の意味、cursor・overlay の plane の present、`vkDestroySwapchainKHR` が worker の job の終わりを待つ事（wsi-swapchain.c:1634）。

## 確かめ

- build: libvulkan（x86_64・arm64・riscv64）warning 0。host: 偽の adapter で worker を動かせるなら、fence の順、失敗・retire・device lost で fence が signal する事、OOM で変わらない事。
- QEMU（Venus）: 小さな program で display の swapchain に 100 frame を present し、別の thread が各 present の fence を `vkWaitForFences(UINT64_MAX)` で待って全部返る、返りの間隔が refresh に合う、出力を抜いた時（venus-head.sh）に待ちが返る、swapchain の破棄の後に待ちが残らない。Wayland の swapchain（compositor の中の client）でも fence が返る。
- 既存の libvulkan の試験（vkcube など、Q1 の割当）で回帰が無い。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p003（同じ libvulkan の範囲、token の待つ物の形）。
