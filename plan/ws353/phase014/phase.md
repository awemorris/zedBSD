<!-- awesome-plan project=zedbsd record=ws353-p014 -->

# ws353-p014: refresh の事象の fence（kernel の push）、1 ms の見回りを libvulkan から消す

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §3.2・§4.4、対応表の 4-01。

## 目的

`vkRegisterDisplayEventEXT`（FIRST_PIXEL_OUT）の fence も kernel が refresh の境で signal する形にする（ユーザーの決定 2 の「同じ形の display の event」）。`vulkan_display_event_poll` と `VULKAN_SYNC_POLL_NS` を消す。

## 変える file

- kernel: `include/drivers/gpu/gpu.h` に `drv_gpu_display_refreshed(device, display_id, generation, sequence)` と display の op `refresh_arm(private, display_id, on)`（任意、登録がある間だけ vblank の割り込みを有効に）。`gpu.c` の REFRESH の登録（p002 の ioctl の EOPNOTSUPP を外す）。`GPU_CAP_DISPLAY_CONTROL` の driver（i915 の display、amdrdna、nvrtx、venus、他にあれば）の vblank の処理で push。
- libvulkan: `wsi-display-control.c` の `vkRegisterDisplayEventEXT` を REFRESH の fd に、`event_refresh_poll`・`event_refresh_base`・`vulkan_display_event_poll` を消す。`sync.c` の `VULKAN_SYNC_POLL_NS` と、事象の観測の枝を消す（`vulkan_sync_pause` の他の利用者を数え、残る物は理由を書く）。

## 残す振る舞い

- `GPU_DISPLAY_REFRESH`（同期の待ち）の ioctl は今のまま（使う所があれば）。登録の無い間は vblank の割り込みを増やさない。

## 確かめ

- build warning 0。QEMU（Venus）: 小さな program で refresh の fence を 100 回、間隔が refresh に合う、登録の無い間に driver の vblank の push が起きない（診断で数える）。実機（i915）は p016 で。

## 受け入れ

- 上の判定が PASS。libvulkan に周期の見回りが無い（grep）。結果・SHA・未実施を書く。

## 依存

- p003。
