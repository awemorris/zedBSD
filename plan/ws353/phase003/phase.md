<!-- awesome-plan project=zedbsd record=ws353-p003 -->

# ws353-p003: libvulkan の hotplug の fence と、未提出の fence の待ち

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.1・§2.2・§4.1・§4.2。

## 目的

`vkRegisterDeviceEventEXT` の fence を p002 の kernel の fence で明け、`event_hotplug_poll` と 1 ms の見回りを消す。未提出の fence を含む待ちも 1 ms で回さない（compositor の hotplug の thread の停止の fence が要る）。

## 変える file

- `userland/desktop/libvulkan/wsi-display-control.c`: `vkRegisterDeviceEventEXT` は renderer の display node ごとに p002 の ioctlで fd を得る（`GPU_DISPLAY_FENCE`、事象 `GPU_DISPLAY_FENCE_HOTPLUG`）。`event_hotplug_poll` を消す。
- `wsi-display-nodes.c`: node ごとの ioctl の口（今の `vulkan_wsi_display_node_ioctl` に倣う、HOTPLUG の fd を作る関数）。`vulkan_wsi_display_node_topology` は他に使う所が無ければ消す。
- `sync-internal.h`・`sync.c`: `struct vulkan_sync` に事象の fd の配列。状態は fd の poll（timeout 0）、待ちは fd を poll の集合に（external fence と同じ扱い、missing にしない）、all の待ちの事象の fence の枝。reset・destroy で CANCEL と close。未提出の fence は `vulkan_wake` を待ちに入れ、`vkQueueSubmit`・`vkResetFences`・import は**その fence を待つ token だけ**を起こす（token に待つ fence の集合を持たせる。今の `vulkan_wake_notify` は device の全部の token を起こすので、render の submit のたびに hotplug の thread が起きる。設計 §4.2、レビュー M4）。device・context の error は今のまま全部を起こす。事象の fence の待ちも token を登録する（device の error で `VK_ERROR_DEVICE_LOST` を返す。レビュー M3）。token の登録と drain は最初の観測（`device->mutex` の下の notification の写し、sync.c:1612〜1630）の前、poll はその後（lost wakeup を作らない。レビュー L3）。
- `objects.c`・`internal.h`: `struct vulkan_wake` に待つ物（fence の集合）、fence の知らせの関数（例 `vulkan_wake_notify_fence`）。
- `external-fence.c`・`queue.c`: 要る所だけ（知らせの呼び出し）。

## 残す振る舞い

- 提出した fence の待ち（`GPU_COMMAND_WAIT`・context の fd）、external fence、swapchain の acquire の待ちは変えない。
- REFRESH の fence（`vkRegisterDisplayEventEXT`）は今のまま（p014）。だから `vulkan_display_event_poll` と `VULKAN_SYNC_POLL_NS` はこの Phase では REFRESH のためだけに残る。
- b05187933a（POLLPRI の待ち）は採らない（main に入っていればこの Phase で消す）。

## 先に確かめる事

- `vkQueueSubmit(queue, 0, NULL, fence)` が fence を signal する経路: queue.c:223〜255 の `vulkan_queue_idle` が同じ形を使っている（レビューで確かめ済み）。
- libvulkan の display node の open が process に一つか（複数の `VkDevice` が共有するか。kernel の登録の上限 16 の数え方に効く。レビュー L12）。

## 確かめ

- build: libvulkan（x86_64・arm64・riscv64）warning 0。
- QEMU（Venus）: `plan/ws353/tests/` の Vulkan の小さな program と 1 本の script: (1) hotplug の fence を `vkWaitForFences(UINT64_MAX)` で待ち、venus-head.sh の抜き差しで 1 s 以内に返る。(2) 抜き差しの無い 30 s に program の thread の起床が無い（WS352 の poll の診断か、program の中の wait の回数）。(3) hotplug の fence と未提出の fence の any の待ちが、別の thread の empty submit で返る。(4) reset・destroy の後に kernel に登録が残らない。(5) hotplug の fence と未提出の fence の any の待ちが、別の thread の**別の fence** の submit では起きない（token の絞り、待ちの回数で数える）。(6) 待ちの直前に別の thread が submit を何度も繰り返しても、待ちが落とさず返る（L3）。(7) device の error（試験の program から `vulkan_device_error` の経路を起こせる形、例えば render node の job の打ち切り）で、事象の fence と未提出の fence の待ちが `VK_ERROR_DEVICE_LOST` で返る（M3）。
- 既存の libvulkan の試験（あれば fence の試験、QEMU の vkcube など Q1 の割当の物）で回帰が無い。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p002。
