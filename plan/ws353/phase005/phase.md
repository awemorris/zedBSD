<!-- awesome-plan project=zedbsd record=ws353-p005 -->

# ws353-p005: timer の thread と、thread の結果の flag を wake に（main loop の 1）

Status: cleared（2026-11-14 P1、q2061-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の main-wake 試験 PASS、p002・p003 の QEMU の試験の流し直し PASS（下の「結果」）。2-03 は残した（理由は下、Q1 の判断 (B)）。Q1 の確認待ち）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.2・§5.5・§5.11、対応表の 2-02・2-03・2-07〜2-12・2-14。

## 目的

main の締め切りを持つ timer の thread（T16、未決 1 の推し C）と API を作り、main が周期で覗いている「thread の結果の flag」（名残り）を wake に変える。入力の scan を事象だけにする。

## 前提

- WS352 p002 の merge の後を推す（設計 §7）。merge しない時は、main の固定 10 ms の pass はこの Phase の後も p008 まで残る。どちらでも、main の poll の timeout を -1 にするのは p008 の終わり（全部の期限が timer に移った後）。

## 変える file

- `userland/desktop/wayland/threads.c`・`threads.h`: `struct kwl_timer`、min-heap（main だけが書く）、`kwl_timer_arm`・`kwl_timer_disarm`、timer の thread（次の期限まで `pthread_cond_timedwait`、無ければ `pthread_cond_wait`、発火で出口と wake）、main の take（期限の過ぎた物の handler）。`KWL THREADS` に `timer`。
- `main.c`: 起動と終了の順（timer は lane の後に始め、終了は lane の前に止める）、wake の後の take に timer。SIGCHLD の handler が wake pipe に 1 byte（branch で済なら確かめだけ）。去った client の buffer の解放（`kwl_retire_ready`、objects.c:742〜781）: 時刻の条件（`retire_next_ms`・入力の静けさ・starve）は返し待ちの list が空でない間だけ timer に arm、状態の条件（`in_flight`・`dirty`）は COMPLETED（p011 の後は REAPED）と描き終わりで見直す。自分の wake pipe に書くのは「今 `kwl_retire_ready` が 1 で、1 pass の数を越えて残りがある」時だけ（残りがあるだけで書くと poll(-1) が直ちに返り続け idle の CPU が埋まる。設計 §5.2、レビュー S3）。timeout 0 にしない。`frame_fd`（main.c:91・994・1076、kwl.h:1565、-1 のまま使われない）を消す（レビュー L5）。
- 名残りの 7 つ: `cursor-plane.c`（手放しで cursor の thread が wake）、`glass.c`（loader の終わりで wake）、`shot.c`（送りの終わりで wake）、`system.c`（562: lane の終わりで続ける、2613: sessiond の答えで問い直す）、`media-library.c`（pipe を poll、子の終わりは SIGCHLD の wake）、`machine-shell.c`（branch で済なら確かめだけ）。
- 入力の scan（main.c の `KWL_INPUT_SCAN_MS`、zedBSD）: /dev/system の INPUT の事象でだけ scan（Linux・FreeBSD の backend は今のまま）。`src/drivers/generic/input.c` で INPUT の事象を evdev の node が見えた後に出しているかを確かめ、前なら順を直して `KWL_INPUT_SETTLE_SCAN_MS` を消す（直せない理由があれば止まって Q1 に報告）。

## 残す振る舞い

- 各機能の結果と順（WS309 §3.3 の take の順）。Linux・FreeBSD の backend の scan。

## 確かめ

- build: compositor（x86_64・arm64・riscv64、keiland-linux は崩れてよい）warning 0。
- host: timer の heap と thread の試験（arm・disarm・同じ時刻・先頭の差し替え・停止、TSan）を `plan/ws353/tests/timer/` に（host の cc で build、Makefile を使わない）。去った client の解放の歩調（list が残り `kwl_retire_ready` が 0 の間に main が起きない、時刻の条件が満ちた時に timer で 1 回起きる）。
- QEMU: 壁紙の変更・shot・media の helper・USB の keyboard の抜き差し（BUG-264 の `hotplug-guest.sh` の形、10 tick 以内に取る）が前と同じに動く。WS352 の poll の診断で、これらの 10 ms の起床が無い。

## 受け入れ

- 上の判定が PASS。対応表の 2-02・2-03・2-07〜2-11・2-14 が消えている、2-12 の歩調が timer と事象になっている（serial は p010）（2-03 は kernel の順を直せた時。直せない時は理由を書いて Q1 に）。結果・SHA・未実施を書く。

## 依存

- p004。WS352 p002 の merge（推し）。

## 結果（2026-11-14 P1、q2061-i01、branch `agent/p1-ws353e`、基点 main 9a9598bcc3）

### commit

- 5569485ff0: WS352 の branch の先端 82d2a01458 の上で b05187933a を `git revert -n` した物（その tree に b05187933a があるので、逆を当てても libvulkan の今の形と混ざらない）。
- 3e9c426f74: その merge（親 9a9598bcc3・5569485ff0、`git log -1 --format='%h %p'` で 2 つを確かめた）。
- 56a43ffd37: kernel の GPU の fence の poll を名指しに（設計 §3.1、レビュー M2）。
- 386b86fa94: compositor・backend の p005 と host 試験（`plan/ws353/tests/timer/`）。
- c3fc188117: 書き方の直し、QEMU の試験（`plan/ws353/tests/qemu-main-wake.sh`・`config-amd64-mainwake.mk`・`perf-rate.sh`）、design.md の 2-03 の記録。
- この記録（phase.md・ws.md）の commit は報告に書く。

### WS352 の取り込み

- 方法: WS352 の branch を b05187933a（libvulkan を POLLPRI で block する作りかけ、採らない）を除いて merge した。branch の先端を detached で取り出して b05187933a を revert（commit 5569485ff0）し、それを自分の branch に merge した（Q1 の指示の「merge の後に revert」と同じ結果。b05187933a がある tree に逆を当てたので、p003 で変わった libvulkan に逆を当てる誤りは無い）。
- 衝突: `userland/desktop/wayland/output-switch.c` だけ（WS352 が `kwl_due` を足した旧 `kwl_output_tick`。p004 で消した物）。今の main の形を正として解き、main と同じにした。
- 確かめ: `git diff main -- userland/desktop/libvulkan` は空（sync-internal.h・sync.c・wsi-display-nodes.c を含め main と同じ）、output-switch.c も main と同じ。x86_64 の sysroot・`vmunix`・wayland の package は warning 0（merge の直後）。
- WS352 の kernel の poll（object ごとの waiter）と今の kernel（p002 の display の fence、BUG-431）: p002 の phase.md の未実施にあった「merge の後に fence の poll を名指しに」を 56a43ffd37 で入れた（`fence_poll` で `poll_wait_object(payload)`、`drv_gpu_fence_reset`・`drv_gpu_fence_signal` で `poll_notify_object(payload)`。descriptor と import の alias は同じ payload から答えるので取りこぼさない。GPU の job の完了などの名指し無しの `poll_notify()` は WS352 の判断どおり全員を起こすまま）。p002 の `qemu-display-fence.sh` と p003 の `qemu-hotplug-wait.sh` をこの tree で流し直し、どちらも全 step PASS（下）。

### 実装

- timer（`threads.c`・`threads.h`）: `struct kwl_timer`（期限・heap の中の場所・発火した take の番号・名前・handler）、`struct kwl_timers`（main だけが触る min-heap と、lock の下で timer の thread と分ける「次の期限」・発火の印・停止）。API は `kwl_timer_init`・`kwl_timer_arm`（arm 済みなら動かす、heap が満ちたら ENOSPC）・`kwl_timer_disarm`・`kwl_timers_take`（期限の過ぎた物を早い順に handler へ。1 回の take で同じ timer は 1 回だけ、handler が過去に arm し直せば次の take）・`kwl_timers_next`・`kwl_timers_start`・`kwl_timers_stop`（`KWL TIMER stopped wakes=N armed=M`）・`kwl_timers_destroy`。thread（`KWL THREADS` の名前 `timer`）は期限まで `pthread_cond_timedwait`（CLOCK_MONOTONIC）、無ければ `pthread_cond_wait`。期限で「次の期限」を無しにして、take の前の最初の発火だけ wake pipe に 1 byte。take が次の期限を渡す。heap の大きさは 128（timer は機能ごとの record なので数は決まっている）。
- main（`main.c`）: timer は wake pipe の直後に init（`kwl_retire_init` も）、thread は READY の後に他の thread の後で start、event loop の後に他の thread の前で stop、cleanup の後に destroy。pass の中の take は全 thread の出口の後・client の要求の前。thread が無い時（作れない時）は heap の先頭を loop の期限（`kwl_due`）に畳む。`frame_fd` と `kwl_frame_done`（使われていない）を消した。
- 去った client の buffer の解放の歩調（`objects.c`）: `kwl_retire_due`（状態が変わらなければ解放してよくなる最も早い時刻。list が空・frame が飛んでいる間は無し）、`kwl_retire_pace`（pass の終わりに: 今解放してよいなら自分の wake pipe に 1 byte と disarm、時刻を待つなら timer に arm、それ以外は disarm）。timer の handler は何もしない（起きた pass の終わりで解放する）。main の pass の前の `kwl_due_soon`（list がある間の 10 ms）と `kwl_due(0)` を消した。状態の条件（`in_flight`・`dirty`）は COMPLETED と描き終わりの pass の終わりの `kwl_retire_pace` で見直す（p011 の後は REAPED）。
- 名残り:
  - cursor（`cursor-plane.c`・`cursor-thread.c`・`cursor-box.h`）: 2 つの写しが両方使用中で写せない時、main が box の lock の下で `copy_wanted` を立てる。cursor の thread は写しの最後の pin を外した時に `copy_wanted` を見て下ろし、main を wake。`kwl_due_soon` を消した。
  - glass（`glass.c`）: loader の thread が終わりに wake pipe に 1 byte。`kwl_due_soon` を消した。
  - shot（`shot.c`）: 送りの thread が終わりに wake pipe に 1 byte。送りの間の `kwl_due_soon` を消した。
  - system.c:562（鍵の保存の段が system bar の要求に止められた時の毎 pass の再送）: network の daemon の答え（DONE）を `kwl_network_tick` で扱った後に `kwl_system_network_free` が再送する。`kwl_system_tick` の再送と `kwl_due_soon` を消した。
  - system.c:2613（sessiond の要求が他で埋まっている時の ENROLLED の問い）: sessiond の答え（`kwl_handoff_answer`）の後に `kwl_system_session_free` が問い直す（`handoff.c` は答えの振り分けを static の `handoff_answer_route` に分けた）。`kwl_due_soon` を消した。
  - media（`media-library.c`、backend の `media/media.c`・`keiland-backend.h`）: backend に `kl_backend_media_poll`（動いている helper の pipe と、最も早い時間切れまでの秒）を足し、compositor は pipe を `kwl_watch` で poll に、時間切れを timer に（helper の終わりは SIGCHLD の wake）。`kwl_due_soon` を消した。一度に watch する pipe は 4 まで（越えたら loop の期限で戻る）。
  - machine-shell.c: WS352 の branch で thread が wake を書く形になっていた（machine-shell.c:406）。確かめだけ。SIGCHLD の handler の wake も branch で済（main.c の `child_ended`）。
- 入力の scan（`main.c` の `input_scan_pace`、`input.c`、`backend-host.c`、`kwl.h`）: backend に `kl_backend_input_events`（zedBSD は /dev/system の事象を聞いている間 1、Linux・FreeBSD は 0）を足した。zedBSD は scan を頼まれた時（INPUT の事象、resume、finding の途中に来た scan）と事象の後 2 s の 100 ms だけ、Linux・FreeBSD と事象の無い時は今のまま 2 s ごと。io の thread の finding の途中に来た scan（EBUSY）は `input_scan_time = 0` で頼まれたままにし、finding の終わり（lane の wake）の pass で scan する（前は 2 s の scan が拾っていた）。

### 設計からの変更（理由）

1. 2-03（事象の後 2 s の 100 ms の scan）は消さずに残した。kernel の順は既に正しい（`src/drivers/generic/input.c` は `cdev_register_managed` の後に `post_device_event(ADD)`、362〜376 行）。100 ms の理由は、sessiond が同じ ADD の事象で node を seat の利用者へ chown する前は compositor が開けない事（BUG-264、devfs の既定は root の 0640）で、kernel の順では直らない。Q1 に報告し、(B)（p005 は 2 s の周期だけ消し、100 ms は事象の後だけの物として残す）で進めた。design.md の §5.11 と対応表 2-03 に記録した。
2. timer の API は `struct kwl_timers *` を取る（設計は `server`）。threads.c を compositor の他から切り離して host で試験するため。呼び手は `&server->timers` を渡す。
3. 時刻だけを待つ物（去った client の解放、media の時間切れ）の timer の handler は何もせず、起きた pass の中で今の処理（pass の終わりの解放、pass の始めの `kwl_system_tick` の media の update）が走る。handler に処理を移すのは p006・p007 の各機能の移しと同じ形で行える。
4. system.c の 2 件の続けは「lane の終わり」ではなく、それぞれ network の daemon の答え（DONE）と sessiond の答えにした（EBUSY の原因が、それぞれ daemon と sessiond が一度に一つの要求しか取らない事なので、その答えが空きの事象）。
5. QEMU の「10 ms の起床が無い」の判定は WS352 の poll の診断の kernel（試験だけの patch）ではなく、compositor の `KWL PERF` の窓（pass と timeout の数）で行った。10 ms の歩調は 1 秒に約 100 の pass と timeout になる。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: `make -j4 BUILD_JOBS=4 build/x86_64/packages/wayland/.stamp`、`make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk build/rpi4/packages/wayland/.stamp`（arm64）、`... CONFIGMK=config/config.vf2.mk build/vf2/packages/wayland/.stamp`（riscv64）、kernel は `build/x86_64/vmunix`・`build/rpi4/vmunix`・`build/vf2/vmunix`。最後の source（c3fc188117）で: arm64・riscv64 は wayland の package（threads.c を compile し直した事を log で確かめた）と vmunix（gpu-fence.c を compile）が rc 0・warning 0。x86_64 は vmunix（56a43ffd37 の後）が rc 0・warning 0、wayland の package は QEMU の image の build（capture 付き、最後の source を compile）で warning 0（その後の `make build/x86_64/packages/wayland/.stamp` は compile する物が無く rc 0）。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file に流し、この Phase で増えた指摘が 0 の事を基点（3e9c426f74）と比べて確かめた（残りは前からの物）。`git diff --check` は空。
- host 試験 `sh plan/ws353/tests/timer/run.sh`（host の cc、Makefile を使わない、gnu89、plain・ASan/UBSan・TSan）: PASS。
  - timer-test.c: 100 個の順の乱れた arm・動かし・disarm の後に時刻の順で発火、同じ時刻の 2 つ、先頭の差し替え（thread に渡す期限が変わる）、先頭の disarm、遅らせた timer は新しい時刻まで発火しない、handler が過去に arm し直しても 1 回の take で 1 回、heap が満ちたら ENOSPC。thread: 何も arm していない 300 ms は byte 0、遠い期限の後に近い期限で差し替えて近い期限で 1 byte（期限より前に起きない）、take の前は byte が増えない、2 つの期限が take の前に過ぎても 1 byte で 1 回の take が 2 つ発火、期限の前に disarm で byte 0、stop で join（wakes=2）。
  - retire-test.c（objects.c と threads.c、残りは stub）: list が空なら arm も wake も無い。3 つが待ち入力の直後なら timer は静けさの終わり（200 ms）に arm、その前の 120 ms は wake 0、終わりに timer で 1 回。その pass で 1 つ解放し、次も今すぐ出来るので自分を 1 回だけ起こす。frame が飛んでいる間は 1.3 s（list の上限の 1 s を越えて）wake 0、完了の pass で解放して 1 回。描く物があり入力の直後なら list の上限（1 s）に arm、その前の 700 ms は wake 0、上限で 1 回。空になれば disarm、wake 0。loop の期限への fallback は 0 回。
  - 変異: `kwl_retire_pace` を「list があれば自分を起こす」（レビュー S3 の busy loop の形）にすると retire-test は 11 件 FAIL（試験が busy loop を見分ける事を確かめた。source は戻した = 変異は build/tmp の写しにだけ当てた）。
- QEMU（Venus、max_outputs=1、`-display dbus,gl=on`、runtime `build/ws353-p005-run`）: `sh plan/ws353/tests/qemu-main-wake.sh build/ws353-p005-qemu`（image の build から。userland と disk の warning は noct の外で 0）。compositor は serial から `--testing --glass` で起動、log は guest の `/tmp/p5/kwl.log`、判定は serial の `grep -c` の数と status と guest の awk の集計（console の log で判定しない）。
  - 1 回目: compositor の package の stamp が `ZEDBSD_TEST_SCREEN_CAPTURE` の切り替えを追わず、capture の無い compositor が image に入り start が FAIL（`KWL SHOT listening` 無し）。止めて、script が build の前に `shot.c` を touch するように直した（中身は変えない）。
  - 2 回目（`build/ws353-p005-qemu`）: `--glass` 無しで壁紙が出ない（glass の look が無いと `kwl_glass_wallpaper_begin` は ENODEV）、idle の判定が厳しすぎた（下の残りの 1 秒の期限）。start・shot・plug・unplug・stop は PASS。
  - 3 回目（`IMAGE=build/x86_64/hdd-image.img sh plan/ws353/tests/qemu-main-wake.sh build/ws353-p005-qemu2`、2 回目と同じ image、script の `--glass` と判定を直した）: 全 step PASS。
    - start: window mode、`KWL THREADS count=6 names=io-file,io-device,present,display,hotplug,timer`、capture が listening。
    - idle-30: `ms=30041 passes=63 timeouts=31`（約 2 pass/s・1 timeout/s。10 ms の歩調なら約 100/s）。
    - 壁紙: `keiland-settings set wallpaper /root/wall-1.png`・`wall-2.png` で `KWL GLASS wallpaper path=... ms=51`・`ms=87`、頼んでから行が見えるまで 272・243 ms（serial の確かめの粒度を含む上限）。
    - shot: `keiland-shot` を 2 つ同時に。1 つ目の送りの終わりの wake で 2 つ目が受け付けられ、両方 `KEILAND-SHOT` で書かれた（`KWL SHOT sent` 2 回）。
    - idle-after-20: `ms=20123 passes=69 timeouts=23`。
    - plug: QMP `device_add usb-kbd` から `KWL INPUT device=/dev/input/event3 kind=keyboard` まで 200 ms 以内（`KWL EVENT input changed` の事象の scan で取った。root の compositor なので sessiond の chown を待たない）。unplug: `device_del` で `KWL INPUT_CLOSED`。
    - idle-after-plug-20: `ms=20024 passes=40 timeouts=20`。
    - stop: SIGTERM で `KWL TIMER stopped wakes=0 armed=0` と `KWL EXIT`、`KWL FAILED` 0。
  - idle に残る約 1 秒に 1 回の timeout と 1 回の wake: `--testing` の compositor でも printers の file の 1 s の確かめ（printers-shell.c:278・280、対応表 2-04、p007 で timer へ）が残る。この Phase の 7 つではない。
- p002・p003 の QEMU の試験の流し直し（WS352 の kernel の poll と M2 の後、Q1 の依頼）:
  - `sh plan/ws353/tests/qemu-display-fence.sh build/ws353-p005-p002`: 全 step PASS（idle の 30 s で番号 3→3、wait 3 回、cancel、limit、race 600 登録、各 step の間の quiet）。latency は変化の始まりから 248 ms 以内。
  - `sh plan/ws353/tests/qemu-hotplug-wait.sh build/ws353-p005-p003`: 全 step PASS（idle の 30 s は poll 1 回、別の fence の 200 回の submit の間の poll 1（起きない）、300 回の競い、16 の上限、plug 2379 ms・unplug 2222 ms は kernel の fence と同時（vulkan-later 0）、device lost で返る）。
- 証拠: `build/tmp/ws353-timer`（host）、`build/ws353-p005-qemu`・`build/ws353-p005-qemu2`・`build/ws353-p005-p002`・`build/ws353-p005-p003`（summary.txt、kwl.log は serial で読んだ写しで記録用）。

### 未実施

- media の helper の QEMU での確かめ: guest に media library の client（`kl_system_media_*` を使う物は Photos だけ）が無く、試験の client を足すと userland の Makefile を変える事になる。code の読みと build だけ。host 試験も無い（helper の起動が固定の path の CLI のため）。
- 去った client の buffer の解放の QEMU での確かめ（試験の client は buffer を作らない。QEMU の run で timer は 0 回発火）。host 試験で代えた。
- cursor の写しの手放しの wake、system.c の 2 件（network の鍵の段、ENROLLED）の QEMU での確かめ（cursor の plane・networkd の鍵・sessiond の session が要る）。code の読みと build だけ。
- BUG-264 の `hotplug-guest.sh`（AAT の image、sessiond の session の利用者の compositor）: 流していない。上の QEMU は root の compositor で、sessiond の chown を待つ形（残した 100 ms の scan）は通っていない。
- WS352 の poll の診断の kernel での確かめ（KWL PERF で代えた、上の 5）。
- Linux・FreeBSD の backend の build（`kl_backend_input_events` を足した。keiland-linux は崩れてよい）。arm64・riscv64 は build だけ。実機は p016。

### p006 への申し送り

- timer の使い方: `kwl_timer_init(&t, "name", handler)` を一度、予定ができたら `kwl_timer_arm(&server->timers, &t, due_ms)`、終われば `kwl_timer_disarm`。handler は `(void *context, struct kwl_timer *timer)` で context は server。handler は全 thread の出口の後・client の要求の前に呼ばれる。`kwl_timer_arm` が ENOSPC を返したら（起きないはずだが）`kwl_due` に落とす（retire と media の形）。heap は 128、p006・p007 で 40 余りを足しても足りる。
- 毎 pass の「`now >= x_until` の比べ」と `kwl_due` を「始めに arm・終わりに disarm・handler で処理」に置き換える。この Phase の retire と media の handler は空で、pass の中の今の処理が走る形（設計からの変更 3）。p006 で機能の処理を handler に移すなら、`kwl_system_tick` の順（WS309 §3.3）に注意。
- `kwl_due` と `kwl_due_soon` は loop の期限として残っている（p008 で -1 に）。この Phase で消した `kwl_due_soon` は 7 つ（cursor・glass・shot・system 2・media・retire）。残りの `kwl_due_soon` は animation・touch・keyboard などで p007・p008。
- idle の QEMU で残る 1 秒の timeout は printers の 1 s（2-04、p007）。p006・p007 の後に `qemu-main-wake.sh` の idle の閾値（今は 5 pass/s・2 timeout/s）を狭められる。
- 入力の 100 ms の settle（2-03）の push の形（(A)、Q1 が後の Phase に足す）の見込み:
  - sessiond（`userland/desktop/sessiond/seat.c`・`session.c`・`greeter.c`）: ADD・OVERFLOW の事象で `sessiond_seat_give` した後に、session の control の fd（greeter は auth の fd）へ 1 行（例 `SEAT given`）を書く。
  - backend（`libkeiland-backend-zedbsd/session-zedbsd.c`）: その行を要求の答えとは別の知らせとして読み（`session_kind` に種類を足す、`session_request` を変えない）、host の callback（今の `input_changed` を呼ぶか、新しい `seat_given`）を呼ぶ。
  - compositor（`backend-host.c`・`main.c`・`kwl.h`）: その知らせで scan を 1 回頼み、`KWL_INPUT_SETTLE_MS`・`KWL_INPUT_SETTLE_SCAN_MS` と `input_settle_until` を消す。sessiond の無い compositor（`--testing`、root）は INPUT の事象の scan で足りる（node は root の物で開ける）。
  - 試験: BUG-264 の `hotplug-guest.sh`（10 tick 以内）。
