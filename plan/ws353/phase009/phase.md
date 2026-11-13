<!-- awesome-plan project=zedbsd record=ws353-p009 -->

# ws353-p009: libvulkan の present の完了（VK_KHR_present_id・VK_KHR_present_wait）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.5・§4.3。

## 目的

display の swapchain に、present が画面に出た事を待つ標準の口を足す。compositor が frame の callback と animation を表示の完了で進めるため（ユーザーと合意した形）。

## 変える file

- `include/libc/vulkan/vulkan_core.h`: Khronos の header の `VK_KHR_present_id`・`VK_KHR_present_wait` の型・struct・PFN・prototype（同じ header の版から写す、許諾は header のまま）。
- `userland/desktop/libvulkan/`: `wsi-swapchain.c`（present の job に id、worker が kernel の present の成功の後に `presented_id` を上げて `swapchain_notify_locked`、`vkWaitForPresentKHR` は swapchain の cond と wake の token で block し、out of date・lost・retire で error で返る）、`device.c`・`instance.c`（extension と feature）、`api-commands.tsv`・`dispatch-table.inc`・`commands-generated.inc`・`exports.map`（`tools/maintain-dispatch.noct` の手順。共有の `build/NoctLang` は build しない。無ければ手で同じ形に書き Q1 に言う）。
- Wayland の WSI（client 側）には足さない（extension を出すのは display の surface だけにするか、Wayland では present を受けた時点で返すかを Phase で決め、理由を書く）。

## 残す振る舞い

- FIFO の present と acquire の意味、cursor・overlay の plane の present。

## 確かめ

- build: libvulkan warning 0。host: id の順と error の時の返りの試験（swapchain の worker を偽の adapter で動かせるなら）。
- QEMU（Venus）: 小さな program で display の swapchain に 100 frame を present し、各 `vkWaitForPresentKHR` が返る、返りの間隔が refresh に合う、出力を抜いた時に error で返る（venus-head.sh）。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p003（同じ libvulkan の範囲の後）。
