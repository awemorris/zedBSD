<!-- awesome-plan project=zedbsd record=ws353-design -->

# WS353 設計: compositor・libvulkan・kernel の待ち方の作り直し（2026-11-13 P1、q2056-i01）

- 親: [ws.md](ws.md)。ws.md の「ユーザーの決定」と「合意した形」が正。この文書はその具体化。
- 調べた tree: main 483bec3718（worktree `p1-ws353`）。file:line はこの tree。WS352 の branch `agent/p1-ws352`（基点 6aff283baa、82d2a01458）は別に読んだ。
- 土台: [WS309 の設計](../ws309/design.md)（thread の構成）。WS309 design.md §4.3 の「hotplug の 250 ms の tick は main に残す」と §3.3 の「poll の timeout は今のまま（10 ms）」はこの設計で改める。
- 2026-11-13 Q1 の指示（起動の途中）を反映した: 合成の完了を知らせる専用の thread を置かない（render が slot の数で回し、足りない時だけ最古の fence を自分で待つ）。frame の callback は present の完了。client の buffer の寿命のための待ちは、明示・暗黙の同期と比べて書く（§5.8・§8 の未決 3）。
- 敵対的レビュー: 未実施（Q1 が Fable で起動する）。

## 1. 原則

1. **idle は全 thread が timeout 無しで block する。** 起きるのは外の事象（入力・client の要求・hotplug・daemon の知らせ・GPU と display の完了）と、機能が予定した時刻だけ。
2. **変化は持ち主が push する。** 周期で読み直す code、取りこぼしの保険の timeout を置かない。事象が来ない時は通知の側を直す（Bug にする）。
3. **待つ物は一度に一つ。** 一つの thread が「仕事の依頼」と「GPU の完了」を交互に短い timeout で覗く形をやめ、その時に待つべき一つを無期限に待つ（render・cursor）。
4. **結果は wake で知らせる。** thread の結果を flag に置いて main に覗かせない。出口に積み、空から非空になった時だけ共通の wake pipe に 1 byte（WS309 §3.3 の `main_woken` の形）。
5. **締め切りは予定がある間だけ登録する。** 毎 pass に全部の機能の tick を呼んで期限を集め直す形をやめる。
6. **compositor は Vulkan だけに依存する**（WS309 決定 7）。kernel の新しい口は libvulkan が Vulkan の機能（fence・present wait）として出す形でだけ使う。fd は libvulkan と backend の中に閉じる。
7. **GPU の待ちは無期限。** GPU の止まりは kernel の job の監視（`CONFIG_GPU_JOB_EXECUTION_MS`）が device lost にするので、compositor は fence を timeout 付きで覗かない（終了と display の切り替えの打ち切りは別、§5.10）。

## 2. 今の実態（調べの結果）

### 2.1 display の事象の fence（hotplug・refresh）

- `vkRegisterDeviceEventEXT`（`userland/desktop/libvulkan/wsi-display-control.c:139`）は普通の fence を作り、登録の時の topology の番号（renderer の display node 全部の和、`wsi-display-nodes.c:194` `vulkan_wsi_display_node_topology`）を cursor に持つだけ。kernel は何も知らない。
- fence の状態は、問われるたびに libvulkan が各 node に `GPU_DISPLAY_EVENTS` を問い、和が cursor を越えたかで決める（`event_hotplug_poll`、wsi-display-control.c:324）。待ちは `sync_fences_wait` の「notification の無い fence」の枝で 1 ms ごとの見回り（sync.c:22 `VULKAN_SYNC_POLL_NS`、sync.c:1683〜1688）。
- refresh（`vkRegisterDisplayEventEXT` の FIRST_PIXEL_OUT）も同じ形で、問いは `GPU_DISPLAY_REFRESH` の timeout 0（`event_refresh_poll`）。compositor は使っていない（grep で 0 件）。
- compositor は main の毎 pass の `kwl_output_tick`（output-switch.c:145、`kwl_schedule` display.c:131 から）が 250 ms ごとに `vkGetFenceStatus`（output-switch.c:46・166〜178）。main は固定 10 ms の poll（main.c:1041・1110）。
- kernel: topology の番号は driver が持つ（例: i915 `hpd.c:694`・`1226` が `topology_sequence++` の後に名指し無しの `poll_notify()`）。gpu.c は driver の `display->events` の callback で読むだけで、変化を知らない（gpu.c:2501 `gpu_events_snapshot`）。open ごとの ACK と POLLPRI（gpu.c:2477〜2488、2538〜2625）は swapchain の acquire の待ち（wsi-swapchain.c:4781 `swapchain_poll`）が使う。
- `GPU_CAP_DISPLAY_EVENTS` を出す driver: i915（display.c・bootfb.c・capture.c）、venus、amdrdna、nvrtx、ghci、videocore の display。`events` の callback は 8 つ。

### 2.2 GPU の job の fence の知らせ

- 提出した fence は kernel の completion record（sequence）に結び付き、待ちは `GPU_COMMAND_WAIT`（その sequence で block、sync.c:1965）か context の fd の POLLIN（sync.c:2031）。周期は無い。
- 未提出の fence（作っただけ）を含む待ちは「missing」になり 1 ms の見回り（sync.c:1675・1683）。

### 2.3 kernel の fence object（`GPU_FENCE_*`）

- `src/drivers/gpu/gpu-fence.c` の payload は driver に依らない（spinlock・waitq・generation、fd の poll で signal の後 POLLIN、`fence_poll` gpu-fence.c:545）。job の完了で signal する bind も gpu.c の共通の部分（gpu.c:606〜620）。
- ただし ioctl は `GPU_CAP_FENCE` の device だけ（gpu.c:7368）。出すのは venus と ghci の vk-node だけ。i915・amdrdna・powervr・mali・videocore の render は出さない。だから export できる fence（OPAQUE_FD、external-fence.c）と client の acquire fence（kl_gpu_buffer_v1 rev 2）は QEMU（venus）だけで働き、5330（i915）では無い。

### 2.4 client の buffer

- **shm**（shm.c）: commit の後、damage の行を CPU で compositor の host image の staging に写し、写したらすぐ release。写しは「frame が飛んでいない間だけ」（staging を飛んでいる frame の copy が読むため。shm.c の先頭の説明、compose.c の `in_flight`）。
- **GPU の buffer**（`kl_gpu_buffer_v1` rev 3、gpu-buffer-zedbsd.c）: client の image を import して直に sample。
  - **acquire（明示の同期の口がある）**: rev 2 から commit に acquire fence（kernel の fence の fd と generation、`kl_gpu_buffer_v1_set_acquire_fence`）。compositor は main の poll に fd を入れ（main.c:1003〜1015・1080〜1095）、読めたら commit を使う（display.c:51 `kwl_fence_ready`）。fence を出せない device（i915 など）では client の WSI が描き終わりを待ってから commit する（wsi-wayland.c:793〜815）。
  - **release（同期の口が無い）**: protocol に release fence は無く、`wl_buffer.release` だけ。kernel に暗黙の同期（allocation ごとの読み手の追跡）も無い。release は compositor が「読んだ GPU の仕事が終わった」のを見てから送る。今は frame の slot に結び付いている: `compose_hold`（compose.c:4405〜4485）が slot の `held[]` に hold を積み、render の COMPLETED（slot の fence を render が 1 ms ごとに覗いて見つける）で `kwl_buffer_put`（compose.c:4829、objects.c:226）。frame の callback も同じ slot（`slot->callbacks`）で GPU の完了の時に送る。
- client の WSI（wsi-wayland.c）は release で image を再び使う。frame の進みの待ちは 50 ms の slice（wsi-wayland.c:24 `WAYLAND_WAIT_SLICE_MS`）と 1 s の pace の上限（`WAYLAND_PACE_NS`）。

### 2.5 present と表示の完了

- display の swapchain は FIFO だけ。`vkQueuePresentKHR` は swapchain の worker に job を積み、worker が GPU の完了を確かめてから kernel の present を同期で呼ぶ（wsi-display.c:1384 `display_present_image_sync`、kernel は選択と flush の後に返る）。前の front が外れると image は空き、acquire の待ち手が起きる（wsi-swapchain.c の `swapchain_notify_locked`）。acquire の待ちは fd で block（周期なし、wsi-swapchain.c:4700〜4730）。
- 表示の完了を app に知らせる口（`VK_KHR_present_wait`・`VK_KHR_present_id`）は無い。`include/libc/vulkan/vulkan_core.h` にも宣言が無い。

### 2.6 render と cursor の待ち

- render（render-thread.c）: 仕事が無い時は `changed` の cond を 100 ms の timedwait（`RENDER_WAIT_MS`、render-thread.c:25・675）。job が飛んでいる間は「最古の fence を 1 ms（`KWL_RENDER_POLL_NS`）待つ → main の依頼を見る」を繰り返し、acquire は 50 ms（`KWL_RENDER_ACQUIRE_NS`）か 1 ms。slot は `KWL_FRAME_SLOTS`＝`KWL_RENDER_FLIGHTS`＝2。
- cursor（cursor-thread.c）: box の cond を待つ（変化が無ければ無期限）。acquire は timeout 0 で、空きが無ければ 1 ms 後に再試行（`CURSOR_RETRY_US`）。main の copy（`KWL_CURSOR_COPIES` 2）の pin は copy の fence を `vkGetFenceStatus` で覗き、残る間 5 ms ごと（`CURSOR_PIN_POLL_US`）。移動の hold（refresh の終わり近くまで待つ）は締め切り。

### 2.7 main loop（main と branch）

- main: 固定 10 ms（2 ms・0 の場合あり）の poll。毎 pass、入力の scan（2 s）・ping・settings・system・IME・titlebar の tick、`kwl_schedule` の中の sleep・output・displays・glass の tick を呼ぶ。poll の集合は listener・client・wake pipe・client の acquire fence・backend（seat・/dev/system の事象・notify の bus）。
- WS352 の branch（未 merge）: 10 ms をやめ、各 tick が `kwl_due(server, at)` で次の期限を出し、poll の timeout はその最小（無ければ -1）。merge-tree で main と競合なしを確かめた。

### 2.8 build

- `userland/desktop/wayland/Makefile`・`Makefile.linux`・`Makefile.freebsd` は source を名指しで並べる。新しい file は Makefile の変更（ユーザーの承認）が要る。この設計は新しい file を作らず、既存の file に置く（§9）。

## 3. kernel

### 3.1 display の事象の fence（新しい口）

- uapi（`include/uapi/gpu-display.h`）に `GPU_DISPLAY_EVENT_FENCE`（display node、`GPU_CAP_DISPLAY_EVENTS`、読みの権限）を足す:

  ```c
  struct gpu_display_event_fence {
  	uint32_t version;
  	uint32_t size;
  	uint32_t flags;		/* GPU_HANDLE_CLOEXEC・CLOFORK */
  	uint32_t event;		/* GPU_DISPLAY_EVENT_HOTPLUG・GPU_DISPLAY_EVENT_REFRESH */
  	uint32_t display_id;	/* REFRESH だけ */
  	uint32_t reserved;
  	uint64_t generation;	/* REFRESH だけ: display の generation */
  	int32_t fd;		/* 出力: fence の fd */
  	uint32_t reserved2;
  };
  ```

- kernel は gpu-fence.c の payload（`drv_gpu_fence_create`、未 signal）を作り、fd を返し、その open（session）の登録の list に「事象・cursor・fence の参照」を入れる。cursor は kernel が登録の時に読む（HOTPLUG: その node の topology の番号、REFRESH: その display の refresh の数）。**登録を list に入れた後に番号を読み直し、cursor を越えていればその場で signal する**（driver の push と競っても取りこぼさない）。
- `GPU_CAP_FENCE` を要らない（payload は共通の部品で、job に結び付けない）。fd の待ちは普通の poll（POLLIN、失敗は POLLERR）。
- 取り消し: 同じ ioctl の flag `GPU_DISPLAY_EVENT_FENCE_CANCEL` と fd で、その open の未 signal の登録を外す（libvulkan の reset・destroy から）。open の close で全部外す。登録は open ごとに上限 16（越えたら ENOSPC）。
- device の offline・故障: 全登録の fence を error で signal する（待ち手は起き、libvulkan は signal と同じに扱い、compositor は列挙で失敗を見る）。

### 3.2 driver からの push

- 枠組みの関数（`include/drivers/gpu/gpu.h`、driver と gpu.c の間の口。HAL ではない）:
  - `void drv_gpu_display_changed(struct drv_gpu_device *device, uint64_t sequence);` — topology の番号が sequence になった。cursor < sequence の HOTPLUG の登録を signal して外す。今 driver が番号を上げた後に呼ぶ `poll_notify()`（POLLPRI の待ち手のため）は、この関数の中で呼ぶ（driver からは消す）。
  - `void drv_gpu_display_refreshed(struct drv_gpu_device *device, uint32_t display_id, uint64_t generation, uint64_t sequence);` と、登録の有無を driver に知らせる任意の op `refresh_arm(private, display_id, on)`（vblank の割り込みを登録がある間だけ有効にするため）。REFRESH は §10 の後の Phase。
- 呼ぶ文脈: driver が今 `poll_notify()` を呼ぶ所と同じ（spinlock を持たない、IRQ の外）。中は registry の lock と fence の spinlock・waitq の wake だけ。
- 契約: topology の番号は device（node）ごとに一つ（open ごとに違わない）。8 つの `events` の実装で確かめる（Phase p002）。
- 変える driver: i915（`hpd.c` の 2 か所、bootfb・capture は番号が動かないなら呼ばない）、venus、amdrdna、nvrtx、ghci、videocore の display。

### 3.3 採らない物

- b05187933a（libvulkan が display node の POLLPRI と ACK で block する形）は採らない（ws.md の決定 2: 問われた時に番号を問い合わせる形をやめる）。
- WS352 の branch の kernel の poll の直し（object ごとの起こし、9a5c910d65・67df3d26a0・508c992109・7b301652fc・839faa674c）はこの設計と独立で、方向（原則 E）が同じなので採る（§7）。

## 4. libvulkan

### 4.1 hotplug の fence

- `vkRegisterDeviceEventEXT`: renderer の display node（`GPU_CAP_DISPLAY_EVENTS` のある物）ごとに `GPU_DISPLAY_EVENT_FENCE`（HOTPLUG）で fd を得て、`struct vulkan_sync` に fd の配列（node の数、allocator で確保）を持たせる。`event_cursor`・`event_based` と `vulkan_wsi_display_node_topology` の hotplug の用は消す。事象を出す node が一つも無い fence は signal しない（今と同じ: hotplug が無い）。
- 状態（`vkGetFenceStatus`、`vulkan_sync_status_locked`）: fd のどれかが POLLIN（poll の timeout 0、kernel への問い合わせではなく payload の写しを読むだけ）なら signal。一度見たら `software_signaled`。
- 待ち（`sync_fences_wait`）: 事象の fence は fd を poll の集合に入れる（external fence と同じ扱い）。「missing」にしない。all の待ちで最初の未完が事象の fence なら、その fd で block する枝を足す。
- `vkResetFences`: 未 signal の登録を CANCEL して fd を閉じ、`VULKAN_SYNC_EVENT_SPENT`（今と同じ意味）。`vkDestroyFence`: 同じく CANCEL と close。

### 4.2 未提出の fence の待ち

- 未提出の普通の fence を含む待ちの 1 ms の見回り（sync.c:1683〜1688）をやめ、device の提出の知らせで起きる: 待ち手は `vulkan_wake`（internal.h:505、今は error と image の遷移に使う pipe）を登録して poll の集合に入れ、`vkQueueSubmit` と `vkResetFences`・import は `vulkan_wake_notify` を呼ぶ。起きたら観測をやり直す。
- これで compositor の hotplug の thread の「hotplug の fence と停止の fence のどちらか」の待ち（§5.3）が 1 ms で回らない。

### 4.3 present の完了（`VK_KHR_present_id`・`VK_KHR_present_wait`）

- display の swapchain に足す: `VkPresentIdKHR` の id を present の job に持たせ、worker が kernel の present の成功の後に `chain->presented_id` を上げて `swapchain_notify_locked`。`vkWaitForPresentKHR(device, swapchain, id, timeout)` は `swapchain_condition`（と wake の token）で block し、`presented_id ≥ id` で VK_SUCCESS、swapchain が out of date・lost・retire なら その error で返る（無期限の待ちが必ず終わる条件）。
- 宣言（`include/libc/vulkan/vulkan_core.h` に Khronos の header の該当の型・struct・PFN・prototype）、`api-commands.tsv`・`dispatch-table.inc`・`commands-generated.inc`（`tools/maintain-dispatch.noct` の手順。共有の `build/NoctLang` を build しない。無ければ手で同じ形に書き、Q1 に言う）、device の extension の一覧と feature。
- Wayland の WSI（client 側）には足さない（要る app が出たら別に）。

### 4.4 refresh の fence（後の Phase）

- `vkRegisterDisplayEventEXT`: その display の node に `GPU_DISPLAY_EVENT_FENCE`（REFRESH、display_id・generation）。kernel は driver の `drv_gpu_display_refreshed` で signal（§3.2）。
- これが入れば `vulkan_display_event_poll`・`event_refresh_poll`・`VULKAN_SYNC_EVENT_HOTPLUG/REFRESH` の観測と `VULKAN_SYNC_POLL_NS` は消える（`vulkan_sync_pause` の他の利用者は Phase で数える）。

## 5. compositor

### 5.1 thread の一覧（WS309 §2 を土台に）

| # | thread | 待つ物（一つ） | 起きる時 | 結果の知らせ | 変わる所 |
| --- | --- | --- | --- | --- | --- |
| T0 | main | poll（timeout -1）: listener・client の socket・wake pipe・backend の事象の fd・client の acquire fence | client の要求・wake・backend の事象 | — | 固定 10 ms と毎 pass の tick を消す（§5.2） |
| T1 | render | 自分の cond（依頼）。slot が足りない時だけ最古の slot の fence、acquire は空きまで | main の依頼 | 今の message（READY・COMPLETED は REAPED に意味を変える） | §5.6 |
| T2 | cursor | box の cond（移動の hold の間だけ期限付き）。present の後は次の image の acquire、copy の後はその fence | box の変化 | 失敗と pin の解放で wake | §5.7 |
| T3 | display | 依頼の列（lane） | 依頼 | 今のまま | hotplug の列挙の依頼の出し手が hotplug の thread の知らせになる |
| T4 | present | 依頼の列: game mode の present（同期の ioctl）と、window mode の frame の表示の完了（`vkWaitForPresentKHR`） | 依頼 | SHOWN（frame の番号・結果）、game mode の release | §5.9。合意した形の「present（present の完了）」 |
| T5 | input | poll（evdev・wake、tap の期限の間だけ timeout） | 入力 | 今のまま | WS352 の branch で 10 ms を消した形 |
| T6〜T9 | io-device・io-file・font・blur | 依頼の列 | 依頼 | 今のまま | — |
| T10〜T14 | daemon ×5 | poll（socket・link の wake、再接続・scan の期限の間だけ timeout） | daemon の知らせ | 今のまま | 250 ms を消した形（branch）、network の答えの fd（§5.11） |
| T15 | **hotplug**（新） | `vkWaitForFences`（hotplug の fence と停止の fence のどちらか、無期限） | display の抜き差し | HOTPLUG・FIRST・UNAVAILABLE | §5.3 |
| T16 | **timer**（新、合意した形の「締め切り」） | 自分の cond（最も早い期限まで、無ければ無期限） | 期限 | main の timer の発火 | §5.5 |
| P1 | hand ×N | 今のまま | | | |

- 合成の完了だけを知らせる thread は置かない（Q1 の指示）。合成の slot の空きは render が自分で fence を待って知る。frame の callback・animation・client の buffer の release は表示の完了（T4）から。
- thread の数: 常駐は WS309 の 14 に hotplug と timer の 2 を足して 16（T2 は output の間）。

### 5.2 main loop

- poll の timeout は常に -1。毎 pass に全部の tick を呼ばない。pass は「wake を空にする → 出口を全部取る（今の順、§3.3 of WS309）→ 発火した timer の handler → client の要求 → 描くかの判断 → flush」。
- 毎 pass に呼んでいた物の行き先（main 483bec3718 の main.c:950〜975 と display.c:115〜200）:
  - 入力の scan: zedBSD は /dev/system の INPUT の事象だけ（backend-host.c の `input_scan_time = 0` を「scan の依頼」に）。事象の後 2 s の 100 ms は §5.11。
  - ping・IME・titlebar・sleep・displays・glass・sysmon・machine・shot・data・clipboard の期限: timer（§5.5）。
  - `kwl_settings_tick`・`kwl_system_tick`: 中身は thread の結果の取り込みと daemon の写しの更新。wake の後の take（出口がある時だけ）に移す。
  - `kwl_output_tick`: §5.4。
  - `kwl_window_bounds_refresh`: 変化を起こす所（glass の look の変更）で直接呼ぶ。
- 子の終わり: SIGCHLD の handler が wake pipe に 1 byte（WS352 の branch の形）。main は wake の後に `waitpid(WNOHANG)`。
- 去った client の buffer の解放（main.c:1115 の 1 pass に数個）: 残りがあれば自分の wake pipe に 1 byte 書いて直ちに次の pass にする（入力を先に見る順は保つ）。timeout 0 と 10 ms を使わない。
- `perf.timeouts`（KWL PERF）が idle で 0 である事を試験の判定にする。

### 5.3 hotplug の thread（T15、最初の実装）

- 置き場所: `output-switch.c`（Makefile を変えない）。開始は compose の device ができた後、window mode の前でもよい（hotplug は device の事象）。`compose->register_device_event` が無い（拡張が無い）なら作らず、今と同じ「hotplug unavailable」。
- 流れ（取りこぼさない順、今の tick と同じ不変条件「新しい fence を先に、列挙はその後」）:
  1. fence F を登録 → main に FIRST（main は今の OUTPUT_LOOK_FIRST）。
  2. `vkWaitForFences(device, 2, {F, stop}, VK_FALSE, UINT64_MAX)`。
  3. F が signal: 新しい fence G を登録してから F を壊し、main に HOTPLUG。G で 2 へ。
  4. stop が signal: F を壊して終わる。登録の失敗: main に UNAVAILABLE（log）して終わる。
- 停止の fence: 起動の時に作る普通の fence。終了（event loop の後、原則の例外）で main が queue の lock の下で `vkQueueSubmit(queue, 0, NULL, stop)` して join。libvulkan の未提出の fence の待ち（§4.2）が要る。
- 知らせ: lane と同じ出口（lock・FIFO・`main_woken`）と共通の wake pipe。thread は compositor の状態に触れない（Vulkan の device の呼び出しと fence だけ）。

### 5.4 出力の追従を事象で（`kwl_output_tick` を消す）

- `kwl_output_follow(server)` を作り、今の tick の後半（出力を失った時の移動 `output_recover`、head の作り直し `heads_stale`・`kwl_heads_lost`）だけを持たせる。fence の look と 250 ms は消す。
- 呼ぶ所（事象）: hotplug の thread の HOTPLUG（`limited`・`move_failed` を 0 にして OUTPUT_LOOK_SIGNAL の列挙）と FIRST、`output_lost`・`heads_stale`・head の `lost` を立てる所（compose.c:552・4665・4892・4906、heads.c:580・695・766、output-switch.c の各所）、display の thread の完了（`kwl_display_take`）と列挙の完了（`output_listed`）。
- display が busy か列挙の途中で来た事象は `follow_pending` に覚え、busy の終わり・列挙の終わりで `kwl_output_follow` を呼び直す（周期で覗かない）。
- `kwl_displays_pending_tick`（displays の保留の答えの 3 s、displays-shell.c:254）は timer（§5.5）。p004 の時点で timer が無ければ、今の pass ごとの期限のまま `kwl_displays_tick` に移して残す（p006 で timer へ）。

### 5.5 timer（T16）

- 形: main だけが持つ `struct kwl_timer`（期限・handler・armed）と min-heap（main だけが書く）。API は main からだけ: `kwl_timer_arm(server, timer, due_ms)`・`kwl_timer_disarm(server, timer)`。heap の先頭が変わった時だけ、timer thread に「次の期限」を lock と cond で渡す。
- timer thread: 期限があれば `pthread_cond_timedwait`（CLOCK_MONOTONIC）、無ければ `pthread_cond_wait`。期限に達したら出口に「発火」を立てて wake。main は wake の後に heap から期限の過ぎた timer を全部取り、handler を呼ぶ。handler は要るなら次を arm する（予定がある間だけ）。
- 置き場所: `threads.c`（thread の台帳の file）と `threads.h`。
- main 以外の thread の締め切り（input の tap、cursor の hold、daemon link の再接続・scan、render の打ち切り）は、その thread が自分の一つの待ちに期限を付ける（未決 1 の推し、§8）。
- 各機能の変え方: 毎 pass の「`now >= x_until` を比べる」を、「x を始める所で arm、終わる所で disarm、handler で処理」にする。WS352 の branch の `kwl_due` の呼び出し 50 か所余りがそのまま一覧になる（branch を merge した後なら `kwl_due` を `kwl_timer_arm` に置き換える作業、merge しないなら main の比べの所から）。

### 5.6 render（T1）: 合成の buffer は数で回す

- slot（`KWL_FRAME_SLOTS`）は合成の資源だけを持つ: command buffer・fence・copy の有無・層の絵の generation・scene の書き込み・frame の番号と時刻。client の buffer の hold（`held[]`）と frame の callback（`callbacks`）は slot から外す（§5.8・§5.9）。
- 待ち（一度に一つ）:
  1. 依頼が無い: `changed` の cond を無期限（WS352 の branch で 100 ms をやめた形）。
  2. frame の依頼で空きの slot が無い: 最古の slot の fence を `vkWaitForFences(UINT64_MAX)`。fence を reset するのも render だけ（他の thread は slot の fence を待たない）。
  3. output の image: `vkAcquireNextImageKHR(UINT64_MAX)`（FIFO では次の image は最長で一つの refresh の後に空く）。先回りの acquire（`ahead`）は前の present の後に同じく block。
  4. head の image: timeout 0（空きが無い head はこの frame を飛ばし missed、§5.9 の SHOWN でもう一度 want）。
- 完了の観測: render は起きた時（依頼・park）に飛んでいる slot の fence を `vkGetFenceStatus` で見て、終わった物を REAPED（serial、層の絵の完了、scene の有効）で main に知らせる。2. の待ちの後も同じ。idle では何もしない（何も要らない: 層の絵は同じ queue の順で後の frame から読めるので、完了は古い絵の解放の時期だけに効く）。
- park・drain・stop: 飛んでいる slot の fence と task の fence を順に無期限で待ってから答える（今の `KWL_RENDER_POLL_NS` の覗きをやめる）。
- 消える物: `KWL_RENDER_POLL_NS`（1 ms）、`KWL_RENDER_ACQUIRE_NS`（50 ms）、`RENDER_WAIT_MS`（100 ms、branch で済）。
- 確かめる点（Phase で）: display の電源を切っている間に acquire が返らない事が無いか（sleep の手順は park の後に電源を切るので、block 中の park は最長で一つの refresh を待つ）。

### 5.7 cursor（T2）

- box の cond を待つ（変化が無ければ無期限、移動の hold の間だけ hold の期限）。
- present の後に次の image を `vkAcquireNextImageKHR(UINT64_MAX)` で先に取っておく（最長で一つの refresh）。変化が来たら持っている image に描いて present。timeout 0 の acquire と 1 ms の再試行（`CURSOR_RETRY_US`）を消す。
- copy を submit したら、その fence をすぐ無期限で待ち、pin を外す（copy は小さい）。pin を外したら main に wake（今の cursor-plane.c:475 の 10 ms の見張りを消す）。`cursor_unpin_done` の 5 ms の見回り（`CURSOR_PIN_POLL_US`）を消す。
- park の答え・停止の 100 ms の打ち切り（T3 が待つ側）は残す（§5.10）。

### 5.8 client の buffer の寿命（合成の slot から外す）

- 持ち主は client。返す条件は (a) 新しい buffer に置き換わった（surface の current でない、pending・queued・scanout も持たない）と (b) それを読んだ GPU の仕事が終わった。
- **shm**: commit の時に CPU で写し、写したらすぐ release（今と同じ）。写しの条件を「frame が一つも飛んでいない」から「その host image の staging を読む copy を持つ slot が飛んでいない」に狭める（slot は copy した host image の generation を覚える）。待つ時は REAPED・SHOWN で続きを行う（覗かない）。
- **GPU の buffer**: buffer ごとに「最後にそれを読んだ submit の serial」（`read_serial`）を持つ。compose が sample した時に上げる。hold の数（`holds`）は protocol と current の分だけ。置き換わった時、`read_serial ≤ done_serial` ならすぐ release、でなければ main の「返し待ち」の list に入れる。`done_serial` は (1) T4 の SHOWN（frame n の表示 ⇒ n までの job が終わった）と (2) render の REAPED で上がり、上がった時に list を返す。
- 前提（Phase で確かめる）: 同じ queue の job は出した順に終わる（libvulkan の 1 本の queue と kernel の job の列）。present が失敗した frame（OUT_OF_DATE など）は SHOWN が serial を上げず、REAPED（出力の作り直しの drain）を待つ。
- 去った client・壊された buffer: `dead` かつ返せる時に解放（今の `object_free` の条件に serial を足す）。
- これで `slot->held`・`compose_hold` の hold と `kwl_buffer_put` の slot からの呼び出しは消える。
- client の acquire fence: 今のまま main の poll（未決 2 の推し）。

### 5.9 frame の callback・animation・pacing

- frame の callback: main が frame ごとの記録（番号、その frame に載せた callback の列、表示した head）を持ち、T4 の SHOWN（その frame の present の完了、output と載せた head 全部）で送る。present が失敗しても callback は送る（client を止めない、今と同じ）。
- T4 の window mode の仕事: render が present の後に「frame n、output の swapchain と present id、head の swapchain と id」を T4 に積む。T4 は順に `vkWaitForPresentKHR(..., UINT64_MAX)` を待ち、SHOWN(n, 結果) を出口へ。game mode の present と同じ FIFO（どちらも最長で一つの refresh）。
- animation: 動いている間だけ「描く → SHOWN で一歩進めて次を描く」。描かずに時刻だけで進む物（sleep の保留、spring の判断など）は timer。
- frame の pacing（display.c:255 `frame_done + frame_wait`、client の commit を少し待つ）: timer。
- 時計: 次の分の timer は、時計が見える時（画面が点いていて時計の面がある）だけ arm（消灯の間は arm しない）。
- p009（present wait）と p010 より前の段階では、callback と animation は render の COMPLETED（p011 の後は REAPED）から（今の GPU の完了の時と同じ意味）。順は §10。

### 5.10 打ち切りとして残す待ち

- 起動・終了（READY の前、event loop の後）、display の切り替えの間の park の答え（T3、100 ms ずつ最大 10 s）、cursor の park・停止の答え、sessiond の起動の答え（20 s）、input の detach の受領（100 ms、故障の時だけ）、fatal client の flush（2 s、timer）。いずれも「予定がある間だけ」で idle には無い。

### 5.11 thread の結果の flag を wake に（名残り）

- 壁紙の loader（glass.c:877）、shot の送り（shot.c:276）、cursor の copy の手放し（cursor-plane.c:475）、machine の読み、media の helper（media-library.c:214、pipe を poll と SIGCHLD）: 終わりに wake pipe。system.c の 2 つ（562・2613）: lane の終わりと sessiond の答えで続ける。
- 入力の scan の事象の後 2 s の 100 ms（`KWL_INPUT_SETTLE_SCAN_MS`）: kernel が /dev/system の INPUT の事象を evdev の node が見える前に出しているなら kernel の順を直す（`src/drivers/generic/input.c` の登録）。直れば事象の時の 1 回の scan で足りる。
- daemon link の network の答えの 10 ms（network-zedbsd.c:638 `NETWORK_ANSWER_LOOK_MS`）: 答えの fd を link の poll に入れる（理由の kernel の振る舞いは BUG-149 で直っている）。

## 6. WSI

- Wayland の WSI（libvulkan の client 側）: release の意味（読み終わった後の `wl_buffer.release`）は変えないので、§5.8 の推しの段階では変更が要らない。明示の release fence（未決 3）を取る時は protocol の rev 4 と WSI の待ちが要る（§8）。
- frame の進みの待ちの 50 ms の slice（wsi-wayland.c:24）: progress pipe と接続の fd の poll で起きるので、slice は保険。無期限にする（Phase p015）。1 s の pace（`WAYLAND_PACE_NS`）は、隠れた surface に compositor が callback を送らない間に app の FIFO を止めない上限で、残す（締め切り）。
- libkeiland: 変更なし（release は WSI を通る）。

## 7. WS352 の branch の扱い（推し: b05187933a を除いて merge）

- 採る（WS352 p002 として merge）: kernel の poll を object ごとに起こす形と名指しの通知（poll.c・pipe・socket・evdev・/dev/system・audio・bt-hci・GPU の POLLPRI）、compositor の固定 10 ms の撤去（`kwl_due`）、input の touch pad の 10 ms、render の 100 ms、daemon link の 250 ms と bluetooth の空回り、machine・SIGCHLD・signal の wake、sessiond の fd を poll に。
- 理由:
  1. 方向がこの設計と同じ（原則 1・2・4、poll の object ごとの起こし）。kernel の部分はこの設計と独立で、そのまま最終の形。
  2. `kwl_due` は「main の poll の timeout に畳む」形で最終の形ではないが、各期限の場所と予定の有無を既に整理している。timer への移し（§5.5）は `kwl_due(server, at)` を `kwl_timer_arm` に置き換え、毎 pass の tick を handler に割る作業になり、main の 10 ms の比べから始めるより小さく、確かめやすい。
  3. idle の改善（10 ms・250 ms・100 ms の撤去）がこの WS の長い移行を待たずに入る。
  4. main（483bec3718）と `git merge-tree` で競合なし。
- 採らない: b05187933a（ユーザーの決定 2 と違う。未使用の関数と sync.c の待ちを足すだけなので、branch で revert してから merge するか、merge の後に p003 で消す。前者を推す）。
- merge しない場合: p005〜p008 の作業は main の 10 ms の pass から始まる（同じ到達点、量が増える）。WS353 の Phase の依存は「WS352 p002 の merge の後」と書いた。

## 8. 未決（ws.md の 3 つ）の案と推し

### 未決 1: timer を 1 本の thread で持つか、各 thread が自分で timed wait するか

| 案 | 形 | 良い所 | 悪い所 |
| --- | --- | --- | --- |
| A | 全部の締め切りを 1 本の timer thread（main 以外の thread の分も、その thread の cond に知らせる） | 時刻の持ち主が一つ | input・daemon link・cursor は自分の fd・cond を待っているので、timer からの知らせのために待ちを二つにする（pipe か cond を足す）。hop が増える |
| B | 各 thread が自分の一つの待ちに期限を付ける。main は poll の timeout ＝ 自分の heap の先頭 | thread が増えない | main の poll に期限を畳む形（合意した形が避けた物）が残る。main の「周期・期限を持たない」が成り立たない |
| **C（推し）** | **main の締め切りは timer thread（T16）、他の thread は自分の待ちに期限を付ける** | main は timeout -1 で、事象と timer の発火を同じ wake で受ける。他の thread の期限はその thread の仕事の一部（tap・hold・再接続）で、待ちは一つのまま | thread が 1 本増える（wake の hop は µs） |

### 未決 2: main が client の socket を自分で poll するか、それも thread に出すか

| 案 | 形 | 評価 |
| --- | --- | --- |
| **A（推し）** | **main が poll（timeout -1）: listener・client の socket・wake pipe・backend の事象の fd（seat・/dev/system・notify の bus、WS309 J6 の非 blocking の接続）・client の acquire fence** | protocol の持ち主が自分の入力を直に読む（一般の compositor の形）。fd を渡す（SCM_RIGHTS）要求も順のまま。idle は無期限の block で、thread に出しても電力は同じ。J6 の決定と合う |
| B | 合意した形の文字どおり: main は client の socket と wake pipe だけ。acquire fence と backend の fd は別の thread（「受け」の thread）が待ち、知らせる | main の待ちの集合が小さい。代わりに thread と hop が増え、acquire の fence の待ちと backend の事象の順が main の要求の順とずれる |
| C | client の socket も読む thread に出し、main は wake pipe だけ | 要求の写しと fd の受け渡しが増える。利点が無い |

- A は「main の poll の集合に client の要求と同じ種類の事象の fd を入れる」だけで、timeout と周期は持たない。合意した形の「main: client の socket と共通の wake pipe だけを待ち」との違いはここだけなので、ユーザーに確かめる点として上げる。

### 未決 3: client の buffer の GPU の同期の方式

| 案 | 中身 | 要る物 | 評価 |
| --- | --- | --- | --- |
| **明示（推し、目標）** | compositor は置き換わった buffer にすぐ `wl_buffer.release` と release fence（最後に読んだ job の fence の export）を送る。client（WSI）は fence を待ってから（または wait の semaphore として）描く。compositor は待たない | render の全部の backend の `GPU_CAP_FENCE`（今は venus・ghci-vk だけ。i915・amdrdna・powervr・mali・videocore に、gpu.c の共通の bind で job の完了に fence を結ぶ口を出す）、`kl_gpu_buffer_v1` rev 4（release の fence の event）、WSI の待ち | compositor の側の待ちと返し待ちの list が無くなる。一般の作法（Linux の linux-drm-syncobj の release point と同じ考え）。acquire 側と対称。前提の kernel の作業が大きい |
| 暗黙 | kernel が allocation ごとに読み手を追い、client の次の書きの submit を kernel が待たせる | kernel の全 backend に予約の仕組み | zedBSD の GPU の ABI は明示の依存の形（gpu-fence.h）で、方針と逆。取らない |
| **完了を見てから返す（推しの目標までの段）** | §5.8: buffer ごとの read の serial を、表示の完了（T4 の SHOWN）と render の REAPED で返す。専用の thread なし | present wait（§4.3） | 全 device で働く。返しが最長で一つの refresh 遅れる（FIFO の client は callback と同じ時期に返るので影響は小さい） |

- 推し: 目標は明示の同期。前提（全 render の backend の `GPU_CAP_FENCE`）が揃うまでは「完了を見てから返す」を表示の完了に載せる（合成の slot から外れ、専用の thread も要らない）。明示は Phase p013 で、前提の調べ（各 backend が fence を出せない理由）から始める。

## 9. HAL・Makefile・その他の承認

- **HAL**: 変えない（`include/hal/hal.h` に触れない）。kernel の変更は gpu の枠組み（gpu.c・gpu-fence.c・`include/drivers/gpu/`）と uapi と各 GPU driver。
- **Makefile・mk/**: 変えない。新しい code は既存の file に置く: hotplug の thread は `output-switch.c`、timer は `threads.c`、T4 の window mode の仕事は `scanout.c`（今の present の lane の持ち主）か `render-thread.c`、libvulkan は既存の file。新しい試験の program は `plan/ws353/tests/` の script が host の cc で作る。
- **toolchain**: 変えない。libvulkan の生成の `.inc` は `tools/maintain-dispatch.noct` を、既にある NoctLang で回せる時だけ回す（共有の `build/NoctLang` を build しない）。
- **header**: `include/libc/vulkan/vulkan_core.h` に present id・present wait の宣言（Khronos の header の該当部分、許諾は同じ header のまま）、`include/uapi/gpu-display.h` に新しい ioctl。いずれも HAL・toolchain ではない。
- **protocol**: 明示の同期（未決 3）を取る時だけ `kl_gpu_buffer_v1` rev 4（libwayland の生成の header を足す）。

## 10. 移行の順（Phase の表は ws.md）

1. **hotplug（ユーザーの決定 2、最初）**: p002 kernel → p003 libvulkan → p004 compositor の hotplug の thread と 250 ms の tick の撤去。
2. **main loop**（WS352 p002 の merge の後を推す）: p005 timer と poll(-1)・名残り → p006・p007 締め切りを timer へ → p008 animation・pacing・時計。
3. **表示の完了と client の buffer**: p009 libvulkan の present wait → p010 T4 の SHOWN で callback・animation・client の buffer の返し（slot から外す）。
4. **合成の buffer**: p011 render（slot の数で、足りない時だけ fence、acquire は block）→ p012 cursor。p011 は p010 の後（render が完了を覗くのをやめる前に、callback と release を表示の完了に移す。逆だと idle の FIFO の client が callback を待って止まる）。
5. p013 明示の同期（未決 3 の推しが決まったら）、p014 refresh の fence、p015 backend と WSI の小さな直し。
6. p016 試験（QEMU の回帰と 5330 の idle）、p017 規約の全文の見直し。

## 11. 試験の方針

- 各 Phase: build（x86_64・arm64・riscv64 の該当、warning 0）と変えた所の host 試験。kernel・libvulkan は QEMU（Venus、P1 の名前と port）で、hotplug は `plan/tools/guest/venus-head.sh`（D-Bus の display の head の抜き差し）。
- 判定は guest の file・SSH・QMP（console・serial の log を読まない）。止まり・例外は gdbstub・QMP。
- idle の判定: compositor の `KWL PERF`（`timeouts`）、WS352 の poll の診断（branch の `plan/ws352/tests/poll-diag.patch`・`qemu-poll.sh`、merge の後の場所）で thread ごとの timeout の起床が 0。5330 は WS352 の `5330-idle.sh` で package の電力・PC2（WS333 p029 と同等）。
- 応答: 入力（key repeat・pointer・touch pad の tap）、描画（client の commit から表示）、animation（slide・fade）、hotplug（head の抜き差し、出力の移動）、sleep・lock、game mode の出入り。

## 12. WS352 の 91 件の対応表

- 番号は [poll-report.md](../ws352/poll-report.md) の表の行（表番号-行）。区分: 名＝名残り、道＝道が無い、締＝締め切り。数え方は poll-report.md §5 と同じ（1 行に区分が 2 つあれば両方、animation の 16 か所は 1）。表 6 の締め切り 5 のうち 1 は printd の 60 s の idle の期限と読んだ（行から数えると 4、報告の数に合わせた）。
- 行き先: **消** ＝この設計で無くなる、**形** ＝待ち方が変わり残る、**残** ＝要る締め切りとして残す（予定がある間だけ）、**外** ＝WS353 の範囲の外（WS352 か別の WS。直し方の原則はこの設計の §1）。
- 「352」は WS352 p002（branch）の merge で済む物。

### 表 1 thread ごとの待ち（19）

| # | 場所 | 区分 | 行き先 | Phase |
| --- | --- | --- | --- | --- |
| 1-01 | main の event loop（main.c:1110） | 名 | 消: poll(-1)、締め切りは timer | 352・p005 |
| 1-02 | input の touch pad の 10 ms | 名 | 消 | 352 |
| 1-03 | input の tap の期限（`TAP_DRAG_MS`） | 締 | 残: input の thread の自分の待ちの期限（未決 1 C） | — |
| 1-04 | main が input の detach の受領を待つ 100 ms | 締 | 残: 故障の時の打ち切り（§5.10） | — |
| 1-05 | render の 100 ms の timedwait | 名 | 消 | 352・p011 |
| 1-06 | render の描画中の 1 ms・acquire 50 ms | 道 | 消: slot が足りない時だけ最古の fence、acquire は block（§5.6） | p011 |
| 1-07 | main・display が render の答えを待つ（切り替えの間） | 締 | 残: 切り替えの間の打ち切り | — |
| 1-08 | render の再試行 `usleep(1000)`（memory の失敗） | 締 | 残: 失敗の時だけ | — |
| 1-09 | cursor の移動の hold | 締 | 残: cursor の thread の自分の待ちの期限（動いている間） | p012 |
| 1-10 | cursor の pin の 5 ms | 道 | 消: copy の fence をすぐ待つ（§5.7） | p012 |
| 1-11 | cursor の再試行の 1 ms | 道 | 消: 次の image を先に block で acquire | p012 |
| 1-12 | cursor の copy・park の 100 ms | 締 | 形: copy の fence は無期限（原則 7）、park の答えは打ち切りとして残す | p012 |
| 1-13 | scanout・compose の fence と acquire の打ち切り | 締 | 残: 切り替え・片付けの間だけ | — |
| 1-14 | daemon link の 250 ms | 名 | 消 | 352 |
| 1-15 | bluetooth の止められた scan の空回り | 名 | 消 | 352 |
| 1-16 | network の答えの 10 ms | 名 | 消: 答えの fd を link の poll に（§5.11） | p015 |
| 1-17 | daemon link の再接続・scan の期限 | 締 | 残: link の thread の自分の待ちの期限 | — |
| 1-18 | sessiond の起動の答え 20 s | 締 | 残: 起動の時だけ | — |
| 1-19 | machine の結果の flag | 名 | 消: 終わりに wake | 352 |

### 表 2 main loop の期限（44）

| # | 場所 | 区分 | 行き先 | Phase |
| --- | --- | --- | --- | --- |
| 2-01 | output-switch.c の hotplug 250 ms（と出力を失った時の 10 ms） | 道 | 消: kernel が signal する fence と hotplug の thread（§3・§4.1・§5.3・§5.4） | p002〜p004 |
| 2-02 | 入力の scan 2 s（zedBSD） | 名 | 消: /dev/system の INPUT の事象だけ（Linux・FreeBSD は数えない行で残す） | p005 |
| 2-03 | 事象の後 2 s の 100 ms の scan | 道 | 消: kernel の事象を node の後に出す（§5.11、順を確かめて直す） | p005 |
| 2-04 | printers-shell の 1 s | 道 | 外: printd が設定を持ち変化を push する（daemon）。それまで printers の面がある間の timer として残す | p007（timer へ）・外 |
| 2-05 | network.c の詳細の 1 s | 道 | 外: networkd が詳細の変化を push する。それまで view がある間の timer | p006（timer へ）・外 |
| 2-06 | sysmon の 250 ms | 締 | 残: 購読がある間の timer | p006 |
| 2-07 | cursor-plane.c の 10 ms | 名 | 消: cursor の thread が手放しで wake | p005 |
| 2-08 | glass の loader の 10 ms | 名 | 消: 終わりに wake | p005 |
| 2-09 | shot の送りの 10 ms | 名 | 消: 終わりに wake | p005 |
| 2-10 | system.c:562 の再試行 | 名 | 消: lane の終わりで続ける | p005 |
| 2-11 | system.c:2613 の EBUSY の問い | 名 | 消: sessiond の答えで問い直す | p005 |
| 2-12 | main.c:1115 の去った client の buffer の解放 | 名 | 形: 返し待ちの list と自分への wake（§5.2・§5.8） | p010 |
| 2-13 | animation の 16 か所 | 名 | 形: 表示の完了で一歩（§5.9）。時刻だけの物は timer | p008・p010 |
| 2-14 | media-library の 10 ms | 名 | 消: pipe を poll、子の終わりは SIGCHLD の wake | p005 |
| 2-15 | fatal client の flush 2 s | 締 | 残: timer | p006 |
| 2-16 | `--timeout` | 締 | 残: timer | p006 |
| 2-17 | 時計の分 | 締 | 形: 時計が見える間だけ timer（消灯の間は無し） | p008 |
| 2-18 | band の長押し 10 ms | 締 | 形: 閾値の時刻に 1 回の timer | p007 |
| 2-19 | handoff の期限 | 締 | 残: timer | p006 |
| 2-20 | sleep.c の 4 つ | 締 | 残: timer | p006 |
| 2-21 | idle の lock | 締 | 残: timer | p006 |
| 2-22 | titlebar の key repeat | 締 | 残: timer | p007 |
| 2-23 | apps bar の hover・leave | 締 | 残: timer | p007 |
| 2-24 | OSK の手書きの認識 600 ms | 締 | 残: timer | p007 |
| 2-25 | greeter の 3 つ | 締 | 残: timer | p007 |
| 2-26 | toplevel の ping 5 s | 締 | 残: timer | p006 |
| 2-27 | IME の再起動・答え | 締 | 残: timer | p006 |
| 2-28 | clipboard の 2 s | 締 | 残: timer | p006 |
| 2-29 | drag の release | 締 | 残: timer | p006 |
| 2-30 | desktop の再起動の間隔 | 締 | 残: timer | p006 |
| 2-31 | home・corner の 30 s・5 s | 締 | 残: timer | p007 |
| 2-32 | network.c の 30 s・切り替え | 締 | 残: timer | p006 |
| 2-33 | notify-log を閉じる | 締 | 残: timer | p007 |
| 2-34 | displays の保留の答え 3 s | 締 | 残: timer（p004 では今の期限のまま displays へ移す） | p004・p006 |
| 2-35 | rotate の hold | 締 | 残: timer | p007 |
| 2-36 | scanout の pointer の静止・lend の拒み | 締 | 残: timer | p007 |
| 2-37 | layer cache の静けさ | 締 | 残: timer | p007 |
| 2-38 | frame の pacing（display.c:255） | 締 | 残: timer（§5.9） | p008 |
| 2-39 | glass の blur の hold | 締 | 残: timer | p007 |
| 2-40 | volume の送りの間隔 | 締 | 残: timer | p007 |
| 2-41 | machine の読みの打ち切り 10 s | 締 | 残: timer | p006 |
| 2-42 | shot の答えの打ち切り | 締 | 残: timer | p006 |
| 2-43 | phone の held・sync | 締 | 残: timer | p006 |
| 2-44 | system の scan の表示 | 締 | 残: timer | p007 |

### 表 4 libvulkan（1）

| # | 場所 | 区分 | 行き先 | Phase |
| --- | --- | --- | --- | --- |
| 4-01 | sync.c の `VULKAN_SYNC_POLL_NS` 1 ms | 道 | 消: hotplug の fence は kernel の fd（p003）、未提出の fence は提出の知らせ（p003）、refresh の fence は p014 | p003・p014 |

### 表 5 client（9）

| # | 場所 | 区分 | 行き先 |
| --- | --- | --- | --- |
| 5-01 | files の folder の 2 s | 道 | 外: kernel に file の変化の知らせが無い（別の WS） |
| 5-02 | files の子の 20 ms | 名 | 外: WS352（子の pipe を待ちの fd に） |
| 5-03 | files の検索・task・scroll など | 締 | 残（操作の間） |
| 5-04 | settings の address の 1 s | 道 | 外: networkd の state に address を入れる（WS352 か別の WS） |
| 5-05 | settings の network の activity | 締 | 残（表示の間） |
| 5-06 | settings の About の時計・慣性 | 締 | 残 |
| 5-07 | keiland-ime の key repeat・保存 | 締 | 残（idle は -1） |
| 5-08 | libkeiland の clipboard・primary の 100 ms 刻み | 締 | 残（貼り付けの間） |
| 5-09 | libkeiland の起動の読み 500 ms | 締 | 残 |

### 表 6 daemon（13）

| # | 場所 | 区分 | 行き先 |
| --- | --- | --- | --- |
| 6-01 | sessiond（session）の 1 s | 名 | 外: WS352（SIGCHLD を self-pipe に） |
| 6-02 | sessiond（greeter）の 1 s | 名 | 外: 同じ |
| 6-03 | bluetoothd の 1 s の上限 | 名 | 外: WS352 |
| 6-04 | bluetoothd の seat の 5 s | 道 | 外: seat の持ち主の変化の事象（WS352 か別の WS） |
| 6-05 | bluetoothd の controller の無い間 2 s | 締 | 残 |
| 6-06 | audiod の mmap の 1 s | 名 | 外: WS352 |
| 6-07 | audiod の write の半 period | 道 | 外: dsp の残量の知らせ（別の WS） |
| 6-08 | audiod の device の無い間の period | 締 | 残（要検討は WS352） |
| 6-09 | networkd の期限 | 締 | 残 |
| 6-10 | networkd の受けの途中の 20 ms | 名 | 外: WS352 |
| 6-11 | volumed の settle | 締 | 残 |
| 6-12 | printd の 1 s | 名 | 外: WS352 |
| 6-13 | printd の 60 s の idle の期限 | 締 | 残 |

### 表 7 kernel の周期の sleeper（5）

| # | 場所 | 区分 | 行き先 |
| --- | --- | --- | --- |
| 7-01 | xHCI の port worker の 100 ms ×2 | 名 | 外: kernel（WS352 か別の WS）。waitq の sequence で競いを閉じ、保留の間だけ期限 |
| 7-02 | buffer の flusher の 1 s | 道 | 外: dirty の知らせ（別の WS） |
| 7-03 | cpuperf の governor の 100 ms | 締 | 残（要る間だけ） |
| 7-04 | thermal の 1 s | 締 | 残（割り込みの無い sensor の sample） |
| 7-05 | pageout の 100 ms | 名 | 外: kernel（WS352 か別の WS） |

### 件数

| 行き先 | 表 1 | 表 2 | 表 4 | 表 5 | 表 6 | 表 7 | 計 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 消（352 で済む物を含む） | 10 | 9 | 1 | 0 | 0 | 0 | 20 |
| 形 | 1 | 4 | 0 | 0 | 0 | 0 | 5 |
| 残 | 8 | 29 | 0 | 6 | 5 | 2 | 50 |
| 外 | 0 | 2 | 0 | 3 | 8 | 3 | 16 |
| 計 | 19 | 44 | 1 | 9 | 13 | 5 | 91 |

- 表 2 の 2-04・2-05 は「外」に数えた（compositor の側は timer に移すが、周期の読み直しが消えるのは daemon の push の後）。
- 「残」の 50 は全部、予定がある間だけの締め切り（idle で arm されている物は、消灯・lock の予定と、購読がある間の sysmon など）。

### poll-report に無く、調べで見つけた物

| 場所 | 中身 | 行き先 |
| --- | --- | --- |
| render の先回りの acquire（`ahead`） | idle で acquire を 50 ms・1 ms で回し得る | 消（p011、block の acquire） |
| wsi-wayland.c:24 の 50 ms の slice | client の FIFO の待ち（present の間だけ） | 消（p015） |
| wsi-wayland.c の 1 s の pace | 隠れた surface の app を止めない上限 | 残 |
| head の image の取れなかった frame | 今は次の pass・tick で描き直し | 形（p010・p011、その head の SHOWN で want） |

## 13. 未確認・調べの残り

- topology の番号が node ごとに一つか（open ごとに違う driver が無いか）: p002 で 8 つの `events` を読む。
- 同じ queue の job が出した順に終わるか（§5.8 の前提）: p010 で libvulkan の queue と kernel の job の列を読み、試験で確かめる。
- display の電源を切っている間の FIFO の acquire・present の完了: p011 で sleep の手順の順を確かめる。
- i915 などが `GPU_CAP_FENCE` を出さない理由: p013 の最初の調べ。
- `vkQueueSubmit` の submitCount 0 と fence の扱い（停止の fence）: queue.c の「empty submit」の経路を p003・p004 で確かめる。
