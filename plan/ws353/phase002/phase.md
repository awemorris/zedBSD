<!-- awesome-plan project=zedbsd record=ws353-p002 -->

# ws353-p002: kernel の display の事象の fence（HOTPLUG）

Status: uncleared（2026-11-14 00:46 P1、q2057-i01。kernel の実装・3 機種の build・i915 の host 試験・QEMU の 7 step 中 6 は PASS。QEMU の latency の step だけ FAIL: venus の driver が idle の間の head の変化を見ない既存の事象の取りこぼしで、fence の側ではない。下の「結果」）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.1・§3。

## 目的

display node の topology の変化で、登録された fence を kernel が signal する口を作る。libvulkan が問い合わせで判定する形（ユーザーの決定 2 でやめる形）の土台を置き換える。

## 変える file

- `include/uapi/gpu-display.h`: ioctl `GPU_DISPLAY_FENCE`（`_IOWR`、`struct gpu_display_fence`）と `GPU_DISPLAY_FENCE_CANCEL`（`_IOW`、`struct gpu_display_fence_cancel`）、事象の `GPU_DISPLAY_FENCE_HOTPLUG`・`GPU_DISPLAY_FENCE_REFRESH`（REFRESH は p014 まで EOPNOTSUPP）。既にある `GPU_DISPLAY_EVENT_CHANGE`・`ACK`（events の flag）と名前を混ぜない。番号は 'G' の 51・52 を候補に、tree の全部の `_IO*('G', n)` を grep して空きを決める（設計 §3.1、レビュー L1）。
- `include/drivers/gpu/gpu.h`（driver と枠組みの口、HAL ではない）: `drv_gpu_display_changed(device, sequence)`。
- `src/drivers/gpu/gpu.c`: ioctl（`GPU_CAP_DISPLAY_EVENTS`・読みの権限、`GPU_CAP_FENCE` は要らない）、open ごとの登録の list（上限 16）、登録の後の読み直し、changed での signal と外し、CANCEL、close・offline・故障での片付け（故障は error で signal）。`gpu-fence.c` の口を使う: 登録で `drv_gpu_fence_create(display node の識別子, 0, &handle)` と `drv_gpu_fence_bind(handle, 1, registration)`（bind が無いと signal は EPERM、gpu-fence.c:441〜444）、push で `drv_gpu_fence_signal(handle, 1, registration, 0)`、offline・故障で status `ENODEV`、外す時に handle の参照を放す（設計 §3.1、レビュー M1）。
- `src/drivers/gpu/gpu-fence.c`: WS352 の branch の poll.c（名指しの poll）が main にあれば、`fence_poll` に `poll_wait_object(fence)`、signal・reset に `poll_notify_object(fence)`（名指しが無いと、fence の fd を待つ thread が GPU の job の完了のたびに起きる。設計 §3.1、レビュー M2）。branch が無ければ今のまま。
- driver（`events` の実装 8 つ）: i915（`display/hpd.c` の `drv_i915_hpd_topology_touch` 680〜700 と publish 1215〜1235 の 2 か所だけ。`drv_gpu_register` は i915.c:211 の 1 回で、bootfb・capture は同じ device の op の変種。`drv_gpu_device` は `display->device->gpu`、display/internal.h:1168・i915.h:128。レビュー M9）、venus（`display.c`）、amdrdna（`present.c`）、nvrtx（`display/present.c`）、ghci（`display.c`）、videocore（`display/display-device.c`）。番号を上げた後の `poll_notify()` を `drv_gpu_display_changed()` に置き換える（中で `poll_notify()` を呼ぶので POLLPRI の待ち手は今のまま）。

## 残す振る舞い

- `GPU_DISPLAY_EVENTS` の QUERY・ACK と POLLPRI（swapchain の acquire の待ちが使う）は変えない。
- HAL（`include/hal/hal.h`）・Makefile・mk/ は変えない。

## 先に確かめる事

- 8 つの `events` の実装で、topology の番号が device（node）ごとに一つで open によらない事。違う driver があれば止まって Q1 に報告（設計の契約が崩れる）。
- `drv_gpu_display_changed` を呼ぶ文脈（spinlock を持たない、IRQ の外）が今の `poll_notify()` の場所で成り立つ事。

## 確かめ

- build: x86_64・arm64・riscv64 の kernel（変えた driver を含む config）warning 0。
- QEMU（Venus、P1 の名前・port）: `plan/ws353/tests/` に guest の小さな program（ioctl で HOTPLUG の fence を作り、fd を poll(-1)）と 1 本の script（配置 → 起動 → program → `plan/tools/guest/venus-head.sh` で head を抜き差し → 回収 → 要約）。判定: 抜き差しから 1 s 以内に POLLIN、抜き差しが無い 30 s は起きない（WS352 の名指しの poll が main に無い時は「GPU の job が無い 30 s」に限る）、登録の直前の変化（cursor の競い）は登録の直後に POLLIN、CANCEL の後は起きない、open の close で登録が残らない（同じ open で 16 を越えると ENOSPC、close して作り直せる）。
- 判定は guest の file と SSH・QMP（console・serial の log を読まない）。

## 受け入れ

- 上の build と QEMU の判定が PASS。i915 などの実機は未実施でよい（p016 で 5330）。
- 結果・SHA・未実施をこの phase.md に書く。

## 依存

- p001（設計）。

## 結果（2026-11-14 00:46 P1、q2057-i01、branch `agent/p1-ws353b`、基点 main c88842a749）

### commit

- 20a75bb908: kernel（uapi・枠組み・gpu.c・5 つの driver）と build の config 2 つ。
- 9d589fa150: QEMU の試験（`plan/ws353/tests/config-amd64-fence.mk`・`display-fence-test.c`・`qemu-display-fence.sh`）。
- この記録の commit（phase.md・ws.md・config に grep を足した物）は報告に書く。

### 実装

- uapi（`include/uapi/gpu-display.h`）: `GPU_DISPLAY_FENCE`＝`_IOWR('G', 54, struct gpu_display_fence)`、`GPU_DISPLAY_FENCE_CANCEL`＝`_IOW('G', 55, struct gpu_display_fence_cancel)`、`GPU_DISPLAY_FENCE_HOTPLUG`（1）・`GPU_DISPLAY_FENCE_REFRESH`（2、EOPNOTSUPP）・`GPU_DISPLAY_FENCE_MAX`（16）。struct は設計 §3.1 のまま。
- 枠組み（`include/drivers/gpu/gpu.h`）: `drv_gpu_display_changed(device, sequence)`。`gpu-display.h` の events の op の説明を「番号は device ごとに一つ、変化は `drv_gpu_display_changed` で知らせる」に直した。
- `gpu.c`: open ごとの登録の slot 16（FREE→RESERVED→ARMED→SIGNALED）。登録は events の snapshot で cursor を読み、`drv_gpu_fence_create(identity, 0)`・`drv_gpu_fence_bind(handle, 1, slot)`、slot が参照を一つ持って ARMED、snapshot を読み直して越えていればその場で signal（読み直しが失敗すればその errno で signal）、最後に fd を出す。`drv_gpu_display_changed` は registry の lock の下で全 open の ARMED の slot を `sequence > cursor` で signal（`drv_gpu_fence_signal_deferred(handle, 1, slot, 0)`）、unlock の後に `poll_notify()`（POLLPRI の待ち手は今のまま）。CANCEL は slot を探し、ARMED なら `drv_gpu_fence_unbind` して外す。`gpu_session_fail_locked`（device の故障・offline の report_error、session の故障、final close）で ARMED を ENODEV で signal。SIGNALED の slot の参照は thread の文脈（次の登録の reserve・CANCEL・`gpu_session_drop`）で放す（signal は venus の IRQ からも来るので、そこで最後の参照を落とさない）。`GPU_CAP_FENCE` は要らない。
- driver: venus（`transport.c`: IRQ と query の経路。queue の lock の下で `transport->gpu` を retain して渡す）、i915（`display.c` に `drv_i915_display_changed`: 登録した op の events で番号を読んで渡す。`hpd.c` の 2 か所と `xd9-node.c` の bump）、amdrdna（`node.c` に `amdrdna_node_display_changed`、`display.c` の 2 か所）、ghci（`display-scan.c` の service の終わり、変化の round だけ）。nvrtx と videocore の events は番号 1 の固定で push が無いので変えていない。
- `gpu-fence.c`: WS352 の branch（82d2a01458）が main に無いので変えていない（設計 §3.1 のとおり）。

### 設計からの変更（理由）

1. ioctl の番号は 54・55: 'G' の 51〜53 は [WS306 の設計](../../ws306/design.md) が `GPU_GAME_INFO`・`OPTION`・`SET` に取っている（tree の grep。47・48 は空きだが過去の利用が分からないので避けた）。
2. i915 の push は hpd.c の 2 か所に加え `xd9-node.c` の `i915_xd9_node_bump`: bootfb の events は外部の display（xd9）が動いている間は xd9 の番号を返す（bootfb.c:1150〜）。番号の出所が 2 つあるので、driver は「登録した op の events が今返す番号」を渡す形にした（レビュー M9 の「hpd.c だけ」を直す）。
3. 先に確かめる事の 2 つ目（呼ぶ文脈は IRQ の外）は venus で成り立たない: `drv_venus_transport_display_changed` は IRQ（transport.c の `venus_interrupt`）からも呼ばれる。だから `drv_gpu_display_changed` は IRQ でも呼べる形（registry の lock と fence の IRQ-safe の lock だけ、`kern_free` をしない）にし、signal 済みの参照は thread で放す。venus は wrapper を retain して渡す（今の fault の snapshot と同じ形）。
4. close でも登録を ENODEV で signal する（設計は「外す」だけ）: bind した owner が残ると fence を reset できず、別に fd を持つ待ち手が永久に起きないため。uapi の説明に書いた。
5. 番号が device ごとに一つの確かめ: 8 つの events の実装は全部 session の引数を読まない（venus は transport、i915 は hpd・xd9 の大域、amdrdna・ghci は device、nvrtx・videocore は 1 の定数）。契約は崩れない。

### 確かめ（実行したコマンドと結果）

- build（-Werror、warning 0）: `make CONFIGMK=plan/ws353/tests/config-amd64-display.mk build/x86_64/vmunix`（venus・i915・nvrtx・amdrdna・ghci）、`make CONFIGMK=config/config.rpi4.mk build/rpi4/vmunix`（arm64、videocore。`arm64 vmunix check: PASS`）、`make CONFIGMK=plan/ws353/tests/config-visionfive2-display.mk build/vf2/vmunix`（riscv64、powervr と ghci）。どれも rc 0、log に warning 0。
- i915 の display の host 試験: `taskset -c 0-3 sh plan/tools/i915-display/run-xd-host-tests.sh build/ws353-p002-i915host` → 31 本 0 failures、check-display-steps・check-sources PASS、`run-xd-host-tests: all passed`。
- QEMU（Venus、max_outputs=2、`-display dbus,gl=on`、host の virgl_render_server、P1 の runtime `build/ws353-p002-run`）: `sh plan/ws353/tests/qemu-display-fence.sh build/ws353-p002-qemu` を 2 回。guest の操作は serial（`plan/tools/guest/serial.py`）、判定は guest の結果の file（`/tmp/<step>.out`）を command で読む。2 回とも同じ結果:
  - idle（登録して 30 s、変化なし）: PASS（番号 4→4、起きない）。WS352 の名指しの poll が main に無いので「GPU の job が無い 30 s」に当たる。
  - wait（挿す）: PASS（marker から 254 ms・227 ms、番号 6→7）。wait（抜く）: PASS（238 ms、9→10）。marker からの時間は host が marker を見て D-Bus の SetUIInfo を送るまでを含む。
  - latency（marker を見てから 2 s 待って挿す）: FAIL。20 s の間 guest の番号（`GPU_DISPLAY_EVENTS`）自体が 12 のまま動かなかった。fence の取りこぼしではなく、venus の driver が変化を見ていない（次の節）。
  - cancel: PASS（CANCEL の後、抜いても 8 s 起きない）。
  - limit: PASS（16 で ENOSPC、CANCEL で空く、close で全部が POLLIN|POLLERR（ENODEV）、reopen で 16）。
  - race（600 登録を 5 ms おき、16 ごとに確かめて CANCEL、head を 8 回抜き差し）: PASS（missed 0、spurious 0）。ただし登録の後に番号が動いた登録は 1 回目 128、2 回目 0（2 回目は抜き差しが guest に見えなかった。次の節と同じ原因）。

### 見つけた事（p002 の範囲の外、Q1 へ）

- **venus の display の変化が idle の間に届かない（既存）**: head の抜き差しが、guest で GPU の仕事（venus の queue の割り込み）が起きた時にだけ番号に出る。wait の step は program の開始の直後で通り、2 s 後の latency は 20 s 見えない（2 回とも同じ）。venus の IRQ の経路は MSI-X の時 config の `events_read` を読むが clear しない（clear は display の query、display.c:1502 だけ）ので、一度立つと以後の queue の割り込みのたびに番号が上がる（番号が抜き差し 1 回に 2〜3 上がる）。config の変化の割り込み自体が来ていない疑い（未確認: QEMU の側か driver の側か）。POLLPRI の今の経路も同じ影響を受ける。Bug の ticket と venus の driver の直しを推す（直るまで QEMU の「1 s 以内」の判定は venus で取れない）。
- 新しい build の試験の道具の食い違い: (1) x86_64 の sysroot は BUILD に依らず `build/x86_64/sysroot`（mk/platform-x86_64/sysroot.mk）で、BUILD を分けた image の build は package の header が見つからず失敗する。(2) `vmunix userland` を一つの make で -j に並べると sysroot の前に package が走る。(3) sysroot の target が自分の build/NoctLang を cmake --parallel 16 で作る（4 thread の上限を超える。Makefile の規則）。(4) `plan/tools/guest/test-image.sh` と `guest.py extra-files` は古い変数（ZEDBSD_CONFIG・ZEDBSD_TEST_EXTRA_FILES・disk-image）。(5) rootfs は `--file` の後に `$(BUILD)/etc` を写すので、harness の net.conf は init の INIT_DATA を test の config で override して入れた。(6) この最小の image では `net startup` の UP が「No such file or directory」で失敗して SSH が使えない（原因は未調査）ので serial に切り替えた。(7) 手で link した static の program は stdout が file の時に exit で flush されない（test の program は行 buffer と fflush で避けた。libc の crt0 か exit の経路の疑い、未調査）。
- `docs/reference/gpu-requests.md`・`gpu-display.md` に新しい ioctl を書いていない（この Phase の範囲の外）。`plan/tools/docs-check/check-gpu-uapi-doc.py` は main でも '0660' の解析で落ちる（既存）。

### 未実施

- QEMU の「抜き差しから 1 s 以内」の確かめ（latency の step）: 上の venus の取りこぼしで判定できない。
- i915・amdrdna・ghci・nvrtx・videocore の実機（p016 で 5330）。WS352 の名指しの poll（merge の後に fence_poll の `poll_wait_object` を足す）。

### p003 への申し送り

- 登録の fence の fd は gpu-fence.c の payload（generation 1）。poll の POLLIN で signal、POLLERR は ENODEV（offline・故障・close）か読み直しの失敗の errno。signal の後の CANCEL は 0 か ENOENT（kernel が次の登録で slot を回収した後）を返すので、libvulkan は ENOENT を無視してよい。
- 登録は open ごとに 16。libvulkan の display node の open を process で共有するなら、全 VkDevice の hotplug の fence の数の合計になる。
- venus の取りこぼし（上）が直るまで、p003 の QEMU の判定（1 s 以内）も同じ影響を受ける。
