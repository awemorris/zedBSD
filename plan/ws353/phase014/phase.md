<!-- awesome-plan project=zedbsd record=ws353-p014 -->

# ws353-p014: refresh の事象の fence（kernel の push）、1 ms の見回りを libvulkan から消す

Status: cleared（2026-11-14 P2 q2064-i01、Q1 の確認待ち。branch `agent/p2-ws353b`、66b803a706・bc77e214ab。3 機種の build warning 0、QEMU（Venus）の refresh-fence 試験が 2 回 PASS。i915・nvrtx・venus は driver の push ではなく kernel の refresh watcher（下の「設計からの変更」）、i915 の vblank の割り込みの push は後の Phase か Bug）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §3.2・§4.4、対応表の 4-01。

## 目的

`vkRegisterDisplayEventEXT`（FIRST_PIXEL_OUT）の fence も kernel が refresh の境で signal する形にする（ユーザーの決定 2 の「同じ形の display の event」）。`vulkan_display_event_poll` と `VULKAN_SYNC_POLL_NS` を消す。

## 変える file

- kernel: `include/drivers/gpu/gpu.h` に `drv_gpu_display_refreshed(device, display_id, generation, sequence)` と display の op `refresh_arm(private, display_id, on)`（任意、登録がある間だけ vblank の割り込みを有効に）。**op は `struct drv_gpu_display_ops` の最後に足す**: venus（venus display.c:165〜172）・i915（display.c:181〜）などが位置で初期化している（レビュー M8）。display の op を持つ全部の初期化（i915 の display・bootfb・capture、venus、amdrdna、nvrtx、ghci の 2 つ、powervr、mali、videocore）を読み、足した op が NULL で埋まる事を確かめる。足す時に該当の struct を名前付きの初期化に直してよい（動作は変えない）。`gpu.c` の REFRESH の登録（p002 の ioctl の EOPNOTSUPP を外す）。`GPU_CAP_DISPLAY_CONTROL` の driver（i915 の display、amdrdna、nvrtx、venus、videocore の display と ghci の display は `GPU_CAP_DISPLAY_CONTROL` の有無を読んで、出す物だけ）の vblank の処理で push（レビュー L11）。
- libvulkan: `wsi-display-control.c` の `vkRegisterDisplayEventEXT` を REFRESH の fd に、`event_refresh_poll`・`event_refresh_base`・`vulkan_display_event_poll` を消す。`sync.c` の `VULKAN_SYNC_POLL_NS` と、事象の観測の枝を消す（`vulkan_sync_pause` の他の利用者を数え、残る物は理由を書く）。

## 残す振る舞い

- `GPU_DISPLAY_REFRESH`（同期の待ち）の ioctl は今のまま（使う所があれば）。登録の無い間は vblank の割り込みを増やさない。

## 確かめ

- build warning 0。QEMU（Venus）: 小さな program で refresh の fence を 100 回、間隔が refresh に合う、登録の無い間に driver の vblank の push が起きない（診断で数える）。実機（i915）は p016 で。

## 受け入れ

- 上の判定が PASS。libvulkan に周期の見回りが無い（grep）。結果・SHA・未実施を書く。

## 依存

- p003（p002 の登録の list と ioctl の上に足す）。

## 結果（2026-11-14 P2、q2064-i01、branch `agent/p2-ws353b`、基点 main d5dc642a48）

### commit

- 66b803a706: kernel（`include/uapi/gpu-display.h`、`include/drivers/gpu/gpu-display.h`・`gpu.h`、`src/drivers/gpu/gpu.c`、amdrdna の `amdrdna-private.h`・`display.c`・`present.c`、位置の初期化に NULL を足した ghci・i915（display・bootfb・capture）・nvrtx・venus）と libvulkan（`sync.c`・`sync-internal.h`・`wsi-display-control.c`・`wsi-display-nodes.c`・`wsi-internal.h`）。
- bc77e214ab: QEMU の試験（`plan/ws353/tests/refresh-fence-test.c`・`qemu-refresh-fence.sh`・`config-amd64-refresh.mk`）。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- uapi: `GPU_DISPLAY_FENCE` の REFRESH を受ける（EOPNOTSUPP を外した）。REFRESH は `GPU_CAP_DISPLAY_CONTROL` の node で display_id・generation を名指す（HOTPLUG は今どおり `GPU_CAP_DISPLAY_EVENTS`、名指し無し）。登録の時の refresh の数は driver の `refresh` op（cursor 0、timeout 0）で読み、ENOENT・ESTALE・ENXIO などはそのまま登録の失敗。output の generation が変われば ESTALE で signal。CANCEL は DISPLAY_EVENTS か DISPLAY_CONTROL の node。
- 枠組み: `unsigned drv_gpu_display_refreshed(device, display_id, generation, sequence)`（cursor より新しい境の登録を signal、generation の違う登録は ESTALE、残った登録の数を返す。IRQ でも呼べる: registry の lock と fence の lock だけ）。`struct drv_gpu_display_ops` の最後に任意の `int (*refresh_arm)(void *, uint32_t display_id, unsigned on)`（core は登録を ARMED にした後と CANCEL で出力の登録が 0 になった後に、device の `refresh_lock` の下で今の状態を読んで呼ぶ。push の返りが 0 なら driver は自分で止めてよい）。`gpu_ops_validate` は DISPLAY_CONTROL の無い device の refresh_arm を拒む。
- `drv_gpu_display_changed` は HOTPLUG の slot だけを signal する（REFRESH の slot を混ぜない）。slot に event・display_id・generation を足した。
- refresh watcher（`gpu.c`、refresh_arm の無い device）: 最初の REFRESH の登録で kthread を作り、unregister（monitor の後）で止めて reap する。ARMED の REFRESH の登録が無い間は `refresh_waitq` で timeout 無しに眠り、登録の ARMED で起きる。登録があれば 1 つの出力（前回と違う出力を優先、その出力・generation の最古の cursor）を driver の `refresh` op（session は NULL、timeout は 1 s）で待ち、境で `drv_gpu_display_refreshed`、ETIMEDOUT・EINTR は待ち直し、他の error はその出力・generation の登録を error で signal。cursor 0 は「今の数をすぐ返す」になるので 1 にして待つ。
- 診断の数: `gpu_refresh_pushes`（`drv_gpu_display_refreshed` の回数）・`gpu_refresh_waits`（watcher の refresh op の回数）。gpu.c の static、atomic で足すだけ、debugger で読む。
- amdrdna: `present_refresh_arm`（その display の `refresh_armed` と epoch を display の lock の下で立てる。firmware が display を持つ（frame の割り込みが無い）時は EOPNOTSUPP）。interrupt thread の frame start の処理で、armed なら数を `amdrdna_present_refreshed` → `drv_gpu_display_refreshed` で publish し、0 が返り epoch が変わっていなければ armed を下ろす。frame の割り込みは takeover から既に on で、増やしていない。
- libvulkan: `vkRegisterDisplayEventEXT` は display の snapshot で `GPU_DISPLAY_FENCE`（REFRESH、CLOEXEC）を display の node に出し、fd を `event_fences`（1 個）に持つ。ESTALE は snapshot を更新して 1 回やり直す。EOPNOTSUPP・ENOENT・ENXIO・ENODEV（と 2 回目の ESTALE）は登録の無い fence（何も signal しない。hotplug の node の無い renderer と同じ）、ENOSPC・ENOMEM・EMFILE・ENFILE は `VK_ERROR_OUT_OF_HOST_MEMORY`、他は `VK_ERROR_UNKNOWN`。観測・待ち・reset・destroy は hotplug の fence と同じ経路（`vulkan_display_event_signaled` は fd を timeout 0 で poll するだけ、待ちは fd と wake の token で眠る）。`vulkan_display_event_poll`・`event_refresh_poll`・`event_refresh_base`・`event_based`・`event_display`・`event_cursor`・`event_generation` を消した。cancel の関数は `vulkan_wsi_display_node_fence_cancel` に改名（hotplug と refresh の両方）。
- `sync.c`: `VULKAN_SYNC_POLL_NS` と refresh の枝（`refresh_pending`、1 ms の pause）を消した。any-wait の「pin の見つからない queue の marker」の 1 ms の pause もやめ、次の pass へすぐ進む（その marker は pin と観測の間に submit された物で、消費された record なら観測が完了を返すので、次の pass で pin できる。一時の競いで、続けて起きない）。`vulkan_sync_pause` は clamp を外し（秒を分けて nanosleep）、利用者は query.c の `vkGetQueryPoolResults` の WAIT_BIT だけ（renderer が query の可用を知らせないので 1 ms ごとに聞き直す。理由を comment に書いた）。

### 先に確かめた事

- display の op を持つ初期化: 位置の初期化は amdrdna・nvrtx・venus・ghci（2 つ）・i915（display・bootfb・capture）で、全部に最後の NULL（amdrdna は op）を足した（-Wextra の missing-field-initializers を避けるため。名前付きに直すのはしなかった）。powervr・mali は名前付きの初期化、videocore は calloc した struct への代入で、足した op は NULL のまま。
- `GPU_CAP_DISPLAY_CONTROL`（refresh op）を出すのは i915 の display・amdrdna・nvrtx・venus。videocore・ghci・powervr・mali・i915 の bootfb と capture は出さない → REFRESH の登録は EOPNOTSUPP、libvulkan の fence は何も signal しない（前の形でも `GPU_DISPLAY_REFRESH` が失敗して永久に未 signal だったのと同じ）。
- 4 つの refresh op は全部 session を読まない（venus は `(void)`、i915・amdrdna・nvrtx は UNUSED_PARAMETER）ので、watcher が NULL を渡せる。契約を gpu-display.h に書いた。

### 設計からの変更（理由）

1. **vblank の割り込みで push できる driver は amdrdna だけ**: 設計は「GPU_CAP_DISPLAY_CONTROL の driver の vblank の処理で push」。実態は、venus は仮想の時計（`sched_ticks` から数を出す、割り込みが無い）、i915 は resident run が pipe の vblank の割り込みを mask したまま PIPE_FRMCOUNT を 1 ms ごとに読む（control.c）、nvrtx は割り込みが無く frame counter を 1 ms ごとに読む。だから refresh_arm を持たない device は core の refresh watcher が driver の既存の refresh op で境を待って push する形にした（libvulkan から見れば kernel の push）。**登録がある間、i915 と nvrtx では kernel の中に 1 ms の frame counter の見回りが残る**（前は同じ 1 ms の見回りを libvulkan が ioctl で回していた。登録の無い間は watcher は眠り、見回りも無い）。venus の refresh op は次の境の tick まで眠るので見回りにならない（表示していない出力では 10 ms ごとに見直す）。
2. refresh_arm の off: core は CANCEL で出力の登録が 0 になった時だけ呼ぶ。push で signal されて 0 になった時は push の返り値（0）で driver が自分で止める。close・故障で消えた時は次の push の返りで止まる（1 境だけ余分）。on・off の競いは core の `refresh_lock`、driver の中の止めと on の競いは epoch で解く。
3. 登録の後の読み直し（HOTPLUG の「登録の後に番号を読み直して越えていればすぐ signal」）は REFRESH ではしない: watcher・push は ARMED の後にしか来ず、その前の境は cursor より新しい数で来るので次の境で signal する。
4. 出力の generation の変化は ESTALE で signal（libvulkan は fired として扱う）。前の libvulkan は「新しい generation で cursor を取り直し、次の本当の境で signal」だった。kernel は新しい generation を知らないので、client が display を列挙し直す合図として返す形にした。unplug でも同じ（試験の unplug の step）。
5. 試験の「間隔が refresh に合う」: Venus では 1 回の登録（`vkCreateFence`・`vkDestroyFence` の renderer の往復）が中央値 20 ms かかり、1 周期（13.3 ms）を越えるので、続けて登録すると間隔は 2 周期になる（1 回目の run の FAIL はこの判定の誤り: 中央値 26.9 ms＝2 周期）。判定を「各 fence は登録から 1 周期＋4 ms 以内に signal」「signal の間隔の 9 割が周期の整数倍 ±3 ms」に直した。

### 確かめ（実行したコマンドと結果）

- build（-Werror、warning 0）: `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-display.mk sysroot` と `build/x86_64/vmunix`（venus・i915・nvrtx・amdrdna・ghci）、`make -j4 BUILD_JOBS=4 build/x86_64/packages/libvulkan/.stamp`、`CONFIGMK=config/config.rpi4.mk` の sysroot・`build/rpi4/vmunix`（`arm64 vmunix check: PASS`）・libvulkan、`CONFIGMK=plan/ws353/tests/config-visionfive2-display.mk` の sysroot・`build/vf2/vmunix` と `CONFIGMK=config/config.vf2.mk` の libvulkan。全部 rc 0、log（`build/p14-*.log`）に warning 0。
- QEMU（Venus、max_outputs=2、`-display dbus,gl=on`、renderer は p2-ws353a の `build/ws035-sq-venus` を copy、runtime `build/ws353-p014-run`）: `sh plan/ws353/tests/qemu-refresh-fence.sh build/ws353-p014-qemu2`・`…-qemu3` の 2 回とも全 step PASS（`qemu-refresh-fence: PASS`）。判定は guest の `/tmp/p14/result.txt`（serial の command で読む）と、QEMU の gdbstub で読んだ kernel の数（`guest.py kgdb` で `gpu_refresh_pushes`・`gpu_refresh_waits`）。console・serial の log は読んでいない。3 回目の値:
  - idle0（登録無しの 20 s の中で 8 s 離して 2 回読む）: PASS、pushes・waits とも 0 → 0。
  - frames（100 個を順に登録し UINT64_MAX で待つ）: PASS、周期 13334 us（74994 mHz）、登録から signal まで中央値 7014 us・最大 13488 us、間隔 99/99 が周期の整数倍、blocking な poll 100 回（1 fence に 1 回）、登録の中央値 19863 us。
  - group（4 個の all-wait と any-wait）: PASS、all 3151 us、any 18 us、any の後に 4 個とも signal。
  - reset: PASS（signal → reset → 200 ms 後も NOT_READY）。cancel: PASS（20 個を境の前に destroy、その後の 1 個が signal）。noscan（何も表示していない display 1）: PASS（500 ms で VK_TIMEOUT）。
  - idle1: PASS、pushes 129・waits 130 のまま 8 s 動かない。idle0 から idle1 の間に push 129（≥100）。waits が 1 多いのは noscan の待ち（境が来ず 1 s で ETIMEDOUT）。
  - unplug（display 1 の fence を待つ間に host が head 1 を抜く）: PASS、marker から 2660 ms（host は marker を見て 2 s 後に抜く）で VK_SUCCESS（kernel の fence の status は読んでいない。watcher の refresh op が ESTALE か ENOENT を返して error で signal したと推すが未確認）。
  - 2 回目も同じ（frames: 中央値 6975 us・最大 14593 us、on-grid 99/99、polls 100、unplug 2570 ms）。1 回目（`build/ws353-p014-qemu`）は frames だけ FAIL（上の 5）。
- grep: libvulkan に `VULKAN_SYNC_POLL_NS`・`vulkan_display_event_poll`・`event_refresh_poll`・`GPU_DISPLAY_REFRESH` の利用は無い。fence の待ちの経路に周期の待ちは無い。libvulkan に残る周期の待ちは表示の事象の外の 3 つ: query.c の query の結果の 1 ms（上）、context.c:1124 の transport の trailer の 1 ms（`GPU_CAP_NOTIFICATION` の無い device の初期化だけ）、wsi-swapchain.c:5608 の cursor の busy の再試行（1 refresh、上限つき）。

### Q1 の問い（2026-11-14）への答え

1. **誰が refresh の fence を登録するか**: tree の中で `vkRegisterDisplayEventEXT`（と `GPU_DISPLAY_REFRESH`）を使うのは試験だけ（`userland/tests/display-events/main.c`・`userland/tests/display-control/main.c`、`plan/ws113/tests/host-display-events.c`）。compositor（`userland/desktop/wayland/`）・libkeiland・WSI は登録しない（grep で 0 件）。だから今、idle の compositor の下で watcher は眠ったままで、i915 の 1 ms の見回りも起きず、idle の電力に効かない。将来 compositor の歩調（design §4.3 の退けた案 (b)）や app が FIRST_PIXEL_OUT を待ち続けると、i915・nvrtx では登録の間 1 ms の見回りが残る。
2. **i915 の vblank の割り込みで push する本来の形に要る物**（この Phase では直していない）:
   - なぜ mask か: resident run は pipe A の vblank と underrun の割り込みを mask したまま走る前提で作られている。preflight（resident.c の `resident_preflight_panel`、`I915_RESIDENT_KEEP_MASKED_D12/D13`）は vblank か underrun が unmask の interrupt state を拒み、run の observer（run-log.c）は PIPE_STATUS の underrun を自分で読む（unmask すると IRQ handler が status を読み・消す）。observer が「well が on の間に vblank の unmask を見た」（`vblank_unmasked_seen`）を記録し、`resident_show_passed`・同 2547 行はそれを run の失敗とする。flip の時だけ xd-picture の更新が `vblank_get`・`vblank_sleep`（`drv_i915_wait_vblank` は pipe ごとに待ち手 1 つ）を使う。
   - 要る物: (a) IRQ handler の vblank の枝（interrupts.c の `i915_gen8_de_pipe_irq_handler`、`vbl->enabled` の時）から display へ届く hook（`struct i915_display_irq` に display への戻りが無い）、(b) refresh_arm で resident output の pipe に `drv_i915_display_vblank_get/put`、output の移動（`i915_display_move`）・pipe の停止・power well の off（pipe の close）をまたぐ armed の pipe の付け替え、(c) push の数は `i915_refresh_sample`（PIPE_FRMCOUNT の拡張、refresh.lock）を IRQ から呼んで作る、(d) observer と run の合否の規則を「refresh の登録がある間の vblank の unmask」を許す形に直す（underrun の status の持ち主を決め直す）、(e) vblank の割り込みが有効な間の DC state・PSR の深さへの影響を 5330 で測る。全部 5330 の実機でしか確かめられない。

### 未実施

- i915・amdrdna・nvrtx の実機（p016 で 5330。amdrdna の push は build だけ）。
- 既存の libvulkan の回帰（vkcube・compositor の起動）、`userland/tests/display-events` の probe の流し直し（Q1 の割当が無い）。
- arm64・riscv64 は build だけ。docs/reference の gpu-display・gpu-requests に REFRESH の登録と `drv_gpu_display_refreshed`・refresh_arm を書いていない（この Phase の範囲の外）。
- 掃除を頼む物（Q1）: なし（scratch は自分の build/ の中だけ: `build/ws353-p014-*`、`build/p14-*.log`）。
