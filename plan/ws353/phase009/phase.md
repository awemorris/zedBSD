<!-- awesome-plan project=zedbsd record=ws353-p009 -->

# ws353-p009: libvulkan の present の fence（VK_EXT_swapchain_maintenance1）

Status: cleared（2026-11-14 04:01 P2、q2063-i01。build 3 機種 warning 0、QEMU（Venus）の 11 step が 2 回とも PASS（下の「結果」）。Wayland の swapchain の fence・既存の回帰は未実施。Q1 の確認待ち）
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

## 結果（2026-11-14 P2、q2063-i01、branch `agent/p2-ws353a`、基点 main 2c1152a565）

### commit

- 9f6e89c68c: 宣言と libvulkan（`include/libc/vulkan/vulkan_external.h`・`API-PROVENANCE.md`、`userland/desktop/libvulkan/` の internal.h・objects.c・instance.c・device.c・external-properties.c・wsi.c・wsi-swapchain.c・dispatch-table.inc・api-commands.tsv・tools/maintain-external.noct・tools/maintain-dispatch.noct）。
- 05fe512c28: QEMU の試験（`plan/ws353/tests/present-fence-test.c`・`qemu-present-fence.sh`・`config-amd64-present.mk`）と objects.c の comment の直し。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 先に確かめた事

- v1.3.269 の header に 3 つの extension の宣言がある: virglrenderer 1.1.0 の `src/venus/venus-protocol/vulkan_core.h`（p003 の renderer の build の展開、`API-PROVENANCE.md` の固定の SHA-256 8cb01233… と一致、`VK_HEADER_VERSION 269`）の 9269 行（`VK_KHR_get_surface_capabilities2`）・14301 行（`VK_EXT_surface_maintenance1`）・14346 行（`VK_EXT_swapchain_maintenance1`）。tree には取り込まず、scratch に写して tool の入力にした。
- 依存（host の `/usr/share/vulkan/registry/vk.xml` 1.4.309）: swapchain_maintenance1 は `VK_KHR_swapchain+VK_EXT_surface_maintenance1+(VK_KHR_get_physical_device_properties2,VK_VERSION_1_1)`、surface_maintenance1 は `VK_KHR_surface+VK_KHR_get_surface_capabilities2`、caps2 は `VK_KHR_surface`。
- 仕様（Vulkan-Docs main の `chapters/VK_KHR_surface/wsi.adoc` を scratch に取得して読んだ）: common_present_fence（queue の操作の完了と presentation engine が参照を取った後に signal、表示の完了は待たなくてよい）、同じ swapchain・同じ VkQueue の present の fence は present の順、swapchain は過去の present の fence が全部 signal した後に破棄してよい、vkQueuePresentKHR が積めずに OOM を返す時は同期の物を変えない、OUT_OF_DATE・SURFACE_LOST は積まれたと見なす、vkReleaseSwapchainImages は acquire 済みで使用の終わった image、VkSwapchainPresentModesCreateInfo は surface の互換の部分集合で作成の presentMode を含む。実装はこれに合わせた。
- `LICENSE-API` は Apache-2.0 の本文だけで範囲の記述を持たない（範囲は `API-PROVENANCE.md`）。足した宣言は同じ入力・同じ許諾なので変えていない。

### 実装

- 宣言: `maintain-external.noct` の extension の一覧に 3 つを足し（`#define VK_EXT_` の block も選ぶ、command の数 16 → 19）、file の頭を tree の形に合わせた。出力は今の `vulkan_external.h` と、足した block と頭の説明の 2 行だけが違う（`vulkan_core.h` の末尾が include するので `vulkan_core.h` から見える）。`maintain-dispatch.noct` は 2 つの marker と数（197 → 200）を足し、device extension の physical-device の command を `VULKAN_ENTRY_PHYSICAL` で出す形にした（tree の `dispatch-table.inc` は video の 2 つを手で PHYSICAL に直してあり、tool の出力と食い違っていた）。今の tool の出力は tree の表と 3 行の追加だけが違う。`API-PROVENANCE.md` に節を足した。
- extension と feature: instance に caps2・surface_maintenance1（常に offer、依存を検査）、device に swapchain_maintenance1（swapchain と同じく全 renderer で offer、swapchain・surface_maintenance1・properties2 の有効を検査）、`vkGetPhysicalDeviceFeatures2KHR` が `swapchainMaintenance1` を答える。
- surface（wsi.c）: `vkGetPhysicalDeviceSurfaceCapabilities2KHR`（scaling・gravity は 0、scaled extent は image extent の範囲、present mode の互換はその mode 自身だけ）、`vkGetPhysicalDeviceSurfaceFormats2KHR`。
- swapchain（wsi-swapchain.c）: 作成で `VkSwapchainPresentModesCreateInfoEXT`（作成の mode だけ）・`VkSwapchainPresentScalingCreateInfoEXT`（全部 0）を検査、extension を有効にした device では `DEFERRED_MEMORY_ALLOCATION` の bit を受ける（今どおり作成で確保）。present で `VkSwapchainPresentModeInfoEXT`（chain の mode だけ）と `VkSwapchainPresentFenceInfoEXT`（job が fence の配列の写しを持つ）。`vkReleaseSwapchainImagesEXT`（全 index が ACQUIRED の時だけ AVAILABLE に戻し、その chain の acquire を起こす）。
- present の fence の signal: job が retire する所（`present_finish`）で、present が積まれた（`job->submitted`）なら、chain の `pending_jobs` を減らす前に各 fence を acquire と同じ software の signal（`vulkan_wsi_acquire_signal`）で signal する。worker の job は native の present（display は kernel の present の返り、Wayland は commit）の後、失敗した job も同じ所を通る。積まれなかった present（検証の失敗・OOM）は fence を変えない。device lost の時は signal せず、待ちは p003 の token で `VK_ERROR_DEVICE_LOST` を返す。overlay を隠す present は fence があれば worker に回す（fast の経路で前の job を追い越さない。他の fast の経路は pNext に fence があると既に COPY）。
- swapchain の wake の絞り: `struct vulkan_wake` に acquire する swapchain を持たせ（`vulkan_wake_create(wake, device, swapchain)`）、`vulkan_wake_notify_swapchain(chain)` はその chain の image の待ちだけを起こす。`swapchain_notify_locked(chain)` は image の状態・retire・error の変化（present の retire、retire、acquire の失敗の戻し、release）で、job の publish・worker の current の解除・worker の停止は cond だけ（`swapchain_signal_locked`、image は変わらない）。chain を名指せない window system の release（`vulkan_wsi_image_notify`: wsi-display の overlay の採用、Wayland の buffer の release）は今どおり全部の image の待ちを起こす。

### 設計からの変更（理由）

1. 宣言の置き場所: phase.md は `vulkan_core.h` に写すとしたが、`vulkan_external.h`（同じ v1.3.269 の入力を tool で選び出し、`vulkan_core.h` の末尾が include する file）に足した。`vulkan_core.h` は 1.0 と直接表示の選び出しで、tool（`maintain-api.noct`）の数の検査を変えずに済み、include の利用者からの見え方は同じ。
2. present mode の互換: phase.md は「FIFO だけ」。Wayland の surface は FIFO と MAILBOX を出すので「どの mode も自分自身とだけ互換」（swapchain は作成の mode を変えない）にした。display は FIFO だけなので同じ結果。
3. device lost の fence: 設計 §4.3 は「fence も lost」。実装は signal せず、待ちが `VK_ERROR_DEVICE_LOST` で返る（p003 の token が device の error で起きる）。仕様の device lost の扱いの範囲。
4. wake の絞りの範囲: chain を名指せない adapter の release は全部の image の待ちを起こすまま（compositor の render の present では起きない。overlay plane の有る出力と Wayland の client だけ）。
5. dispatch の tool の直し（上）: 今の表の再生成を正しくするため。範囲の外の変更は無い。
6. 判定「返りの間隔が refresh に合う」: Venus では 1 present に refresh 3 つ分（40 ms、mode 75 Hz の 13.3 ms×3）かかり、fence の無い present も同じ 40 ms（pace の step）。だから判定は「fence の間隔が present の間隔の ±25%、かつ refresh の 3/4 以上」にした（refresh の歩調そのものは Venus では見えない。i915 の実機は p016）。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror、header を変えたので先に `make sysroot`）: `make -j4 BUILD_JOBS=4 sysroot`・`make -j4 BUILD_JOBS=4 build/x86_64/packages/libvulkan/.stamp`、`CONFIGMK=config/config.rpi4.mk`（arm64）・`config/config.vf2.mk`（riscv64）で同じ。最終の source（05fe512c28）で 3 機種とも rc 0・warning 0（`build/p9-x86-final.log`・`p9-rpi4-final.log`・`p9-vf2-final.log`）。基点でも 3 機種 rc 0・warning 0 を先に確かめた。
- 宣言の tool: `build/NoctLang/build-static/noct userland/desktop/libvulkan/tools/maintain-external.noct <v1.3.269 の写し> <out>`（自分の build の NoctLang、p2-bug431 の build から copy）が tree の `vulkan_external.h` と一致、`maintain-dispatch.noct include/libc/vulkan/vulkan_core.h userland/desktop/libvulkan` が tree の表と一致（maintained 200 dispatch records）。
- QEMU（Venus、max_outputs=2、`-display dbus,gl=on`、runtime `build/ws353-p009-run`、renderer は p1-ws353e の `build/ws035-sq-venus/install` を copy）: `sh plan/ws353/tests/qemu-present-fence.sh build/ws353-p009-qemu`。program は一度だけ起動、結果は guest の `/tmp/p9/result.txt` を serial の command で読む（console の log で判定しない）。3 回目（`build/p9-qemu-run3.log`）と 4 回目（`build/ws353-p009-qemu2`、`build/p9-qemu-run4.log`）が全 step PASS。4 回目の値:
  - ext: PASS（instance の 2 つ・device の 1 つが offer、feature が VK_TRUE、device はその 2 つと feature で作れる）。
  - caps2: PASS（FIFO の互換は 1 個で FIFO、scaling 0、formats2 は formats と同じ 2 組、1 個の配列で VK_INCOMPLETE）。
  - create: PASS（{FIFO, MAILBOX} は VK_ERROR_INITIALIZATION_FAILED、{FIFO}・scaling 0・DEFERRED は作れる、image 2）。
  - release: PASS（2 個とも acquire、1 個を release、2 度目は refuse、timeout 0 の acquire でその 1 個が返る）。
  - unenqueued: PASS（acquire していない image の present は VK_ERROR_INITIALIZATION_FAILED、fence は 200 ms 後も NOT_READY）。
  - frames: PASS（100 present の fence を別の thread が `vkWaitForFences(any, UINT64_MAX)` で待ち、100 個とも present の順に返る、順の違反 0。間隔の中央値 40 ms、present から signal まで 40 ms、present の間隔 40 ms）。
  - pace: PASS（fence 無しの 60 present の間隔 40 ms。fence で present は遅くならない）。
  - narrow-frames・narrow: PASS（head 1 の image を全部持ち、その次の acquire を 3 s の timeout で待つ thread の blocking な poll は 1 回、その間に head 0 が fence 付きで 60 present。前の形では publish と retire のたびに全部の image の待ちを起こしていた）。
  - destroy: PASS（3 present を待たずに `vkDestroySwapchainKHR`、返った直後に 3 個とも signal 済み）。
  - unplug: PASS（host が head 1 を抜き、present が `VK_ERROR_SURFACE_LOST_KHR` に。積まれた 25 present の fence が全部返る、marker から 2.4 s）。
  - 経過: 1 回目は frames が FAIL（毎 frame に clear と fence の作成・reset の renderer の往復があり 93 ms/frame で、program の速さが歩調を決めていた）。program を semaphore の acquire と事前に作った fence に直し、2 回目は frames の後の「全部の image を持つ」準備が表示中の image のため timeout し narrow-frames・destroy が FAIL（試験の手順の誤り）。準備を最初の 1 回だけにし、pace の step と判定（上の 6）を足して 3 回目から PASS。
- host の偽の adapter の試験: 未実施（worker を偽の adapter で動かす道具が無い。QEMU で fence の順・失敗・破棄を確かめた）。

### 未実施

- Wayland の swapchain（compositor の中の client）の fence: 未実施（compositor の起動の要る image で、この Phase の QEMU の script には入れていない。fence の signal は display と同じ `present_finish` の経路、Wayland では commit の後）。
- 既存の libvulkan の回帰（vkcube など）: 未実施（Q1 の割当が無い）。
- OOM で積めなかった present の fence が変わらない事、present の fence の待ちの device lost: 実行での確認は未実施（OOM は起こす道具が無い。積まれなかった present は unenqueued の step で、device lost は p003 の lost の step の経路）。
- arm64・riscv64 は build だけ。i915 などの実機（p016）。
- 同じ swapchain の present の順は「同じ queue の同じ lane」で保たれる。cursor・overlay の chain を他の chain と一緒に 1 回の present に入れると ordinary の lane に回り、単独の present（cursor の lane）と順が前後しうる（compositor はしない使い方）。

### p010 への申し送り

- 有効にする物: instance に `VK_KHR_surface`・`VK_KHR_display`・`VK_KHR_get_physical_device_properties2`・`VK_KHR_get_surface_capabilities2`・`VK_EXT_surface_maintenance1`、device に `VK_KHR_swapchain`・`VK_EXT_swapchain_maintenance1`（`VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT` を pNext に）。
- present の fence は worker が job を retire する時に signal（display は kernel の present の返りの後）。Venus では present の 40 ms 後、間隔は present の歩調と同じ。同じ swapchain・同じ queue の fence は present の順。T17 の「swapchain ごとの最古を any で」はそのまま使える。
- OUT_OF_DATE・SURFACE_LOST の present の fence は signal する（unplug で確かめた）。積まれなかった present（検証の失敗、OOM）の fence は変わらない → render が空きに戻す。device lost では signal されず、待ちが `VK_ERROR_DEVICE_LOST` で返る。
- `vkDestroySwapchainKHR` は、その chain の present の fence が全部 signal してから返る（destroy の step）。T17 が待っていても破棄は止まらない。
- fence の再利用は T17 の `vkResetFences`（renderer への往復が 1 回ある）。
- present に `VkSwapchainPresentModeInfoEXT` を付けるなら chain の mode だけ。
- cursor の thread の acquire は render の present では起きない（narrow）。overlay plane の採用と Wayland の release は今も全部の acquire を起こす。
