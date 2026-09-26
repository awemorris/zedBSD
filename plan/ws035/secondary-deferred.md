<!-- awesome-plan project=zedbsd record=ws035-secondary-deferred -->

# secondary queue sq001 で後回しにした範囲外の修正

サブエージェントが見つけた、範囲外（kernel・HAL・libc・uapi・GPU driver・他の WS）の問題と要る修正。メインが Bug・Future Work・担当 WS へ移す。

| 日付 | Phase | 問題 | 要る修正（場所） | 影響（止まった Phase） |
| --- | --- | --- | --- | --- |
| 2026-09-25 | p052 | libvulkan の公開 API の `vkAllocateMemory`（`VkImportMemoryFdInfoKHR`、OPAQUE_FD）が、Wayland の WSI が送る画像の fd（`GPU_RESOURCE_EXPORT`、image の schema）を受けない（`VULKAN_MEMORY_FD_SCHEMA` の fd だけを受ける）。設計 D2 の「OPAQUE_FD で import」が標準の API ではできない | `userland/base/libvulkan/memory.c` の `memory_import_fd` が image の schema の fd も受ける（`GPU_RESOURCE_IMPORT` で alias を作り、`vulkan_wsi_shared_image_import` と同じ検査をする）か、WSI（`wsi-image.c`）が memory の schema で export する。直ったら zwl の `import.c` を標準の呼び出しへ戻し、libvulkan の静的 link をやめる | 回避して進めた: zwl は libvulkan の object を静的に link し、内部の `vulkan_wsi_shared_image_import` を呼ぶ（`gpu-share-test` と同じ形）。zwl の実行 file が libvulkan を丸ごと持つ |
| 2026-09-25 | p052（試験環境） | 試験の host（centris）の Debian の virglrenderer 1.1.0 では、libvulkan が physical device を 1 つも見つけない（strict queue・native quiescence の拡張が無い）。llvmpipe の GL では QEMU が blob の scanout の readback で落ちる | 範囲外の修正は無い。WS014 の strict queue の virglrenderer を `build/ws035-sq-venus/` へ写し、GL は zink（Lavapipe）にした（`plan/ws035/tests/zdesktop-guest.sh`）。メインが Lavapipe の Venus の受け入れ環境（ws034 の決定）を正式に作るときの参考 | 無し（回避済み） |

2026-09-26（q457、ws035-p052）: 1 行目（libvulkan の OPAQUE_FD の import）は `userland/base/libvulkan/memory.c` で解決した（画像の capability の fd も受ける）。zwl は標準の `vkAllocateMemory` で import し、libvulkan を静的に link していない。
