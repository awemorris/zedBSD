<!-- awesome-plan project=zedbsd record=ws353-p003 -->

# ws353-p003: libvulkan の hotplug の fence と、未提出の fence の待ち

Status: cleared（2026-11-14 02:05 P1、q2059-i01。build 3 機種 warning 0、QEMU の 13 step と latency が PASS（下の「結果」）。Q1 の確認待ち）
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

## 結果（2026-11-14 P1、q2059-i01、branch `agent/p1-ws353c`、基点 main c1a4e756af）

### commit（基点 main c1a4e756af、途中で main e99e940bc7 を merge）

- f93eb33fab: libvulkan（`userland/desktop/libvulkan/` の 9 file: sync.c・sync-internal.h・objects.c・internal.h・queue.c・external-fence.c・wsi-display-control.c・wsi-display-nodes.c・wsi-internal.h）。
- 139558b9ca・fed87db7f6・a3bada3c90・cf5fb2bf19・89d148b2ed・13a1f1db32: QEMU の試験（`plan/ws353/tests/hotplug-wait-test.c`・`qemu-hotplug-wait.sh`・`config-amd64-hotplug.mk`）。
- 350ab178f3: host の Venus の renderer の build（`plan/ws353/tests/build-venus-renderer.sh`）。
- 23faeda211: main e99e940bc7（BUG-431 の venus の直しと serial.py）の merge（親 cf5fb2bf19・e99e940bc7、競合なし）。
- c44b5ba42c: libvulkan の any-wait の空回りの直し（sync.c、下の「見つけた事」）と試験の poll の数の上限。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- hotplug の fence（`wsi-display-control.c`・`wsi-display-nodes.c`）: `vkRegisterDeviceEventEXT` は renderer の display node ごとに `GPU_DISPLAY_FENCE`（HOTPLUG、CLOEXEC）で kernel の fence の fd を得て、`struct vulkan_sync` の `event_fences`（node の識別子と fd、fence の allocator）に持つ。topology の事象を出さない node（EOPNOTSUPP）と外れた node（ENODEV）は飛ばす。ENOSPC・ENOMEM・EMFILE・ENFILE は `VK_ERROR_OUT_OF_HOST_MEMORY`、他は `VK_ERROR_UNKNOWN`（登録した分は CANCEL と close で戻す）。`event_hotplug_poll` は fd を timeout 0 で poll するだけ（POLLIN・POLLERR のどちらも事象）。`vulkan_wsi_display_node_topology` は他に使う所が無いので消した（`vulkan_wsi_display_node_count`・`_hotplug`・`_hotplug_cancel` を足した）。
- reset と destroy: `vulkan_display_event_reset_locked` は事象が起きた fence（観測済み、または kernel が signal して未観測）を SPENT にして未 signal の登録を CANCEL（ENOENT は無視）、未 signal の fence は今のまま待ち続ける。fd は destroy（`sync_release_storage` → `vulkan_display_event_finish`）で CANCEL と close。
- 未提出の fence の待ち（`sync.c`・`objects.c`・`internal.h`）: `struct vulkan_wake` に待つ fence の集合（呼び手の配列を借りる）。`vulkan_wake_create_fences`・`vulkan_wake_notify_fence(device, fence)`（その fence を名指す同じ device の token だけ起こす）。`vkQueueSubmit`（`queue_enqueue` で受け入れた時、呼び手の fence）・`vkResetFences`・`vkImportFenceFdKHR`・acquire の signal（`vulkan_wsi_acquire_signal`）が呼ぶ。device・context の error は今のまま全部の token、image の遷移（`vulkan_wake_notify(NULL, NULL)`）は fence の待ちの token を起こさない。
- `sync_fences_wait`: 1 回目の観測で token の要る fence（未提出の普通の fence・SPENT の fence・hotplug の fence）を見つけたら token を登録してから観測をやり直し（登録の後の観測の後にだけ眠る、L3）、各回の観測の前に drain。any の待ちは [context, 共有の fence の fd…（error は loss）, token, hotplug の fd…（error は観測のやり直しだけ）] を poll。context は marker か共有の fence がある時だけ POLLIN、無い時は events=0（loss だけ。他の thread の job の完了で起きない）。all の待ちの最初の未完が token の fence なら [context(0), token, その hotplug の fd] を poll。1 ms の見回りは REFRESH の fence（未提出）と record の無い marker の時だけ残る。
- 事象の fence の状態は「queue が持っていない」を `native_unsubmitted` ではなく `notification == 0` で見る（普通の fence の `native_unsubmitted` は作った後ずっと真なので、SPENT の fence を queue に出すと永久に NOT_READY だった）。
- 眠る前の reap（c44b5ba42c）: completion を待つ any-wait は眠る前に pin の無い終わった record を必ず reap する（今までは ledger が 8 以上の時だけ）。

### 先に確かめた事

- `vkQueueSubmit(queue, 0, NULL, fence)` で fence が signal する経路は queue.c の `vulkan_queue_idle` と同じ（試験の submit・narrow・race で使った）。
- b05187933a（POLLPRI の待ち）は main に入っていない（`git merge-base --is-ancestor` で確かめた）ので消す物は無い。
- display node の open は instance ごとに一つ（`wsi-display-nodes.c` の inventory は instance に一つで、その instance の全 physical device と VkDevice が共有、instance の破棄まで開いたまま）。kernel の登録の上限 16 は「instance × node」ごと。compositor は instance が一つで、hotplug の thread は F と G の 2 つまで。

### 設計からの変更（理由）

1. reset: 設計 §4.1 は「reset で未 signal の登録を CANCEL して fd を閉じ SPENT」。実装は、事象が起きた fence（観測済み、または kernel が signal して未観測）だけを SPENT にして CANCEL し、未 signal の fence の reset は何もしない（待ち続ける。Vulkan の reset は signal を消す操作で、未 signal の fence には no-op、今までの code の振る舞いとも同じ）。fd は destroy まで閉じない: vkResetFences は fence の host の待ちと並んでよく、別の thread の poll の fd を閉じると番号の使い回しで別の file を待ちうるため。
2. image の遷移の知らせ（`vulkan_wake_notify(NULL, NULL)`、swapchain の image の遷移）は fence の待ちの token を起こさない形にした（M4 の fence の側。swapchain ごとの絞りは p009 のまま）。
3. 待ちの context の fd: marker か共有の fence がある any-wait だけ POLLIN で待ち、無い時（hotplug と未提出の fence だけの待ち）は events=0（loss の POLLERR だけ）。設計に無かった点で、無いと hotplug の thread が同じ context の render の job の終わり（記録が消費されるまで POLLIN が立ち続ける）で起き、空回りする。
4. `vulkan_wsi_display_node_topology` は他に使う所が無いので消した（phase.md の「他に使う所が無ければ消す」）。
5. 登録の失敗: 今までは常に VK_SUCCESS（cursor を後で取る）。今は ENOSPC・ENOMEM・EMFILE・ENFILE を `VK_ERROR_OUT_OF_HOST_MEMORY`、他を `VK_ERROR_UNKNOWN` で返し、登録した分を戻す。EOPNOTSUPP と ENODEV の node は飛ばす。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: `make -j4 BUILD_JOBS=4 build/x86_64/packages/libvulkan/.stamp`、`make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk build/rpi4/packages/libvulkan/.stamp`（arm64）、`... CONFIGMK=config/config.vf2.mk build/vf2/packages/libvulkan/.stamp`（riscv64）。f93eb33fab と c44b5ba42c（merge の後）の両方で rc 0・warning 0。
- host の Venus の renderer: `sh plan/ws353/tests/build-venus-renderer.sh` → PASS。`build/ws035-sq-venus/install`。tarball https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/1.1.0/virglrenderer-1.1.0.tar.gz（sha256 9996b87bda2fbf515473b60f32b00ed58847da733b47053923fd2cb035a6f5a2）。WS014 の 1.1.0 の before・strict の after・quiesce の proposed の hash の照合は全部 ok。server の sha256 745bef59c17c88975ee567c0ae8c2c689b5b06fca79171d121227d0f5f5f84c2（WS014 の README と一致）、library 547223399c252d6c907be61c8b06fe8a887db23a8953ca726c22e7f32a4cd063（README の 38447dee… と違う、参考。build の環境で変わる）。host に libepoxy-dev が無く、Q1 が入れた。5330 と 10.0.10.25 には触れていない（5330 は読むだけで、tools.md の path は無かった）。
- QEMU（Venus、max_outputs=2、`-display dbus,gl=on`、runtime `build/ws353-p003-run`）: `sh plan/ws353/tests/qemu-hotplug-wait.sh build/ws353-p003-qemu`。program は一度だけ起動し、結果は guest の `/tmp/p3/result.txt` を serial の command で読む（console の log で判定しない）。起床の数は program の `poll()` で数える（rtld は main object を先に引くので libvulkan の poll が program の物になる。timeout 0 でない poll だけ、測る thread だけ）。最後の回（7 回目、13a1f1db32）:
  - idle-h（hotplug fence 単独を 30 s）: PASS、TIMEOUT、poll 1 回。idle-f（未提出 2 つの any、30 s）: PASS、poll 1 回。idle-h2（hotplug＋未提出、30 s）: PASS、poll 1 回。→ (2)
  - submit-f・submit-h（別の thread の空の submit で返る）: PASS、submit から 21・19 ms、poll 1・2 回。→ (3)
  - narrow-f・narrow-h（別の fence の submit・wait・reset を 200 回の間）: PASS、その間の poll 1 回、全体 1・2 回。→ (5)
  - race-f・race-h（待ちの開始の直後に乱数の spin と別の fence の submit の後で submit、各 5 s の上限、300 回）: PASS、取りこぼし 0。→ (6)
  - limit（register・destroy を 40 回の後、16 個生きたまま、17 個目は `VK_ERROR_OUT_OF_HOST_MEMORY`、1 個 destroy で再び登録できる）: PASS。→ (4)
  - plug（hotplug fence を `vkWaitForFences(30 s)`、host が marker を見て 2 s 後に head 1 を挿す）: PASS、poll 1 回、変化の開始から ≤983 ms。program 自身の kernel の fence（同じ `GPU_DISPLAY_FENCE` を /dev/gpu0 で）も同時に起き、Vulkan の待ちは 1 ms 後。reset した fence はその後の抜きで signal されない（spent）。unplug: PASS、≤214 ms、kernel の fence の 0 ms 後。→ (1)、(4) の reset
  - lost（context の fd を /dev/null に dup2 して次の submit を失敗させる。GPU を hang させない）: hotplug fence 単独の待ちと未提出 2 つの any-wait が両方 `VK_ERROR_DEVICE_LOST` で返る。submit は -4。PASS。→ (7)
  - 7 回の経過: 1・2 回目は renderer が無い・`VK_KHR_swapchain` の欠け（試験の program）で setup の FAIL。3・4 回目は main に BUG-431 の直しが無く、renderer への request の割り込みで topology の番号が上がり、待っている hotplug fence が signal された（hotplug と未提出の組・limit・lost が FAIL、libvulkan の側ではない）。5 回目（merge の後）全 step PASS だが submit-h・narrow-h が poll 130・180 回（下の「見つけた事」）。6 回目（直しの後）poll 2 回で全 step PASS、plug の上限が 1085 ms で latency の 1 s を越えた。7 回目で kernel の fence と比べる行を足し、全部 PASS（plug ≤983 ms、kernel の fence と同時）。
- 既存の libvulkan の試験（vkcube など）: 未実施（Q1 の割当が無い）。

### 見つけた事

- **any-wait の空回り（既存、直した）**: any-wait は context の fd の POLLIN を待つが、kernel は「終わって消費されていない record が一つでもある間」POLLIN を立て続け、reap は ledger が 8 以上の時だけだったので、短い ledger に他の終わった record が残ると poll がすぐ返り、待つ job が終わるまで回り続けた（5 回目の submit-h で 19 ms の間に poll 130 回）。未提出の fence の観測は renderer の往復で遅いので -f では隠れていた。眠る前に pin の無い終わった record を必ず reap する形にした（c44b5ba42c）。compositor の hotplug の thread の停止（停止の fence を submit してから返るまで）もこれに当たっていた。
- **plug の latency が 1 s に近い**: plug は 333〜1085 ms（4 回）、unplug は 210〜245 ms。program 自身の kernel の fence も Vulkan の fence と 0〜1 ms の差で起きるので、遅れは libvulkan より下（QEMU の D-Bus display の SetUIInfo から guest の事象まで、または venus の driver）。p002 の試験（BUG-431 の後）の 266・255 ms とも違う。原因は未調査（QEMU の ui info の遅延の timer の疑い、未確認）。p004 以降の判定で 1 s の上限が際どい。
- 試験の道具: Venus の Vulkan の試験は host に strict-queue の renderer が要り、どこにも写しが無かった（`build-venus-renderer.sh` で作れる。Q1 が plan/tools へ移す予定）。guest の image に `tail` が無い。

### 未実施

- 既存の libvulkan の回帰（vkcube・compositor の起動など）、i915・amdrdna などの実機（p016 で 5330）。
- arm64・riscv64 は build だけ（実行は未実施）。
- REFRESH の fence と `VULKAN_SYNC_POLL_NS` は p014 のまま（1 ms の見回りは未提出の REFRESH の fence と record の無い marker の時だけ残る）。

### p004 への申し送り

- hotplug の thread の待ち `vkWaitForFences(device, 2, {F, stop}, VK_FALSE, UINT64_MAX)` は、F の node の fd と wake の token（と loss だけを見る context の fd）で眠り、render の submit・present・image の遷移では起きない（narrow-h で確かめた）。停止は `vkQueueSubmit(queue, 0, NULL, stop)` で token が起きる（submit-h、19 ms）。device lost では `VK_ERROR_DEVICE_LOST` で返る（lost）。
- F の reset は使わず、G を登録してから F を destroy する（destroy で CANCEL と close）。登録は instance × node で 16 まで（limit）。登録の失敗は `VK_ERROR_OUT_OF_HOST_MEMORY` か `VK_ERROR_UNKNOWN`。topology の事象の無い renderer では fence は fd を持たず、待ちは token だけで眠る（stop と device lost でだけ返る）。
- QEMU の plug の latency が 1 s に近い（上）。p004 の判定は「look の回数」中心が安全。
