<!-- awesome-plan project=zedbsd record=ws353-design-review -->

# WS353 設計の敵対的レビュー（2026-11-13 Fable、Q1 の依頼）

- 対象: [design.md](design.md)・[ws.md](ws.md)（正）・phase002〜phase017 の phase.md（worktree `p1-ws353`、ffa65ddc77）。
- 照らした code: 同じ worktree の `userland/desktop/wayland/`・`libkeiland-backend-zedbsd/`・`libvulkan/`、`src/drivers/gpu/`（gpu.c・gpu-fence.c・i915・venus・videocore）、`include/uapi/gpu-display.h`・`gpu-fence.h`、`include/drivers/gpu/gpu.h`、`src/kern/poll.c`（main と WS352 の branch 82d2a01458）。file:line は特記の無い限り worktree の tree（483bec3718 と同じ）。
- 読んだ記録: AGENTS.md §4・§7、`plan/ws352/poll-report.md`（main の checkout）、`plan/ws309/design.md`、`plan/guardrail.md`。
- 書き方: 指摘ごとに重大度（重大・中・軽）、根拠、起こる状況、直し方の案。code で確かめられなかった所は「推測」と書いた。設計の文書と source は変えていない。

## 0. 要約

| 重大度 | 件数 |
| --- | --- |
| 重大 | 4（S1〜S4） |
| 中 | 9（M1〜M9） |
| 軽 | 12（L1〜L12） |

重大の 4 件はどれも「待ちを一つにした結果、起こす側が無くなる」形の抜け: render が idle で完了を見ない事による main との deadlock（S1）、present の lane を window mode の present wait に使い回す事による display thread・終了の hang（S2）、去った client の buffer の解放の自分への wake が busy loop になる事（S3）、display が止まっている間に render・cursor が acquire で無期限に block し park の打ち切りで compositor が落ちる事（S4）。いずれも設計の形を変えずに直せる（各項の案）。

91 件の対応表（§12）は件数・区分とも poll-report.md と一致した（自分で数えた: 表 1 19、表 2 44、表 4 1、表 5 9、表 6 13、表 7 5、計 91。区分の内訳も報告の §5 と同じ）。誤りは 1 行（L7）。

HAL（`include/hal/hal.h`）の変更は要らないという判断は正しい（`include/drivers/gpu/gpu.h` は driver と枠組みの口で、guardrail.md:121 が HAL と区別している）。Makefile・mk/ の変更が要らないという判断も、新しい file を作らない限り正しい（libvulkan の生成の `.inc`・`exports.map` は tree にある source で、Makefile は触れない）。ただし p009 の header の追加は `include/libc/vulkan/API-PROVENANCE.md`・`LICENSE-API` の更新を伴う（L10）。

## 1. 重大

### S1. render が idle で完了を見ない形は、main との deadlock を作る（§5.6、p011）

- 根拠:
  - design.md §5.6「完了の観測: render は起きた時（依頼・park）に飛んでいる slot の fence を `vkGetFenceStatus` で見て … idle では何もしない（何も要らない …）」。
  - main は slot の完了（今の COMPLETED、設計の REAPED）で `compose->in_flight` を減らし（compose.c:4791・4814・4833）、`slot->flying`・`slot->bake`・`slot->copies` を下ろす（compose.c:4782・4812）。
  - main が次の frame を頼めるかは `compose_may_draw`（compose.c:4356〜4396）: `in_flight >= KWL_FRAME_SLOTS` なら 0（4370）、飛んでいる slot に `copies` か `bake` があれば 0（4374〜4379）。shm の写しの条件も同じ（compose.c:4316〜4336、`kwl_render_running`）。`kwl_compose_complete`（compose.c:881〜884）は `in_flight` が 0 になるまで render の message を待つ。
  - layer の焼き（LAYER の want、present しない）の完了は REAPED でしか来ない（layer-cache.c:838〜841、compose.c:4816）。shot（compose.c:4836 `kwl_shot_complete`）、`scene.valid`（4825）、`kwl_layers_complete` も同じ。
- 起こる状況: (1) 静けさの timer で layer を焼く（bake の slot が飛ぶ）→ 焼き終わる → render は依頼が無いので cond で眠る → main は `slot->bake` が立ったままなので `compose_may_draw` が 0 で frame を頼めない → render に依頼が行かないので REAPED が来ない。画面が変わっても描けなくなる。(2) 2 frame が飛んで両方終わった後に何も来ない → `in_flight` が 2 のまま → 同じ。(3) present が失敗した frame（OUT_OF_DATE）は SHOWN が無いので、同じく REAPED 待ち。(4) `kwl_compose_quiesce`（compose.c:985〜）は render の message を待ち続ける。
  p010 の SHOWN で `in_flight` を減らす形にしても、bake（present が無い）と present の失敗は残る。
- 直し方の案（どれか、Q1 の指示「合成の完了のための thread を置かない」の中で）:
  - A（推し）: render は「依頼が無くても飛んでいる slot がある間」は `changed` の cond ではなく、最古の slot の fence を無期限に `vkWaitForFences` する（待つ物は一つのまま: slot が飛んでいる間はその fence、無ければ cond）。終わったら REAPED を出して cond に戻る。これで idle に入る前に全部の完了が main に届く。主な依頼（park・stop・cancel）は fence の待ちが返った後に見る（今の 1 ms の覗きが無期限になるだけで、順は同じ）。render が fence を待っている間に main が依頼を出しても、slot は最長で一つの frame の時間で空くので待ちは短い。
  - B: main は `in_flight` の判断を REAPED ではなく SHOWN（T4）と bake の完了の知らせで行い、bake は present しない代わりに小さな empty submit の fence を T4 に渡す。A より hop が増える。
  - どちらでも、§5.6 の「idle では何もしない」の文と、p011 の「起きた時に見て REAPED」を書き直し、p011 の host 試験に「bake の後に依頼が無い」「2 frame の後に依頼が無い」「present の失敗の後」を足す。

### S2. window mode の present wait を scanout の「present」の lane（T4）に載せると、display thread と終了が hang する（§5.1・§5.9・§9、p010）

- 根拠:
  - design.md §5.1 T4「依頼の列: game mode の present（同期の ioctl）と、window mode の frame の表示の完了（`vkWaitForPresentKHR`）」、§9「T4 の window mode の仕事は `scanout.c`（今の present の lane の持ち主）か `render-thread.c`」。
  - 今の lane は 1 本の thread で job を FIFO に走らせる（scanout.c:146・282、lane.c）。display thread は game chain を壊す前に `kwl_scanout_present_wait`（scanout.c:329〜340）＝`kwl_lane_wait`（lane.c:240〜241、`pending == NULL && running == 0` まで cond で待つ）を呼ぶ。終了は `kwl_scanout_stop` → `kwl_lane_stop`（join、lane.c:284〜291）。WS309 §8 の終了の順は T3 → T4 → … → T1・T2（compose の close、swapchain の破棄）。
  - libvulkan の `vkDestroySwapchainKHR` は worker が chain を使い終わるまで `swapchain_condition` で待つ（wsi-swapchain.c:1634）。設計の `vkWaitForPresentKHR` は「out of date・lost・retire で error」（§4.3）だが、`retired` が立つのは oldSwapchain での置き換えだけ（1155）。
- 起こる状況: (1) game mode に入る時、T3 が `kwl_scanout_present_wait` で lane の空きを待つが、lane には window mode の最後の frame の present wait の job が残っている（present が終わらない＝display が止まっている・head の display が遅い時は長い）。T3 の待ちは無期限で、main は `DISPLAY_SETTLE_MS`（10 s）で `_exit(1)`（display-thread.c:293〜299）。(2) 終了で T4 を止める時（T1・T2 の compose の close より前）、present wait の job が swapchain の retire を待っているが、swapchain はまだ壊されない → join が返らない。(3) lane の FIFO で game mode の present の job が present wait の後ろに並ぶ（mode の切り替えの直後）。
- 直し方の案: window mode の present wait は scanout の lane と分け、`compose` が持つ別の lane（名前は present-wait か shown。thread の台帳に 1 本足す）にする。その thread の停止は compose の close の中で、swapchain を壊す前に「待っている job を error で返す口」（libvulkan の `vkDestroySwapchainKHR` が waiter を起こして `VK_ERROR_OUT_OF_DATE_KHR` にする事を §4.3 に明記し、p009 の host 試験に足す）を使う。display の切り替え（出力の作り直し）でも同じ口で待ちが終わる事を p010 の確かめに足す。§5.1 の表と §9 の置き場所を直す。thread の数は 17 になる（§5.1「16」を直す）。

### S3. 去った client の buffer の解放を「自分の wake pipe に 1 byte」で回すと、idle で busy loop になる（§5.2、p005・p010）

- 根拠: design.md §5.2「去った client の buffer の解放（main.c:1115 の 1 pass に数個）: 残りがあれば自分の wake pipe に 1 byte 書いて直ちに次の pass にする」。`kwl_retire_ready`（objects.c:742〜781）は時刻で判断する: `retire_next_ms`（前の解放の時刻からの間隔）、`RETIRE_INPUT_QUIET_MS`（入力の静けさ）、`RETIRE_STARVE_MS`（待たせ過ぎの上限）、`in_flight`、`dirty`。今の main は `retire_ready` の時だけ timeout 0、それ以外は 10 ms（main.c:1047〜1049）。
- 起こる状況: 残りがあり `retire_ready` が 0 の間（間隔の前、入力の直後、frame が飛んでいる間）、main が自分に 1 byte 書くと poll(-1) が直ちに返り、何もせずにまた 1 byte 書く。1 pass ごとに全 client の poll の集合を作り直すので、idle の CPU が 100 % になる。WS353 の目標（idle で全 thread が眠る）の正反対。
- 直し方の案: `kwl_retire_ready` の時刻の条件（`retire_next_ms`・静けさ・starve）は timer（§5.5）に arm する（残りがある間だけ）。`in_flight` と `dirty` の条件は SHOWN・REAPED・描き終わりの事象で見直す。自分への wake は「今 `retire_ready` が 1 で、1 pass の数を越えた残りがある」時だけ。§12 の 2-12 の行き先を「形: timer と事象」に直す。

### S4. display が止まっている間の render・cursor の無期限の acquire は、park の打ち切りで compositor を落とす（§5.6・§5.7、p011・p012）

- 根拠:
  - design.md §5.6 (3)「`vkAcquireNextImageKHR(UINT64_MAX)`（FIFO では次の image は最長で一つの refresh の後に空く）。先回りの acquire（`ahead`）は前の present の後に同じく block」、§5.7「present の後に次の image を `vkAcquireNextImageKHR(UINT64_MAX)` で先に取っておく（最長で一つの refresh）」。§5.6 の確かめる点は render だけで、cursor には無い。
  - libvulkan の display の FIFO: image が空くのは前の front が present で外れた時（wsi-display.c:1491〜1498、`previous = lease->plane->front`）。kernel の present は i915 では前の flip が latch するまで待つ（present.c:37「a presentation returns once its flip is armed」、419〜424 `i915_present_latch`、`drv_i915_xd_picture_flip_wait`）。refresh の境が無い出力は境を作らない（gpu-display.h:261〜262「An output that does not scan out makes no boundary: ETIMEDOUT」）。
  - park の打ち切り: render の park は `kwl_render_park` の呼び手の上限（100 ms ずつ最大 10 s、display-thread.c:293〜299 で `_exit(1)`）、cursor の park は 100 ms で「答えない thread は compositor を終える」（cursor-plane.c:493〜497 の説明、cursor-thread.c:314〜349）。
  - sleep の経路: `sleep_hold` の間は scheduler が描かず cursor を hold する（display.c:177〜180）だけで、render を park しない。suspend は `kl_backend_power_action(KL_BACKEND_POWER_SUSPEND)`（sleep.c:560）で、driver が display を park する（display.c:176 の説明）。消灯（`kwl_screen_off`、backend-host.c:727〜）は backlight だけで scanout は続く（確認済み: compositor に `vkDisplayPowerControlEXT` の利用者は無い）。
- 起こる状況: suspend の直前（`sleep_hold` を立てた後）に render の `ahead` の acquire か cursor の先回りの acquire が、前の present の latch を待つ kernel の present の後ろで block している。suspend で driver が display を park し、flip が latch しない（i915 の `flip_wait` の打ち切りの有無は未確認＝推測。venus の virtual clock は続くので QEMU では出ない）。復帰の後の mode の切り替え（出力の作り直し）で park を頼むと、thread は acquire の中なので答えず、10 s で compositor が落ちる。同じ事は「出力が別の display に移る時」（`output_recover`）に head の display が止まっている時にも起こり得る。
- 直し方の案:
  1. 先回りの acquire（render の `ahead`、cursor の次の image）は「present の直後」ではなく「次の依頼（want・box の変化）が来た時」に行う。idle では何も acquire しない（待つ物は依頼の cond だけ）。FIFO で image が空くまでの待ちは依頼の後の 1 refresh で、今と同じ。
  2. それでも block 中に park が来る場合のため、libvulkan の acquire の待ち（`swapchain_poll`、wsi-swapchain.c:4781〜4825）が display の電源・lease の終わり（driver の park）で `VK_ERROR_OUT_OF_DATE_KHR` を返す事を p011・p012 の「先に確かめる事」に足し、i915 の `drv_i915_xd_picture_flip_wait` の打ち切りを読む（EIO なら present が失敗し、acquire は OUT_OF_DATE で返る）。
  3. §5.7 に cursor の同じ確かめを足す。

## 2. 中

### M1. kernel の fence の signal には owner の bind が要る（§3.1、p002）

- 根拠: `drv_gpu_fence_signal_deferred`（gpu-fence.c:417〜466）は `owner == NULL || fence->owner != owner` で EPERM、`drv_gpu_fence_bind`（258〜300）は PENDING かつ owner 無しの generation だけ受ける。`drv_gpu_fence_create(device, signaled, &handle)`（45〜91）の `device` は node の識別子で、`drv_gpu_fence_device`（94〜116）が照合する。
- 状況: 設計 §3.1 は「gpu-fence.c の payload（`drv_gpu_fence_create`、未 signal）を作り … 登録に fence の参照を入れ … signal」としか書かず、p002 は「足りなければ最小の口を足す」。bind 無しでは signal できない。error の signal（offline・故障）は `status` が正の errno（signal_deferred:432〜433）。
- 案: 登録の時に `drv_gpu_fence_bind(handle, 1, registration)` し、push で `drv_gpu_fence_signal(handle, 1, registration, 0)`、offline・故障で `status = ENODEV` など。登録の list は handle の参照を持ち、外す時に `handle_put`。`device` は display node の識別子（libvulkan は fd しか使わないので render node との照合は起きない）。これを §3.1 と p002 の「変える file」に書く。

### M2. WS352 の branch の名指しの poll と合わせないと、fence の fd の poll は「全部の変化で起きる」(§3.1・§7、p002・p003)

- 根拠: branch の poll.c（82d2a01458）は、descriptor の poll が object を名指ししない時その poll を `every` にする（poll.c:447〜449・poll_scan）。gpu-fence.c の `fence_poll`（545〜575）は `poll_wait_object` を呼ばない（branch でも同じ）。名指し無しの `poll_notify()` は gpu.c:651（job の完了）・716・897・956、gpu-fence.c:248・407、i915 hpd.c:699・1233 など、GPU を使う間ずっと出る。
- 状況: branch を merge した後（設計 §7 の推し）、hotplug の thread（fence の fd を poll）は GPU の job が終わるたびに起きて見直す。p002 の判定「抜き差しの無い 30 s は起きない」、p003 の (2) は、client が描いている間は成り立たない。idle では GPU の job が無いので電力には効かないが、原則 1（外の事象でだけ起きる）と試験の判定が崩れる。
- 案: `fence_poll` で `poll_wait_object(fence)`、`drv_gpu_display_changed` と fence の signal で `poll_notify_object(fence)`（branch の形）。設計 §3.2 の「`poll_notify()` を中で呼ぶ」は、POLLPRI の待ち手（session を名指し、branch の gpu.c:2451〜2460）のために残すが、fence の分は名指しにする。branch を merge しない場合は今の全員起こしのままで、試験の判定を「GPU の job が無い 30 s」に限る。

### M3. hotplug の thread の終わり: device lost の後に停止の fence を提出できない（§5.3・§4.2、p004）

- 根拠: 停止は main が `vkQueueSubmit(queue, 0, NULL, stop)`（§5.3）。device lost（render node の job の打ち切り、`CONFIG_GPU_JOB_EXECUTION_MS`）の後は submit が `VK_ERROR_DEVICE_LOST` で返り、stop は signal されない。hotplug の fence は display node の open の登録なので、render node の故障では kernel は signal しない（§3.1 の「device の offline・故障」は display node の話）。libvulkan の `vulkan_device_error` は wake の token を起こす（objects.c:705 `vulkan_wake_notify(device, NULL)`）が、設計 §4.2 の wake の登録は「未提出の fence を含む待ち」にしか書かれていない。
- 状況: GPU の fault の後の終了で、hotplug の thread が `vkWaitForFences` の中で起きず、join が返らない（推測: §4.2 の wake を登録していれば `sync_fence_status` の device error の枝で返るので、書き方の問題）。
- 案: §4.2 に「事象の fence の待ちも wake の token を登録し、device error で `VK_ERROR_DEVICE_LOST` を返す」と明記し、p003 の確かめに「device error（`vulkan_device_error` を試験の program から起こす）で待ちが返る」を足す。compositor の側は、submit が失敗したら join の前に `vkDeviceWaitIdle` 相当ではなく、thread の返りを待つ理由（device error で返る）を §5.3 に書く。

### M4. `vulkan_wake_notify` の全員起こしで、acquire・present wait・未提出の fence の待ち手が submit と present のたびに起きる（§4.2・§4.3）

- 根拠: `swapchain_notify_locked` は `vulkan_wake_notify(NULL, NULL)`（wsi-swapchain.c:4835、全部の token）。設計 §4.2 は `vkQueueSubmit`・`vkResetFences`・import で `vulkan_wake_notify` を呼ぶ。§4.3 の `vkWaitForPresentKHR` も「`swapchain_condition`（と wake の token）」で待つ。
- 状況: hotplug の thread（停止の fence が未提出なので wake を登録）は render の submit と present のたびに起きて観測して寝直す。cursor の acquire の待ちも render の submit で起きる。描画中だけで idle では無いが、原則 3・4 と p003 (2)・p012 の判定（「timeout の起床」ではなく「起床の数」で見れば）に効く。
- 案: `vulkan_wake_notify` に理由（device の error・この swapchain の image の遷移・この fence の提出）を付け、token は自分が待つ物の分だけ起きる形にする（token に swapchain か fence の識別子を持たせる）。少なくとも設計に「余分な起床があるが idle では無い」と書き、試験の判定を timeout の起床に限る。

### M5. hotplug の thread を window mode の前から動かすと、FIRST・HOTPLUG の列挙が window mode の外で走る（§5.3・§5.4、p004）

- 根拠: 今の tick は `!server->windowed` と `compose->listing`・`kwl_display_busy` で止める（output-switch.c:158〜163）。game mode（direct scanout）では window mode を出ている（display.c:188〜192）。設計 §5.4 の `follow_pending` は「display が busy か列挙の途中」だけ。
- 状況: game mode の間の hotplug（HOTPLUG の知らせ）で main が OUTPUT_LOOK_SIGNAL の列挙を display thread に頼む → `output_listed` が出力の移動や head の作り直しを window mode の無い状態で行う（今は起きない経路。推測: 失敗か二重の mode の切り替え）。起動の直後（window mode の前）の FIRST も同じ。
- 案: `follow_pending` の条件に「window mode でない」を足し、window mode に入った時（`enter_window_mode` の後）に `kwl_output_follow` を呼ぶ。thread の開始は今と同じ「window mode の最初の pass」でよい（開始が遅れても hotplug の fence は登録の時の番号から数えるので取りこぼさない）。

### M6. 「present の完了」は「表示の完了（vblank）」ではない（§4.3・§5.9、p009・p010）

- 根拠: kernel の present は flip を armed した所で返る（i915 present.c:37、419〜424: 前の flip の latch を待ってから今の frame を armed）。venus は virtual clock（venus display.c の説明）。libvulkan の worker は kernel の present が返った所で `presented_id` を上げる（設計 §4.3）。
- 状況: SHOWN(n) は「frame n が armed され、n-1 が画面に出た」時に来る（i915）。frame の callback と animation の歩調は結果として refresh に合う（latch の待ちがあるため）が、「表示の完了」ではない。`vkWaitForPresentKHR` の仕様（present が画面に出た）と微妙に違うが、実用上は許せる。
- 案: §4.3・§5.9 の語を「present が受け付けられ前の flip が latch した時（i915）／virtual clock の present の時（venus）」と正しく書く。p009 の判定「返りの間隔が refresh に合う」はそのまま。

### M7. 時計の分の timer は実時刻で、CLOCK_MONOTONIC の timer thread では追えない（§5.5、2-17、p008）

- 根拠: poll-report.md 表 2「main.c:1577 実時刻の次の分（実時刻が読めない時 1 s）」。設計 §5.5 の timer thread は `pthread_cond_timedwait`（CLOCK_MONOTONIC）だけ。
- 状況: 時刻の設定（ntp の step、時差の変更）の後、次の分の境が monotonic の予定とずれ、時計の表示が最長 1 分遅れる。
- 案: 時計の timer だけは実時刻の次の分までの長さを monotonic に直して arm し、handler で実時刻を読み直して次を arm する（大きな step は次の発火で直る）。設計に書く。

### M8. `drv_gpu_display_ops` に `refresh_arm` を足すと、位置で初期化している driver が崩れる（§3.2、p014）

- 根拠: venus の `drv_venus_display_operations`（venus display.c:165〜171）は位置で初期化している（`NULL, NULL, NULL, NULL, 0, NULL, NULL`）。他の driver も同じ形の可能性（未確認）。
- 案: 新しい op は struct の最後に足し、p014 で 8 つの driver の初期化を確かめる。位置の初期化を名前付きに直すのは coding-style の範囲で p017。

### M9. i915 の `drv_gpu_display_changed(device, …)` に渡す `drv_gpu_device` が hpd.c から見えるか（§3.2、p002）

- 根拠: `drv_gpu_register` は i915.c:211 の 1 か所（`device->gpu`、i915.h:128）。`drv_i915_hpd_topology_touch(struct i915_display *display)`（hpd.c:680〜700）と publish（1215〜1235）は `i915_hpd_world` を通して `display` を持つ（hpd.c:84）が、`i915_display` から `i915_device` への backpointer は確かめていない（推測: 無ければ hpd の world に `drv_gpu_device *` を持たせる）。bootfb・capture は別の node ではなく同じ device の op の変種なので（`drv_gpu_register` が 1 回）、§2.1・§3.2 の「bootfb・capture は番号が動かないなら呼ばない」は「同じ device なので呼ぶ所は hpd.c だけ」と書き直す。

## 3. 軽

### L1. uapi の名前の衝突と ioctl の番号（§3.1、p002）

- `GPU_DISPLAY_EVENT_CHANGE`・`GPU_DISPLAY_EVENT_ACK` は `struct gpu_display_events` の flag として既にある（gpu-display.h:21〜22）。新しい `GPU_DISPLAY_EVENT_FENCE`（ioctl）、`GPU_DISPLAY_EVENT_HOTPLUG/REFRESH`（event の種類）、`GPU_DISPLAY_EVENT_FENCE_CANCEL`（flag）が同じ接頭で意味が混ざる。案: `GPU_DISPLAY_FENCE`（ioctl）・`GPU_DISPLAY_FENCE_HOTPLUG/REFRESH`・`GPU_DISPLAY_FENCE_CANCEL` に。ioctl の番号は 'G' の 51 以降（今の最大は 50、gpu-job.h:30）を設計に書く。CANCEL は `fd` が入力になるので、別の ioctl（`GPU_DISPLAY_FENCE_CANCEL`、`_IOW`）の方が ABI の説明が簡単。

### L2. `vkQueuePresentKHR` が直ちに失敗した frame の SHOWN（§5.9、p010）

- present が `VK_ERROR_OUT_OF_DATE_KHR` などで直ちに返った frame には present id が無い。T4 に積む物が無いので、render が SHOWN(n, 結果) を自分で出口に積む（順は T4 の SHOWN と前後し得る: frame の番号で main が並べる）と §5.9 に書く。callback はその時に送る（「present が失敗しても callback は送る」の実装）。

### L3. §4.2 の wake の登録の順（lost wakeup）

- `sync_fences_wait` は device->mutex の下で `notification` を snapshot する（sync.c:1612〜1630）。wake の token は、この snapshot の前に登録（と drain）し、snapshot の後に poll する順でないと、間に来た submit の知らせを落とす。§4.2 に順を書き、p003 の (3) の試験を「別の thread の submit を待ちの直前に何度も」で回す。

### L4. T4 の FIFO で head の present wait が output の SHOWN を遅らせる（§5.9）

- T4 が output と head の present wait を順に待つと、遅い head（30 Hz の外部 display）の分だけ output の frame の callback が遅れる。案: output の wait が返った所で SHOWN(n) を出し、head の分は head ごとの SHOWN（missed の want し直しに使う）にする。

### L5. 92 番目: `frame_fd`（main.c:91・994・1076、kwl.h:1565）は -1 のまま使われない

- 設計 §5.2 の poll の集合の整理で消す（p005）。

### L6. 1-12・§1 原則 7 と cursor の停止の `KWL_CURSOR_FENCE_WAIT_NS`（cursor-thread.c:295）

- 停止の時の copy の fence の待ちは打ち切り（stop_failed）で残す。1-12 の「copy の fence は無期限」は present の経路の話で、停止の経路は §5.10 に入れる（今は無い）。

### L7. 対応表 1-13 の行き先（§12）

- 1-13 の compose.c:5138（`compose_op_wait`、slot の fence の timeout）・5194（acquire の timeout）は p011 が無期限にする物（§5.6）なので「残」ではなく「形（compose.c の分は p011 で無期限、scanout.c:1065・1070 だけ残）」。件数は変わらない（1 行）。

### L8. 2-12 の行き先（§12）

- S3 のとおり「形: timer と事象」に。

### L9. §5.8 の hold の数に scanout（game mode）の hold が無い

- kwl.h:496「current and scanout holds」。`kwl_buffer_put` の「protocol と current の分だけ」に game mode の lend の hold を足す（scanout.c:420 の説明）。

### L10. p009 の header の出典（§9）

- `include/libc/vulkan/vulkan_core.h` は Khronos の header からの派生で、版は `API-PROVENANCE.md`・`LICENSE-API` に固定されている（vulkan_core.h:11〜18）。present id・present wait の宣言を足す時は同じ版から写し、`API-PROVENANCE.md` に足した範囲を書く。§9 と p009 の「変える file」に加える。

### L11. Phase の依存

- p015（network の答えの fd、WSI の 50 ms）は p005 に依らない（別の file、別の thread）。独立にしてよい（並べて進められる）。
- p014 は gpu.c の登録の list（p002）と 8 つの driver を変えるので依存は p002（p003 経由で満たされる）。p014 の「変える file」に videocore・ghci の display（`GPU_CAP_DISPLAY_CONTROL` の有無で分かれる）を明記。
- p011 の host 試験は WS309 の `render-task` の試験を広げるとあるが、S1 の場面（bake の後に依頼が無い、2 frame の後に依頼が無い、present の失敗）を項目に入れる。

### L12. 設計の記述の細かな食い違い

- §5.1「常駐は WS309 の 14 に hotplug と timer の 2 を足して 16」: S2 の案で 17。
- §2.6「render の acquire は 50 ms（`KWL_RENDER_ACQUIRE_NS`）か 1 ms」: 正しい（render-thread.c:731〜733）。§2.6「cursor … acquire は timeout 0 で、空きが無ければ 1 ms 後に再試行」: 正しい（cursor-thread.c:443〜447・263）。
- §2.4「fence を出せない device（i915 など）では client の WSI が描き終わりを待ってから commit する（wsi-wayland.c:793〜815）」: 読んでいない（推測のまま）。
- §3.1「登録は open ごとに上限 16」: compositor は F と G の 2 つ（node の数の倍）なので足りる。libvulkan の display node の open が process で一つか（複数の VkDevice が同じ open を共有するか）は未確認。
- p004 の確かめ「`KWL PERF` の `timeouts` に hotplug の分が無い」: main の 10 ms が残る間は timeouts で区別できないので、既に書いてある「look の回数が 0」だけでよい。

## 4. 確かめて問題の無かった事（根拠だけ）

- ユーザーの決定・合意した形との対応: 決定 2（kernel が signal する fence、問い合わせの撤去）は §3・§4.1、決定 4（slot の数で回す、合成の完了の thread を置かない）は §5.6、決定 5（client の buffer を slot から外す）は §5.8、「周期と保険の timeout を置かない」は §1・§5.2・§12。未決 2 の A（main が backend の fd と acquire fence も poll）は合意した形の「socket と wake だけ」と違う事を design.md 自身が確認事項に挙げている（§8）。
- `vkQueueSubmit(queue, 0, NULL, fence)` の経路: queue.c:223〜255 が同じ形（空の submit で private の fence を signal して待つ）を既に使っている。p003 の「先に確かめる事」は済んでよい。
- 91 件の対応表: 件数と区分は poll-report.md §5 と一致（自分で数えた）。
- gpu.c の lock の順: 既に registry の lock の下で fence の spinlock を取る（gpu.c:606〜620）ので、§3.2 の「registry の lock と fence の spinlock」の順は今の順。`LOCK_RANK_DEVICE`（125）< `LOCK_RANK_POLL`（140）なので poll の通知も fence の lock の外で呼べる。
- HAL・Makefile・toolchain: 変えない判断は正しい（guardrail.md:121、新しい file 無し、生成の `.inc`・`exports.map` は tree の source）。
- 消灯（`kwl_screen_off`）は backlight だけで scanout は続く（backend-host.c:727〜）。S4 は suspend と出力の移動の時だけ。

## 5. 確かめられなかった事（推測として書いた物の一覧）

- i915 の `drv_i915_xd_picture_flip_wait` の打ち切りの有無と、display を park した後の present・acquire の返り方（S4）。
- `i915_display` から `i915_device`（`drv_gpu_device`）への backpointer（M9）。
- 8 つの `events` の実装が open によらない事（設計 §13 と同じ、p002 の確かめ）。
- `vulkan_device_error` が、事象の fence だけを待つ poll を起こすか（M3）。
- libvulkan の display node の open の数（L12）。
- wsi-wayland.c の acquire fence の無い device の経路（§2.4、読んでいない）。
