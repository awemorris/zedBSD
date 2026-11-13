<!-- awesome-plan project=zedbsd record=ws353-p003 -->

# ws353-p003: libvulkan の hotplug の fence と、未提出の fence の待ち

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.1・§2.2・§4.1・§4.2。

## 目的

`vkRegisterDeviceEventEXT` の fence を p002 の kernel の fence で明け、`event_hotplug_poll` と 1 ms の見回りを消す。未提出の fence を含む待ちも 1 ms で回さない（compositor の hotplug の thread の停止の fence が要る）。

## 変える file

- `userland/desktop/libvulkan/wsi-display-control.c`: `vkRegisterDeviceEventEXT` は renderer の display node ごとに `GPU_DISPLAY_EVENT_FENCE`（HOTPLUG）で fd を得る。`event_hotplug_poll` を消す。
- `wsi-display-nodes.c`: node ごとの ioctl の口（今の `vulkan_wsi_display_node_ioctl` に倣う、HOTPLUG の fd を作る関数）。`vulkan_wsi_display_node_topology` は他に使う所が無ければ消す。
- `sync-internal.h`・`sync.c`: `struct vulkan_sync` に事象の fd の配列。状態は fd の poll（timeout 0）、待ちは fd を poll の集合に（external fence と同じ扱い、missing にしない）、all の待ちの事象の fence の枝。reset・destroy で CANCEL と close。未提出の fence は `vulkan_wake` を待ちに入れ、`vkQueueSubmit`・`vkResetFences`・import で `vulkan_wake_notify`。
- `external-fence.c`・`queue.c`: 要る所だけ（知らせの呼び出し）。

## 残す振る舞い

- 提出した fence の待ち（`GPU_COMMAND_WAIT`・context の fd）、external fence、swapchain の acquire の待ちは変えない。
- REFRESH の fence（`vkRegisterDisplayEventEXT`）は今のまま（p014）。だから `vulkan_display_event_poll` と `VULKAN_SYNC_POLL_NS` はこの Phase では REFRESH のためだけに残る。
- b05187933a（POLLPRI の待ち）は採らない（main に入っていればこの Phase で消す）。

## 先に確かめる事

- `vkQueueSubmit(queue, 0, NULL, fence)` が fence を signal する経路（queue.c の empty submit）。

## 確かめ

- build: libvulkan（x86_64・arm64・riscv64）warning 0。
- QEMU（Venus）: `plan/ws353/tests/` の Vulkan の小さな program と 1 本の script: (1) hotplug の fence を `vkWaitForFences(UINT64_MAX)` で待ち、venus-head.sh の抜き差しで 1 s 以内に返る。(2) 抜き差しの無い 30 s に program の thread の起床が無い（WS352 の poll の診断か、program の中の wait の回数）。(3) hotplug の fence と未提出の fence の any の待ちが、別の thread の empty submit で返る。(4) reset・destroy の後に kernel に登録が残らない。
- 既存の libvulkan の試験（あれば fence の試験、QEMU の vkcube など Q1 の割当の物）で回帰が無い。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p002。
