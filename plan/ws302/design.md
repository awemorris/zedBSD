<!-- awesome-plan project=zedbsd record=ws302-design -->

# WS302 設計: デスクトップスレッドバランサ（DTB）

- 版: **第 3 版**（2026-11-14 P6、q2093-i01）。第 2 版（2026-10-31 P5、main 3d820d9ca の code で書いた）を今の main（fd18bd61ee）の code・build system と照らし直し、Q302-1〜7 の決定（2026-11-14）を書き入れた。第 2 版からの変更と今の code との食い違いは下の「第 3 版の改訂」、第 3 版の review は §15。
- 第 2 版の版: 第 1 版（f2d6ca7b4）に design-reviewer の指摘（[design-review.md](design-review.md)、blocker 2・major 11・minor 13）を反映した。行き先は §14。
- 基点: worktree `p6-ws302`（branch `agent/p6-ws302`、main fd18bd61ee）。§2.1〜§2.8 は第 2 版の基点（3d820d9ca）の事実と行のまま残し、今の行と変わった事実は §2.9 に集めた。第 3 版で足した本文の file:行 は fd18bd61ee の物。
- 読んだ物: `AGENTS.md`、`plan/agents/protocol.md`、[ws.md](ws.md)、[survey-source.md](survey-source.md)（2026-10-29、main 61630bb0b の調べ。行は §2.8 で引き直した）、
  `plan/ws333/design.md`（§3・§4.1・§4.4・§4.5・§4.12・§9・§11）と `plan/ws333/ws.md`、`plan/ws309/ws.md` と design.md §1〜§3、`plan/guardrail.md`（範囲と構造・driver は HAL を知らない・出典・開発の管理情報）、
  `plan/master-design-policy.md`（§10・§11）、`plan/coding-style.md`（§2・§10・§13）、`plan/tools.md`（計測の道具）。source は §2 に挙げた物。
- 出典: 外部の source（Linux・FreeBSD など）は読んでいない。web も使っていない。設計は zedBSD の code の事実（§2）とユーザーの決定（§0）から立てた。
- 第 3 版で読んだ物（基点 fd18bd61ee）: `src/kern/sched.c`・`include/kern/sched.h`・`src/kern/thread.c`・`include/kern/thread.h`・`include/kern/process.h`・`src/kern/syscall.c`（syscall の入口・setpriority）・`src/kern/signal.c`（user への戻り）・`include/kern/cpuperf.h`・`src/kern/cpuperf.c`（sched_set_cpu の呼び）・`src/drivers/platform/pcat/cpu-x86/cpu-x86.c`・`src/drivers/platform/pcat/intel-tco.c`・`src/drivers/gpu/gpu.c`（gpu_ioctl）・`include/uapi/gpu*.h`・`src/drivers/generic/input.c`・`src/kern/entry.c`・`include/kern/clock.h`・`include/uapi/sysctl.h`・`include/kern/boot.h`・`src/hal/{x86_64,arm64,riscv64}/task.c`（hal_task_transfer）、build の `Makefile`・`mk/config.mk`・`mk/kernel.mk`・`mk/test-config.mk`・`mk/test-kernel.mk`・`mk/tracking.mk`・`mk/platform-*/vmunix.mk`・`mk/platform-*/defaults.mk`、`plan/ws333/ws.md`・`plan/ws333/design.md` §4.5・§16、`plan/ws353/ws.md`。

## 第 3 版の改訂（2026-11-14）

### 改訂の要点（第 2 版から変えた所）

1. **問いが決まった**（2026-11-14、Q302-1〜7 は全部推し）。§10 を決定の表に書き直し、本文の「推し」「Q302-n の答え次第」を決定として読む。
2. **build system の作り直しに合わせた**: config は `config/kernel-options.list`・`Makefile` の `ZEDBSD_CONFIG_CPPFLAGS`・`platform/amd64/vmunix.mk` ではなく、`mk/config.mk`（`CONFIG_* ?= n`）と `mk/kernel.mk`（`ifneq ($(filter y,$(CONFIG_X)),)` で `KERN_CPPFLAGS += -DCONFIG_X=1`・`KERN_SOURCES +=`）。n の時 macro は定義されない（`#if CONFIG_DTB` は 0）。Makefile・mk は Claude が変えないので、差分は案（§4.11）。
3. **platform を amd64 だけにしない**: 新しい mk には platform の欄が無く、code は arch に依らない。`CONFIG_DTB` は全 platform で build でき、x86_64・arm64（rpi4・rpi5）・riscv64（vf2）で n と y の build を確かめる。core の種類が 1 つの機械（今の arm64・riscv64 の全部、hybrid でない x86）では種類の表が ANY になり、priority（nice・GPU の app）の hint だけが効く。
4. **CPU の種類・相対の性能は WS333 の口だけを使う**（重複を作らない）: `kern_cpuperf_cpu()`・`kern_cpuperf_generation()` は `CONFIG_KERN_THERMAL=y` の時だけ link される（`src/kern/cpuperf.c` は mk/kernel.mk:1250〜1256 の KERN_THERMAL の下）。x86 の core の種類（CPUID 1AH）を出す provider の登録は `CONFIG_KERN_THERMAL && CONFIG_DRIVER_X86_POWER_CONTROL` の下（cpu-x86.c:172）。DTB は `CONFIG_KERN_THERMAL` が n なら cpuperf を呼ばず種類の表を ANY にする（`#if CONFIG_KERN_THERMAL`）。5330 の評価の build は WS333 の前提（KERN_THERMAL と X86_POWER_CONTROL を y）に `CONFIG_DTB=y` を足す（§6.3）。
5. **sched の今に合わせた**: (a) thread の affinity（`SCHED_CPU_FREE`・`SCHED_CPU_BOUND`、sched.h の `enum sched_cpu_affinity`、BUG-422）が入り、盗みは BOUND の thread を飛ばす（`first_free_thread` sched.c:2677）。DTB は BOUND の thread を動かさない（§4.5・§4.6.4）。(b) `sched_set_cpu` には外の呼び手が出来た（cpuperf.c:3590 の測りの thread、intel-tco.c:1106 の見張りの thread。どちらも `kthread_create` の直後の NEW の kernel の thread を BOUND にする）。第 2 版 §4.6.7 の「NEW を断る確かめ直しを `sched_set_cpu` に入れる」は今の呼び手を壊すので、確かめ直しは `sched_hint_place` の道だけに置く（§4.6.7 を書き直した）。(c) amd64 の timer の割り込みは 100 Hz になった（2026-11-13 の WS333 の決定）。kernel の時間の単位（`clock_ticks()`）は counter のある機械では 1 ms のまま（clock.c:11〜20）で、割り込みの rate と切り離されている。ms の閾値は `kern_ms_to_ticks()` で直すので値の意味は変わらないが、tick の割り込みで足す統計（`run_ticks`・`user_streak`）は 1 回に約 10 単位ずつ進み、分解能は約 10 ms（§4.4.1 の BUSY の 20 ms は割り込み 2 回）。飢えの防ぎ（100 ms）と Q302-2 の「1 tick 待った」は割り込みの時にしか見ないので、実際の粒は約 10 ms。(d) tick の計数は 1 tick ずつではなく `elapsed`（idle の遅い rate では複数 tick）で足されている（sched.c:1053〜1057）。統計も `elapsed` で足す。
6. **GPU の時刻**: 仕事を出す ioctl に `GPU_JOB_SUBMIT`（`include/uapi/gpu-job.h:29`、G 49）が足されていた。画面に出す ioctl の組（`GPU_PRESENT`・`GPU_DISPLAY_PRESENT`・`_SYNC`・`GPU_DISPLAY_COMMIT`）は `gpu_console_end`（gpu.c:2165）の組と同じ。通知は `gpu_ioctl`（gpu.c:2111）の成功の後、何も持たない所に置く。WS353（compositor の待ち方）は thread を名前つきの待ちに分けるが、DTB の印は process ごとの時刻なので影響しない。compositor の thread が timeout 無しで眠る形は BUSY・HEAVY に当たりにくくする方向で、規則の前提と合う。
7. **入力の通知の置き場所**: 第 2 版は publish の所（irqsave の lock の中）に置いたが、`drv_input_device_emit_at`・`drv_input_device_emit_key_event` の最後の `poll_notify_object` の後（lock を全部離した後、`published` の時だけ）に置く。atomic だけである事は変わらない。
8. **p002 の source の置き場所**: 入力と GPU の通知の本体と入力の時刻は `src/kern/dtb.c`（p002 で作り、p004 で DTB の thread を足す）。新しい source の追加は mk の変更なので、mk の案（§4.11）が承認されるまで、y の build は試験の overlay の makefile（`plan/ws302/tests/dtb-overlay.mk`、`make -f Makefile -f …`）で案と同じ効果を与えて確かめる（p002 の phase.md）。

### 今の code・記録との食い違いの一覧（第 2 版の前提 → 今）

| # | 第 2 版の前提（3d820d9ca） | 今（fd18bd61ee） | 扱い |
| --- | --- | --- | --- |
| D-1 | config は `config/kernel-options.list`・`Makefile:941`・`platform/amd64/vmunix.mk` | `mk/config.mk`・`mk/kernel.mk`・`mk/test-config.mk`・`mk/test-kernel.mk`、config の file は `config/config.<機種>.mk` | §4.11 の mk の案に書き直した |
| D-2 | `CONFIG_DTB` は amd64 だけ | mk に platform の欄が無い。全 platform で build できる形にする | 改訂の要点 3 |
| D-3 | cpuperf の provider は `CONFIG_CPU_PERF` に依らず登録され、core の種類は常に出る（WS333 design §4.5 の表もそう書く） | `cpuperf.c` は `CONFIG_KERN_THERMAL` の時だけ build され、x86 の provider の登録は `CONFIG_KERN_THERMAL && CONFIG_DRIVER_X86_POWER_CONTROL` の時だけ（cpu-x86.c:111〜188）。x86_64 の既定は KERN_THERMAL が n（mk/platform-x86_64/defaults.mk:55） | DTB は KERN_THERMAL が無ければ種類を ANY にする。**WS333 の設計 §4.5 の表と code の食い違いは WS333 の記録の問題として Q1 へ**（あるべき形: 種類と topology は CPUID だけで分かり MSR に依らないので、WS333 §4.5 のとおり X86_POWER_CONTROL に依らず出す方が DTB には良い。ただ WS333 は completed なので直すかはユーザー） |
| D-4 | `kern_cpuperf_cpu` の flag は `READ_SAMPLE` | `KERN_CPUPERF_READ_FRESH`（cpuperf.h:102）。flag 0 なら spinlock だけで読む（cpuperf.h:343〜356） | DTB は flag 0 で読む（周期の中で CPU を起こさない） |
| D-5 | `sched_set_cpu` は外の呼び手が無い | cpuperf.c:3590・intel-tco.c:1106 が NEW の kernel の thread を BOUND に | §4.6.7 を書き直した（確かめ直しは hint の道だけ） |
| D-6 | affinity・cpuset は無い | `SCHED_CPU_BOUND` があり、盗みは BOUND を飛ばす | DTB は BOUND の thread を動かさない（user の thread を BOUND にする口は今は無いが、将来に備えて確かめる） |
| D-7 | tick の割り込みは 1 ms（「1 tick（1 ms）」） | amd64 の割り込みは 100 Hz。時間の単位は 1 ms のまま（clock.c:11〜20）で、単位の長さは clock だけが知り、他は `kern_ms_to_ticks` を使う（clock.h:22〜32 の約束） | 閾値は ms で書き tick に直す（第 2 版のまま）。割り込みで足す統計と判定の粒が約 10 ms になる注を足した |
| D-8 | tick の計数は 1 tick | `elapsed`（前の割り込みからの単位の数。忙しい rate で約 10、idle の rate ではもっと） | 統計も `elapsed` で足す |
| D-9 | GPU の仕事の ioctl は 4 つ | `GPU_JOB_SUBMIT` を足して 5 つ（`GPU_COMMAND`・`GPU_COMMAND_SUBMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_JOB_SUBMIT`・`GPU_JOB_COMMIT`） | §4.3 |
| D-10 | `hal_task_transfer` の「後継の文脈で去った task は移せる」は amd64 の実装だけの性質（§12 の 3） | arm64（task.c:625）・riscv64（task.c:627）の実装も同じ性質を comment に書いている。hal.h:1041 の宣言には契約の文が無いまま | §5・§12 の 3 を直した。hal.h は変えない |
| D-11 | 行の番号（sched.c 2978 行など） | sched.c 3151 行、他も動いた | §2.9 の読み替えの表 |
| D-12 | `kern_thermal_init()` の後で `kern_dtb_init()` | `kern_cpuperf_init()`・`kern_thermal_init()` は `#if CONFIG_KERN_THERMAL` の中（entry.c:385〜418） | `kern_dtb_init()` は thermal の block の後に、KERN_THERMAL に依らず置く（§4.9） |
| D-13 | config の名前 `CONFIG_DTB` | 新しい mk の名前の作法は kernel の枠組みが `CONFIG_KERN_*`（`CONFIG_KERN_THERMAL`・`CONFIG_KERN_NET`）。また「DTB」は arm64・riscv64 の device tree blob（`*.dtb`、mk/platform-vf2/firmware.mk など）と同じ略 | ユーザーの決定（2026-10-27）の名前 `CONFIG_DTB` のまま案を出す。`CONFIG_KERN_DTB` に揃えるか、device tree と紛れない名前（例 `CONFIG_KERN_THREAD_BALANCER`）にするかは mk の案と一緒にユーザーに聞く（Q1） |
| D-14 | user の既定の段 10 は `SCHED_HINT_USER_DEFAULT` | 変わらない。kthread_create の呼び出しは 101 か所（第 2 版 97）、既定でない物に TCO の見張り（`SCHED_PRIOR_HIGH`）・thermal の thread・uvc・usb-bt など | §2.1 の数を直した。結論（kernel の大半は 8）は同じ |

## 0. ユーザーの決定（要点）

記録の規則（2026-10-30）により、ここには要点だけを書く。2026-10-27 の発言は [ws.md](ws.md) の「出典」にある（その記録の形を直すかは Q1、§10.2）。

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-27 | config で有効にできる DTB を kernel に足す。DTB は scheduler ではなく、thread を適した core に移すよう sched に依頼し、balancing と scheduling の hint を出す。HID・GPU・process/thread の統計と nice を使う。GPU を使う app は応答の priority を上げて速い core に、syscall の無いまま長く忙しい thread は遅く省電力の core に、terminal の app（GPU を使わず tty がある）は UI の応答を落とさない範囲で速い core に。まずどの情報からどんな balancing ができるかを検討する | §3（情報 → 性質 → hint の表）、§4（DTB と sched の口） |
| 2026-10-27 09:39（クリック） | `CONFIG_DTB` の既定は off。nice は core の選びに加えて sched の priority にも効かせる | §4.11（config）、§4.7（nice の段と飢えの防ぎ） |
| 2026-10-31（Q1 経由の依頼の趣旨） | 不在の間（昼頃まで）設計だけ進められる WS は設計を進める。WS333 の cpuperf の口（P/E の種類・相対の性能）が main に入ったので WS302 の設計の時期 | この文書 |
| 2026-11-14 | Q302-1〜7 を全部推しの案で決める | §10.1（決定の表）。本文の「推し」は決定として読む |

## 1. 目標と範囲

- 目標（ws.md）: `CONFIG_DTB` で有効にできる DTB を kernel に入れ、5330（P-core 2・E-core 8）で thread の性質を統計と nice から見分けて sched に hint を出し、DTB を切った時に比べて desktop の応答を落とさず、長く忙しい計算の thread を E-core に寄せて電力を下げる事を数で示す。
- この文書が決める物: どの統計からどの性質を推すか（§3）、統計の取り方と置き場所（§4.3）、分類と規則の強さの順（§4.4）、sched への hint の口と sched の最小の変更（§4.5・§4.6）、nice と priority（§4.7）、5330 の割り付けの方針（§4.8）、DTB の周期の処理（§4.9）、状態・資源・lock・失敗（§4.10〜§4.14）、config と観察（§4.11・§4.15）、試験と受け入れ（§6）、Phase（§7）。
- 範囲の外: sched の作り直し（hint を受ける最小の変更だけ）、周波数の制御（WS333）、hybrid でない機械の評価、Keiland の thread の作りの変更（WS309）、compositor からの focus の hint（Q302-4 の決定 B で、p006 の数で要る時だけ条件つきの Phase）。

## 2. 今の振る舞い（code の事実。§2.1〜§2.8 は main 3d820d9ca、§2.9 が fd18bd61ee での読み替え）

### 2.1 sched（`src/kern/sched.c` 2978 行、`include/kern/sched.h`）

- CPU ごとの `struct sched_cpu`（sched.c:181）: spinlock 1 本（`LOCK_RANK_SCHEDULER` 200、`include/kern/lock.h:64`）、priority 16 段ごとの `run[]` と `woken[]`、sleep の queue、idle・current・retired、`need_resched`・`preempt`・`preempt_count`、tick の分類の時間 `times[]`。global な run queue は無い。
- priority は 0 が最高・15 が最低・既定 8（sched.h の `SCHED_PRIOR_*`・`SCHED_PRIORITY_DEFAULT`）。user の thread は作る時に全部 8（`src/kern/thread.c:104`・`:190`、idle は `:410`）。kernel の thread は `kthread_create` の引数で決め（`:256`）、**97 か所の呼び出しの大半が `SCHED_PRIORITY_DEFAULT`（8）**（USB の HID の decode・xHCI・`gpu_monitor`・pageout・reaper など）、既定でないのは `USB_BT_SCO_PRIORITY 4`（usb-bt.c:101）など少数。作った後に priority を変える関数は無い。
- 取り出しは厳密に上の段から（`pick_next_locked` sched.c:2192）。aging は無い。queue の出し入れは `thread->sched.priority` を添字にする（`queue_remove_thread` :2178）。sleep の queue は段で分けない（同）。
- 起床（`sched_wakeup` :520）は thread の居る CPU にだけ戻す。起こされた thread の priority が走っている thread 以上なら `woken[]` に入れ `preempt` を立てる（`wake_enqueue_locked` :2051）。全部が 8 の今は、どの起床も横取りを頼む。移動の途中の thread への起床は `SCHED_WAKE_PENDING` を立てて持ち越す（:545）。
- 切り替わる所: user への戻り（`kernel_user_return_handler` signal.c:401 → `sched_preempt_point` sched.c:910）、tick（`sched_clock_cpu` :1009、quantum 10 ms の終わりで `sched_yield`）、CPU の通知（`sched_cpu_notify` :1790、`clock.c:207` から。その直前に `kern_cpuperf_notify` clock.c:205）。
- 新しい thread の置き場所（`sched_prepare_thread` :398）: 作り手の CPU に backlog が無ければそこ、あれば idle の CPU（`choose_idle_cpu` :2390、番号の小さい順）、それも無ければ round robin（`choose_cpu` :2269）。
- 負荷の分け: idle の CPU が他の CPU の queue から盗む（`sched_idle` :1577 → `steal_runnable` :2554 → `take_stealable_locked` :2479、同じ tick に入った thread は取らない）と、忙しい CPU が tick で idle の CPU を 1 つ起こす（:1125〜1137 の `backlog`）だけ。core の種類・性能・SMT の兄弟を知らない。
- 移動: `sched_set_cpu`（:1691）は走っていない thread を移す。sched.c の外に呼び手は無い。**状態の検査は lock の外で、RUNNING・ZOMBIE・DEAD だけを断る**（:1704〜1712）。NEW の thread（`sched_add` :447 が後で元の CPU に入れる）と REAPING の thread（`thread_wait` が task を壊す、thread.c:581〜589）は断らない。今は外の呼び手が無いので表に出ない欠け（§4.6.7 で直す）。
- 走っている thread は移せない: amd64 の `hal_task_transfer`（`src/hal/amd64/task.c:832`）は `run_cpu >= 0` の task と IRQ の待ちを持つ task を `HAL_ERR_BUSY` で断る。`run_cpu` は切り替えの後、次の context が走り始めた所で `amd64_task_finish_switch`（:676）が -1 にする（新しい task の入口も含む）。だから後継の context の中では、去った task は既に移せる。盗みもこの性質に頼っている（sched.c:2550 の comment）。`hal.h:1035` の宣言には契約の文が無い（§5）。
- `sched_exit_current`（:938）が去る thread を `cpu->retired` に置き、後継が `complete_retired`（:2237）で片付ける形が既にある。切り替えの後の 4 か所（:813・:897・:964・:2361）で呼ぶ。新しい task の入口から始まる後継は呼ばない（:960 の comment）。

### 2.2 nice・syscall・計数

- `setpriority`（`sys_setpriority_call` syscall.c:8880）は process の `nice_value`（`include/kern/process.h:129`、-20〜+20 に丸める）に入れるだけ。下げるのは root だけ（:8936）、他人の process は euid の判定。fork で継ぐ（process.c:1113）。sched は nice を読まない。`/dev/system` の process の情報に `nice_value` と `has_controlling_terminal` がある（`include/uapi/system.h:81`）。
- syscall の入口（`kernel_syscall_handler` syscall.c:10420）は割り込みを止めた窓で `sched_accounting_kernel_enter()`（:10445）を呼び、出口で leave（:10516）。この窓は同じ CPU の tick と競わない。
- tick の計数（`sched_clock_cpu` :1062〜1100）: 走っている user の thread の process に `cpu_ticks`・`user_ticks`・`system_ticks` を足す（`accounting_kernel_depth` で user か kernel か）。thread ごとの時間・syscall の数・起床の数は無い。
- CPU ごとの時間は `sched_cpu_time`（:1517）と `hw.cputimes`（`include/uapi/sysctl.h:72`）。

### 2.3 HID

- input の core（`src/drivers/generic/input.c`）は、report を `drv_input_subscriber_publish`（:1602）で subscriber に配る。呼び出しは 9 か所: device の外し（`drv_input_device_unregister` :671 の中、押されていた key の離しと DETACH、:826・838・848・853）、普通の入力（`drv_input_device_emit_at` :914 の中の :946、`drv_input_device_emit_key_event` :960 の中の :1081）、reader が開いた時の resync（:2526・2576・2617、`INPUT_REPORT_RESYNC_BEGIN` など）。callback は subscriber の spinlock（`LOCK_RANK_DEVICE`、irqsave）の中で、短く眠らない約束（:1600 の comment）。購読の口は `drv_input_subscribe`（:1517、今は console だけ）。
- input.c は 15 の vmunix.mk のうち 12 が build する。最後の入力の時刻・数を持つ所は無い。

### 2.4 GPU

- `/dev/gpuN` の ioctl は全部 `gpu_ioctl`（`src/drivers/gpu/gpu.c:1840`）→ `gpu_ioctl_body`（:2113）を通る。GPU の仕事を出す ioctl は `GPU_COMMAND`・`GPU_COMMAND_SUBMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_JOB_COMMIT`、画面に出す ioctl は `GPU_PRESENT`・`GPU_DISPLAY_PRESENT`・`GPU_DISPLAY_PRESENT_SYNC`・`GPU_DISPLAY_COMMIT`（`include/uapi/gpu*.h`）。
- `/dev/gpuN` を開けるのは root と node の持ち主（seat の利用者、`gpu_open` :1472〜1508）。`struct gpu_session`（:225）に process・thread ごとの使用量は無い。device ごとの `hw.gputelemetry` だけ。
- Keiland の compositor は GPU と表示を libvulkan だけで扱い（guardrail）、client の app も libvulkan で `/dev/gpuN` に仕事を出す。画面に出す ioctl は compositor（と game mode の直の scanout）だけが呼ぶ。

### 2.5 CPU の性能（WS333 の cpuperf、main に入った物）

- `kern_cpuperf_cpu(cpu, flags, info)`（`include/kern/cpuperf.h:354`、cpuperf.c:577）: `READ_SAMPLE` 無しなら box の spinlock（`LOCK_RANK_DEVICE`、irqsave）だけで読む。`info.topology.core_class`（UNKNOWN・PERFORMANCE・EFFICIENCY）・`package/die/module/core/thread`、`relative`（0〜100、UNKNOWN）、`available`・`efficiency`（HFI の 0〜255、UNKNOWN）。
- `kern_cpuperf_generation()`（:385、lock 無し）: topology・domain・種類・能力・相対・HFI のどれかが変わると動く。
- amd64 の CPU の driver（`src/drivers/platform/pcat/cpu-x86/cpu-x86.c`）は `CONFIG_CPU_PERF` に依らず provider を登録し（:171）、CPUID 1AH で core の種類を出す（`cpu_x86_read_kind` :495、hybrid でなければ全部 PERFORMANCE）。`relative` と HFI は `CONFIG_CPU_PERF=y` で、最初の strategy の後だけ（WS333 design §4.5 の表）。
- `kern_cpuperf_init()` は `src/kern/entry.c:382`、`kern_thermal_init()` は :405、x86 の driver の登録は `src/kern/platform/pcat.c:497`。

### 2.6 config と起動の parameter

- `config/kernel-options.list` の行（例 `CONFIG_CPU_PERF|bool|…|amd64|n|`、:3）、`Makefile:941` の `-DCONFIG_CPU_PERF=0/1`（`ZEDBSD_CONFIG_CPPFLAGS`、15 の vmunix.mk のうち mips32-rg350 以外の 14 が使う）、`platform/amd64/vmunix.mk` の source の列（:493 に `src/kern/cpuperf.c`）。amd64 の既定の build は full LTO（`.o` は bitcode）。
- 起動の parameter は `include/kern/boot.h:107` の enum（`KERN_BOOT_PARAMETER_CPUPERF` など）と `src/kern/boot.c` の名前の表と値の検査の表。

### 2.7 Keiland の thread

- WS309（design §2・§3.1）が compositor の thread を台帳（`kwl_thread_start`、名前と本数）に整理した。kernel に thread の名前は無い（`pthread_setname_np` も無い）。compositor・app の thread に priority を付ける所は無い。

### 2.8 survey-source.md・ws.md との違い

- 行の引き直し: `setpriority` は syscall.c:8880（survey では 8863）、syscall の入口の accounting は :10445・:10516（survey 10427・10498）、`sched_set_cpu` は sched.c:1691（1665）、盗みは :2554（2528）、`syscall_dispatch_body` は :9814（ws.md の 9796）、`struct gpu_session` は gpu.c:225（ws.md の 219）、sched.c は 2978 行（ws.md の 2952）。
- survey §0 の ws.md の食い違い（nice と CONFIG_DTB の既定）は、今の ws.md では既に直っている（「含む」と D1・D3 が 2026-10-27 09:39 の決定に揃っている）。
- survey §6 の 5「topology を知らない（WS301 待ち）」は解けた（§2.5）。

### 2.9 第 3 版の読み替え（main fd18bd61ee）

| 物 | 第 2 版（3d820d9ca） | 今 | 変わった事 |
| --- | --- | --- | --- |
| sched.c の長さ | 2978 行 | 3151 行 | 診断（`kern.perf=1`）・affinity・`SCHED_WAKE_LATENCY` が足された |
| `struct sched_cpu` | :181 | :187 | 変わらない（`yield_due`・`tick_last`・perf の窓が増えた） |
| `struct sched` | sched.h | sched.h:77〜156 | `affinity`（`enum sched_cpu_affinity`）、`perf_*` の診断の field、`queued_tick` |
| 取り出し `pick_next_locked` | :2192 | :2247 | — |
| 起床 `sched_wakeup` / `wake_enqueue_locked` | :520 / :2051 | :531 / :2106 | 起床の 3 つの道（`sched_wakeup` :581、tick の期限 :1068、`sched_notify_task` :1282）は全部 `wake_enqueue_locked` を通る |
| tick `sched_clock_cpu` | :1009 | :1020 | 計数は `elapsed`（:1053〜1057）、走っている thread の計数は :1075〜1104、backlog の助けは :1138〜1155 |
| `sched_set_cpu` | :1691 | :1721 | affinity の引数（`SCHED_CPU_FREE`・`_BOUND`）、外の呼び手 2 つ（D-5） |
| `sched_cpu_notify` | :1790 | :1844 | — |
| `sched_exit_current`・`complete_retired` | :938・:2237 | :949・:2292 | — |
| 盗み `steal_runnable`・`take_stealable_locked` | :2554・:2479 | :2727・:2624 | BOUND を飛ばす `first_free_thread`（:2677） |
| `choose_idle_cpu`・`choose_cpu`・`notify_cpu` | :2390・:2269・:2290 | :2445・:2324・:2345 | — |
| `retarget_migrating` | :2533 | :2706 | — |
| thread の priority の初め | thread.c:104・190・256・410 | 同じ行 | — |
| `kthread_create` の呼び出し | 97 か所 | 101 か所（src の中） | D-14 |
| syscall の入口の窓 | syscall.c:10445・10516 | :10448・:10519 | 形は同じ（割り込みを止めて `sched_accounting_kernel_enter`） |
| `sys_setpriority_call` | :8880 | :8883、値の store は :8947 | — |
| `struct process` の `nice_value`・`controlling_tty` | process.h:129・:145 | :129・:149 | — |
| `process_find_next_ref` | process.c:773 | :778 | — |
| `gpu_ioctl`・`gpu_ioctl_body` | gpu.c:1840・:2113 | :2111・:2384 | `GPU_JOB_SUBMIT` が増えた（D-9） |
| `struct gpu_session` | gpu.c:225 | :277 | — |
| input の publish（普通の入力） | input.c:946・:1081 | 同じ行 | — |
| `kern_cpuperf_cpu`・`kern_cpuperf_generation` | cpuperf.h:354・:385 | :358・:396 | flag は `READ_FRESH`（D-4）、cpuperf.c は KERN_THERMAL の時だけ（D-3） |
| `kern_cpuperf_init`・`kern_thermal_init` | entry.c:382・:405 | :387・:412（`#if CONFIG_KERN_THERMAL`） | D-12 |
| `hal_task_transfer` | hal.h:1035、src/hal/amd64/task.c:832 | hal.h:1041、src/hal/x86_64/task.c:832 | D-10 |
| sysctl の KERN の最後の番号 | `KERN_BOOT_KMSG 10` | 同じ（sysctl.h:512） | `kern.dtb` は 11 のまま使える |
| 起動の parameter の enum | boot.h:107 | 同じ所（`KERN_BOOT_PARAMETER_CPUPERF` :129・`_CPUPERF_HWP` :130） | — |

## 3. どの情報から何を推し、どんな hint にするか（ws.md の表を埋めた物）

priority の数は §4.7 の決定（Q302-3 の A: user の thread は kernel の既定の 8 より上に置かない）。

| 情報（源、取り方） | 推す性質 | hint | 取る代価 | 外れる時と注意 |
| --- | --- | --- | --- | --- |
| 画面に出す GPU の ioctl の最後の時刻（GPU の core、process ごと、新） | 画面を作る process（compositor、game mode の app） | 速い core（PLACE）、priority 8（user の他の thread より上、kernel の既定と同じ） | ioctl 1 回に atomic の store 1 つ | game mode の app も当たる（望ましい） |
| GPU の仕事を出す ioctl の最後の時刻（GPU の core、process ごと、新） | GPU を使う app（desktop の client・game・video） | 速い core（PLACE）、priority 9（CPU を使い過ぎない thread だけ、§4.4.3） | 同上 | 背景の GPU の計算だけの process も当たる（kernel には見分けが無い）。nice > 0 で外せる。`/dev/gpuN` を開ける利用者（seat の利用者と root）だけが得る |
| syscall の無いまま user で走り続けた長さ（tick と syscall の入口、thread ごと、新） | 長く忙しい計算 | 遅い省電力の core（PLACE）、priority 10（上げを外す） | tick ごとに加算 1 つ、syscall ごとに store 2 つ | compiler の最適化の段も当たる。build の時間は「空いた P に溢れてよい」（§4.6.4、Q302-2）で守る |
| controlling tty があり GPU を使わない（process、既存＋GPU） | terminal の中の道具（shell・editor・短い command） | 速い core（PLACE_IDLE: 行き先は兄弟も idle の P だけ、移動の予算は最後）、人が見ている時だけ | 無し（既存の field） | build の compiler も tty を持つ。忙しい規則が先に効く（Q302-1） |
| nice（process、既存） | 利用者の意図（背景か急ぎか） | nice > 0 は遅い core（PLACE・溢れない）・priority を下げる、nice < 0 は速い core・priority を上げる（8 まで） | 無し | 他の規則より先（利用者の明示の意図、Q302-1） |
| HID の最後の入力の時刻と数（input の core、全体で 1 つ、新） | 人が操作中か | 操作中: 上の規則を全部。無操作が続く: terminal の規則を止め、周期を延ばす | 普通の入力の report ごとに atomic 2 つ | 入力の宛先の app は kernel に分からない（Q302-4） |
| 起床の数・走った tick（thread ごと、新） | 対話的か（短く走って眠る）、CPU を使い過ぎるか | 起床の数は **v1 の規則に使わず観察だけ**。走った tick の割合は GPU の上げの条件（§4.4.3） | 起床ごとに加算 1 つ | P が 4 つしかなく、弱い候補を増やすと UI の席を食う。p006 の数で要れば足す |
| 待ちの長さ（既存の `queued_tick`） | 飢えているか | priority の下げの飢えの防ぎ（§4.7） | tick ごとに見るのは queue の先頭だけ | — |
| compositor からの前面の app（UAPI、新、任意） | 前面の app | 前面を速い core | — | UAPI の決定が要る。v1 には入れない（Q302-4） |

## 4. 設計

### 4.1 全体の形

```
 user:  （v1 は書きの UAPI を足さない。観察の読みだけ Q302-6）
                                       |
 kernel:  src/kern/dtb.c（CONFIG_DTB=y だけ）: 入力・GPU の通知の受け口と入力の時刻（p002）、周期の thread「dtb」、process と thread を巡って分類し、sched に hint を渡す（p004）
          src/kern/dtb-policy.c（同）: 分類の純な関数（host 試験の対象）
          src/kern/sched-hint-test.c（CONFIG_DTB_TEST=y だけ）: 偽の種類の表と、無作為に hint・移動を出す試験の thread
             |  読む: kern_cpuperf_cpu / kern_cpuperf_generation（WS333、CONFIG_KERN_THERMAL=y の時だけ）、process の nice・tty・GPU の時刻、HID の時刻、sched の thread の計数
             v  渡す: sched_cpu_classes_set（CPU の種類の表）、sched_hint_set（thread の希望と priority）、sched_hint_place（今すぐ寄せる）
          src/kern/sched.c: hint を持ち、置き場所・盗み・idle の CPU の選び・走っている thread の手渡し・飢えの防ぎに使う（#if CONFIG_DTB）
          src/kern/syscall.c・sched.c の tick: thread の計数（#if CONFIG_DTB）
 driver:  src/drivers/gpu/gpu.c: kern_dtb_note_gpu()（gpu_ioctl の成功の後、何も持たない所で、提出と表示の ioctl だけ）
          src/drivers/generic/input.c: kern_dtb_note_input()（普通の入力の 2 つの関数の最後、lock を離した後）
          （driver は kern_* だけを呼ぶ。HAL を知らない規則のまま）
 HAL:     変えない。hal_task_transfer（止まっている task）と hal_cpu_notify を sched が今の使い方のまま使う
```

- **DTB は sched ではない**: 実行の順・CPU の選び・移動の実行は sched が決める。DTB は「この thread はどの種類の core を望むか、どの強さで、どの priority か」を書き、「今寄せてほしい」を頼むだけ。sched は CPU の空き具合を見て、頼みを果たせない時は断る（EAGAIN）。
- **sched は cpuperf を呼ばない**: sched の lock（rank 200）の中で cpuperf の box の lock（rank 125）は取れない。DTB が thread の文脈で cpuperf を読み、CPU の種類の表を sched に写す。
- **`CONFIG_DTB=n` は今と同じ**: sched・syscall・gpu・input の追加は全部 `#if CONFIG_DTB` か、`include/kern/dtb.h`・`include/kern/sched-hint.h` の空の inline。struct の field も同じ条件（Q302-5 の決定）。§4.6.7 の確かめ直しは `sched_hint_place` の道だけに置き、`sched_set_cpu` そのもの（今の呼び手は cpuperf と TCO）は変えない。

### 4.2 責務と境界

| 部品 | 持つ物 | 持たない物 |
| --- | --- | --- |
| DTB（dtb.c） | 周期の処理、process と thread の巡回、分類の状態（履歴の幅）、CPU の種類の表の作成、HID の時刻、移動の予算 | CPU の選び、queue の操作、周波数 |
| 分類（dtb-policy.c） | 入力（統計・nice・tty・時刻・HID の状態）→ hint の純な関数、規則の強さの順、閾値 | kernel の object |
| sched | hint の保存、priority の付け替え、CPU の種類の表、置き場所・盗み・idle の CPU の選びの好み、走っている thread の手渡し、飢えの防ぎ、kernel の中の thread の優先、thread の計数 | 分類、cpuperf の読み |
| cpuperf（WS333） | core の種類・相対の性能・HFI・世代 | DTB の事 |
| GPU の core・input の core | 自分の ioctl・入力の所で 1 行の通知 | 分類 |

### 4.3 統計（取る所・書き手・代価）

全部 `CONFIG_DTB=y` の時だけ（Q302-5）。thread の field は `struct sched` に埋める `struct sched_hint_state`（[proposed/include/kern/sched-hint.h](proposed/include/kern/sched-hint.h)）、process の field は `struct dtb_process_activity`（[proposed/include/kern/dtb.h](proposed/include/kern/dtb.h)）。

| 統計 | 書く所 | 読む所 | 同期 |
| --- | --- | --- | --- |
| `run_ticks`（走った tick） | `sched_clock_cpu` の「Charges the running thread」の block（その CPU の lock の中、書き手はその時の CPU の tick 1 つ）。process の `cpu_ticks` と同じく `elapsed` を足す | DTB | 書きも読みも atomic（u64。i386 でも割れない `atomic_u64_*`） |
| `user_streak`（syscall の後に user で走った tick、飽和する） | tick で `accounting_kernel_depth == 0` なら `elapsed` を足す（上限で止める）。syscall の入口の割り込みを止めた窓（syscall.c:10448 の直後）の `sched_note_syscall()` で 0 に | DTB | 書き手（tick と入口）はどちらも thread が走る CPU の上で割り込みを止めて書くので重ならない。DTB は load だけ |
| `syscalls`（数、一周してよい u32） | `sched_note_syscall()`（同じ窓） | DTB | 同上 |
| `wakeups`（u32、観察だけ） | 眠りから RUNNABLE にする所: `wake_enqueue_locked`（`sched_wakeup`・tick の期限・`sched_notify_task` の 3 つの道が通る）と、`sched_set_cpu` の移動の間に持ち越した起床（成功と失敗の 2 か所）。どれもその時に thread を持つ CPU の lock の中 | DTB（観察） | 書き手は時により違う CPU だが、いつも thread の CPU の lock の中。近似でよい |
| process の `gpu_submit_tick`・`present_tick` | `kern_dtb_note_gpu()`（gpu_ioctl の成功の後、呼んだ thread の文脈、何も持たない所） | DTB | atomic の store・load |
| HID の `input_tick`・`input_count`（全体で 1 つ） | `kern_dtb_note_input()`（`drv_input_device_emit_at` と `drv_input_device_emit_key_event` の最後、lock を全部離した後で `published` の時だけ。atomic だけ） | DTB | atomic |

- `kern_dtb_note_input()` は普通の入力の 2 つの関数だけで呼ぶ。device の外し（:826〜853）と reader の resync（:2526〜2617）は人の操作ではないので呼ばない。subscriber の枠は使わない（購読でなく input の core から kernel への 1 行の通知。kernel が driver の関数を呼ぶ向きを作らず、input.c の無い platform でも DTB が link できる）。
- `kern_dtb_note_gpu()` は ioctl の番号を GPU の core が `KERN_DTB_GPU_SUBMIT`（`GPU_COMMAND`・`GPU_COMMAND_SUBMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_JOB_SUBMIT`・`GPU_JOB_COMMIT`）・`KERN_DTB_GPU_PRESENT`（`GPU_PRESENT`・`GPU_DISPLAY_PRESENT`・`GPU_DISPLAY_PRESENT_SYNC`・`GPU_DISPLAY_COMMIT`、`gpu_console_end` と同じ組）に読み替えて渡す（DTB は GPU の UAPI を知らない）。読み替えの関数も `#if CONFIG_DTB` の中に置く（n の kernel に code を残さない）。thread ごとの GPU の時刻は v1 の規則に使わないので取らない（第 1 版から外した）。
- 代価の見積もり（p002 で測る）: tick に加算 2〜3、syscall に store 2、RUNNABLE にする所に加算 1、GPU の ioctl に store 1〜2、入力の report に atomic 2。

### 4.4 分類（dtb-policy.c）と規則の強さの順

#### 4.4.1 thread の印（DTB が履歴を持つ）

| 印 | 立つ | 消える | 履歴の幅 |
| --- | --- | --- | --- |
| BUSY | 周期の時点で `user_streak` ≥ `DTB_BUSY_ON_MS`（初め 200 ms） | syscall が増え、かつ `user_streak` < `DTB_BUSY_OFF_MS`（20 ms）の周期が `DTB_BUSY_OFF_PASSES`（10）続いた | 立つのは速く、消えるのは遅く（行ったり来たりの防ぎ） |
| HEAVY | 前の 1 s（周期 10 回）に走った tick の割合が `DTB_HEAVY_PERCENT`（50%）以上 | 割合が 30% 未満の周期が 10 続いた | GPU の priority の上げの条件（§4.4.3、review M8） |
| GPU（process） | process の `gpu_submit_tick` が `DTB_GPU_WINDOW_MS`（1000 ms）以内 | 窓を出た | 窓の長さ |
| PRESENTER（process） | process の `present_tick` が `DTB_PRESENT_WINDOW_MS`（1000 ms）以内 | 窓を出た | 同上 |
| TTY（process） | `controlling_tty != NULL` かつ GPU（process）でない | どちらかが崩れた | — |

#### 4.4.2 全体の状態（HID）

| 状態 | 条件 | 効き |
| --- | --- | --- |
| ACTIVE | 最後の入力から `DTB_USER_ACTIVE_MS`（5 s）以内 | 全部の規則。周期 `DTB_PASS_MS`（100 ms） |
| QUIET | 5 s〜60 s | 全部の規則。周期 100 ms |
| AWAY | 60 s 以上入力が無い（起動の直後も） | 規則 R6（terminal）を止める。周期 `DTB_PASS_AWAY_MS`（1000 ms） |

入力の宛先が分からないので、HID は「人が見ているか」にだけ使う（Q302-4）。

#### 4.4.3 規則と強さの順（Q302-1 の決定 A）

上から最初に当たった規則が core の希望を決める。priority は表の列（§4.7 の段）。

| 順 | 規則 | core の希望 | 強さ | priority |
| --- | --- | --- | --- | --- |
| R1 | nice < 0 | FAST | PLACE | nice の段（8 か 9）。R4・R5 の段と比べて高い方 |
| R2 | nice > 0 | EFFICIENT | PLACE＋NO_OVERFLOW | nice の段（11 か 12） |
| R3 | BUSY | EFFICIENT | PLACE | 10（上げを外す） |
| R4 | PRESENTER の process | FAST | PLACE | 8（HEAVY なら 10） |
| R5 | GPU（process） | FAST | PLACE | 9（HEAVY なら 10） |
| R6 | TTY、かつ状態が AWAY でない | FAST | PLACE_IDLE | 10 |
| R7 | 他 | ANY | — | 10 |

- 強さ: **PREFER** は sched が置き場所と盗みで好むだけ（v1 の規則では使わず、口だけ置く）。**PLACE** は DTB が違う種類の core に居る thread を寄せる（行き先はその種類の idle の CPU だけ、§4.6.3）。**PLACE_IDLE** は PLACE と同じだが、行き先に SMT の兄弟も idle の CPU を求め、移動の予算を R1〜R5 の後に使う（P の席を UI に先に回す。ユーザーの「UI の応答を落とさない範囲で」の読み）。**NO_OVERFLOW** は違う種類の idle の CPU に盗ませない（飢えの防ぎの待ちの上限 §4.7 を超えたら盗ませる）。
- 新しい thread と fork の子は作り手の thread の hint と base の priority を継ぐ（§4.6.4）。terminal の shell から起こした command は最初から shell の hint で置かれる（review M9 の答え）。
- R3 が R4・R5 より上: GPU を使う process の中でも、syscall の無い計算の thread（game の worker、compiler の内部）は E に寄せる。GPU に仕事を出す thread は出すたびに syscall を通るので BUSY にならない。compositor の render・main の thread は fence の待ちと ioctl で BUSY にならない。
- R1 が R3 より上: nice < 0 は root が付けた急ぎの印（下げるのは root だけ、syscall.c:8936）。
- HEAVY の条件: GPU に仕事を出すだけで priority が上がると、CPU を使い続ける app が他の user の thread を 100 ms ずつ待たせる（review M8）。上げは「CPU を半分も使わない thread」だけにする。core の希望（FAST）は HEAVY でも残す。GPU を開けるのは seat の利用者と root だけ（§2.4）なので、上げを得るのはその利用者の process に限られる。
- **ユーザーの terminal の規則との差**（Q302-1 に明記）: 決定の A では、terminal の中でも syscall の無い計算（compiler）は E に寄せ、terminal の規則は「兄弟も空いた P があり、人が見ている時に寄せる」になる。これはユーザーの「UI の応答を落とさない範囲で」の解釈で、Q302-1 の案 E（terminal を BUSY より上、ただし PLACE_IDLE）を選ぶと compiler も空いた P に寄せる。
- 選ばなかった順（Q302-1 の案）: B「GPU・PRESENTER を BUSY より上」（game の worker が P に残り、電力が下がらない）、C「UI を nice より上」（利用者の明示の意図に反する）、D「TTY（PLACE）を BUSY より上」（terminal の build が P に張り付き、P が 4 つの 5330 で UI の席が無くなる）。

### 4.5 sched への hint の口（kernel の中、HAL を変えない）

案の全文は [proposed/include/kern/sched-hint.h](proposed/include/kern/sched-hint.h)。要点:

```c
#define SCHED_HINT_CORE_ANY		0U
#define SCHED_HINT_CORE_FAST		1U
#define SCHED_HINT_CORE_EFFICIENT	2U

#define SCHED_HINT_PREFER		1U	/* placement and stealing only */
#define SCHED_HINT_PLACE		2U	/* sched_hint_place() moves it to an idle CPU of the kind */
#define SCHED_HINT_PLACE_IDLE		3U	/* the same, and the CPU's SMT sibling idle too */
#define SCHED_HINT_NO_OVERFLOW		0x1U	/* flag: other kinds steal it only after the starvation limit */

struct sched_hint {
	uint8_t core;		/* SCHED_HINT_CORE_* */
	uint8_t strength;	/* 0, SCHED_HINT_PREFER, _PLACE or _PLACE_IDLE */
	uint8_t flags;		/* SCHED_HINT_NO_OVERFLOW */
	uint8_t priority;	/* the base level, SCHED_PRIORITY_DEFAULT .. SCHED_PRIOR_LOW for a user thread */
};

void sched_cpu_classes_set(const struct sched_cpu_class *classes, unsigned count);	/* thread context */
int  sched_hint_set(struct thread *thread, const struct sched_hint *hint);			/* thread context */
int  sched_hint_place(struct thread *thread);	/* 0, EALREADY, EAGAIN, EBUSY, ESRCH, EINVAL */
void sched_hint_activity(const struct thread *thread, struct sched_hint_activity *activity);
void sched_note_syscall(void);			/* the syscall entry, interrupts masked */
void sched_hint_counters(struct sched_hint_counters *counters);
```

- `sched_cpu_classes_set`: 表を CPU ごとの field に写す。表が無い（呼ばれていない）か、FAST と EFFICIENT がそろわなければ、core の希望は全部 ANY と同じに扱う（sched の振る舞いは priority 以外いまと同じ）。
- `sched_hint_set`: thread の居る CPU の lock を、`sched_wakeup` と同じ「読み直して確かめる」loop で取り（:531〜540 の形）、hint と `base_priority` を置く。process0 の thread（kernel の thread）・idle・NEW・EXITING 以降の状態と、affinity が `SCHED_CPU_BOUND` の thread は ESRCH（BOUND の thread の段も core も変えない）。**queue_kind が RUN か WOKEN の時だけ**新しい段に入れ直す（`queue_remove_thread` → `queue_append`）。SLEEP の時は field だけを変える（sleep の queue は段で分けない。入れ直すと時間つきで眠る thread が起きてしまう、review M2）。走っていて priority が下がり、上の段に待ちがあれば `preempt` を立てる。上げを受けている（`raised`）間は `priority` を変えず、次の戻しで新しい base になる。
- `sched_hint_place`: 希望の種類の CPU に居なければ寄せる。走っていない thread は §4.6.3 の選びで行き先を決めて §4.6.7 の直した `sched_set_cpu`。走っている thread は `move_pending` を立てて居る CPU に `notify_cpu`、その CPU が §4.6.5 の手渡しをする。戻り: EALREADY（既にその種類）、EAGAIN（その種類の idle の CPU が無い）、EBUSY（移動の途中・手渡しの席がふさがり）、ESRCH（動かせない状態、BOUND の thread）、EINVAL（PLACE・PLACE_IDLE の無い hint）。眠らない（CPU の lock と `hal_task_transfer` だけ）。

### 4.6 sched の変更（最小、全部 `#if CONFIG_DTB`）

#### 4.6.1 priority の 2 つの値と kernel の中の thread

`struct sched` に `base_priority`（hint の段）を足し、thread を作る 3 か所（thread.c:104・190・410）と `kthread_create`（:256）で `priority` と同じ値に初める。`priority`（今の field、queue の添字）は普段 `base_priority` と同じで、次の 2 つだけが一時に変える:

1. 飢えの防ぎ（§4.7）が 1 回だけ上げる（`raised = 1`）。
2. **kernel の中の thread の優先**（review M7）: user の thread を queue に入れる時、`accounting_kernel_depth != 0`（kernel の中で lock を持ちうる）で `base_priority` が `SCHED_HINT_USER_DEFAULT`（10）より下なら、10 で入れる。nice で下げた thread が kernel の lock を持ったまま待たされ、前面の process がその lock で待つ逆転を短くする。

戻しは queue に入れる全部の所で行う: `sched_add` :468、`sched_yield` の入れ直し :776、`sched_preempt` の前への戻し :877、`wake_enqueue_locked` :2051、盗みの入れ（`steal_runnable` の最後）、`sched_set_cpu` の入れ直し（元の CPU と行き先の 2 か所）、手渡しの入れ（§4.6.5）。共通の小さな関数 `queue_level_locked(thread)` にまとめる。

#### 4.6.2 CPU の種類の表

`struct sched_cpu` に `class`（§4.5 の `struct sched_cpu_class` の写し）。読みは lock 無しの atomic の load（古い値は 1 回の好みの外れで済む）。

#### 4.6.3 行き先の選び（純な関数、host 試験の対象）

`sched_hint_choose(classes, count, idle, backlog, core, mode)`、`idle`・`backlog` は `KERN_CPU_MASK_WORDS` 語の bit の列。mode は 3 つ（review M3）:

- **MOVE**（`sched_hint_place` の PLACE）: 希望の種類で usable で idle の CPU だけ。同じ種類の中では rank の大きい順、同じなら番号の小さい順。無ければ無し（EAGAIN）。違う種類へは動かさない（E → E・P → E の無駄な移動を作らない）。
- **MOVE_IDLE_CORE**（PLACE_IDLE）: MOVE に加えて SMT の兄弟も idle の物だけ。
- **PLACE**（新しい thread・tick の助け）: FAST は、兄弟も idle の FAST → idle の EFFICIENT と兄弟が忙しい idle の FAST を rank・sibling_rank の大きい順 → 無し（今の選びに任せる）。EFFICIENT は idle の EFFICIENT → 無し。

`rank` の値は §4.8。

#### 4.6.4 置き場所・盗み・idle の CPU の起こし

| 所 | 今 | 変更 |
| --- | --- | --- |
| 新しい thread（`sched_prepare_thread` :398） | 作り手の CPU、backlog があれば idle の CPU（番号順） | 作り手の thread の hint と `base_priority` を継ぐ（同じ process の thread も fork の子も）。作り手の CPU に backlog がある時だけ、§4.6.3 の PLACE の順で idle の CPU を選ぶ。作り手の CPU で済む普通の道は変えない |
| tick の助け（:1125〜1137） | 番号の小さい idle の CPU を 1 つ起こす | 待ちの先頭の thread の希望の種類の idle の CPU を先に。無ければ今どおり |
| 盗み（`take_stealable_locked` :2624） | 上の段から、BOUND の thread を飛ばし（`first_free_thread`）、同じ tick の thread は取らない | 同じ段の中で、盗む CPU の種類を望む thread を先に取る。NO_OVERFLOW の thread は、違う種類の CPU には待ちが飢えの上限を超えるまで渡さない。PLACE EFFICIENT（BUSY）の thread は、idle の FAST の CPU が盗んでよい（**溢れ**、Q302-2 の決定 A）。溢れた thread は DTB が E に idle が出来た時だけ戻す（MOVE は idle の CPU だけを行き先にするので、走っている E へ押し込んで盗み返される行ったり来たりは起きない） |
| 起床（`sched_wakeup` :520） | 居る CPU に戻す | **変えない**（起床の道に移動を足さない。種類の違いは DTB の周期の移動で直す。理由は §13） |

#### 4.6.5 走っている thread の手渡し（handoff）

BUSY の計算の thread は眠らず、CPU に他の仕事が無ければ quantum の終わりにも自分に戻るので、`sched_set_cpu` では一生移せない。今の `retired` の形（§2.1）にならい、去った後に後継が移す:

1. `sched_hint_place` は thread の CPU の lock の中で状態を見る。RUNNING なら `move_pending = 1` を立てて lock を離し、その CPU に通知する。RUNNABLE・SLEEPING なら lock を離して §4.6.7 の道へ（その道が lock の中で状態を確かめ直す）。
2. その CPU の `sched_cpu_notify`（:1790）か tick（`sched_clock_cpu` の終わり、`sched_yield` を呼ぶのと同じ所）が、current に `move_pending` があり `preempt_count == 0` で、`cpu->handoff` が空なら `sched_handoff()` を呼ぶ。`cpu->handoff` がふさがっていれば何もしない（`move_pending` は残り、次の tick で試す。**席は 1 つで、上書きしない**、review M1）。
3. `sched_handoff()`（`sched_yield` の形）: CPU の lock の中で §4.6.3 の MOVE（PLACE_IDLE なら MOVE_IDLE_CORE）の選びで行き先を決める。無ければ `move_pending` を消して戻る。あれば current を RUNNABLE・`need_migrate = SCHED_MIGRATING`・queue に入れずに `cpu->handoff`・`cpu->handoff_target` に置き、`move_pending` を消し、次の thread（無ければ idle）に切り替える。
4. 後継は `complete_retired` を呼ぶ 4 か所と、**tick の頭**（`sched_clock_cpu` の最初、どの切り替えの判断より前）で `complete_handoff(cpu)` を呼ぶ: `cpu->handoff` を取り出して空にし、`hal_task_transfer(task, target)`（去った task は止まっている）→ `retarget_migrating`（:2533）→ 行き先の lock で `queue_level_locked` の段の `run[]` に入れ `need_resched`。`SCHED_WAKE_PENDING` は RUNNABLE の thread には意味が無いので消すだけ → `notify_cpu(target)`。
5. 断られた（`HAL_ERR_BUSY` など）・行き先が offline になった時は、次の tick で 1 回だけ試し直す（`cpu->handoff` を元の CPU に残す。その間、新しい手渡しは 2 の規則で待つ）。それでも駄目なら元の CPU の run queue に戻し（`need_migrate = 0`）、counter に数える。
6. 手渡しの間の他の道: thread は RUNNABLE なので `sched_wakeup` は `SCHED_WAKE_PENDING` を立てるだけ（:545、移動の終わりで消す）。`sched_interrupt` の通知は元の CPU に行くが害は無く、thread は行き先で走り始めた時に user への戻りで signal・stop・終了の頼みを見る（process の stop は全 thread の安全な点を待つ形なので、手渡しの間は 1 ms 程度遅れるだけ）。`sched_set_cpu`・盗みは `need_migrate != 0` を見て触らない（:1721・:2496）。走っていない thread は exit・刈り取りにならない。
7. `move_pending` の後始末（review M10）: current が queue に入らずに切り替わる所（`switch_without_enqueue` :2307、眠る・止まる時）と、`sched_yield`・`sched_preempt` で current を queue に戻す時に消す。thread が走っている間だけ生きる印にし、古い印で後から手渡しが起きない。走っていない thread は DTB の次の周期の `sched_hint_place` が `sched_set_cpu` の道で移す。

新しい task の入口から始まる後継は `complete_retired` を呼ばない（:960 の comment）。その時の手渡しは次の tick（1 ms 以内）の頭の `complete_handoff` が片付け、その間の新しい手渡しは 2 の規則で待つので、席が上書きされる事は無い。

#### 4.6.6 計数

`sched_hint_counters`: hint の変更の数、`sched_hint_place` の移動の数・EAGAIN の数、手渡しの数・試し直し・失敗、飢えの防ぎの上げの数、kernel の中の優先の数。CPU ごとの field に足し、読む時に合わせる（lock 無し、近似でよい）。

#### 4.6.7 動かす前の状態の確かめ（review B2、第 3 版で置き場所を変えた）

- 今の `sched_set_cpu`（:1721）は状態の検査が lock の外で、RUNNING・ZOMBIE・DEAD だけを断る。NEW の thread は移せ（task を移し `sched.cpu` を行き先にするので、後の `sched_add` は行き先の CPU に入れる）、cpuperf.c:3590 と intel-tco.c:1106 が `kthread_create` の直後の NEW の thread を BOUND にするのに使っている。だから `sched_set_cpu` に「NEW を断る」確かめを足すと今の呼び手が壊れる（第 2 版の §4.6.7 の誤り）。
- DTB の道（`sched_hint_place` の走っていない thread）は、`sched_set_cpu` を直に呼ばず、sched.c の中の `hint_move_locked` 相当の道を使う: 元の CPU の lock を取った後に `state` が RUNNABLE か SLEEPING で、`queue_kind` が RUN・WOKEN・SLEEP のどれかで、`affinity` が FREE で、`need_migrate == 0` である事を確かめ直し、違えば ESRCH（移動の途中は EBUSY）。そこから先（queue から外す・印・transfer・retarget・行き先で入れる・持ち越しの起床）は `sched_set_cpu` と同じ順。`CONFIG_DTB=n` の kernel を今と同じに保つため、`sched_set_cpu` を分けて共通にはせず、`#if CONFIG_DTB` の中の別の static 関数に書く（重なりは約 60 行。DTB の外で同じ確かめが要る時が来たら、その時に `sched_set_cpu` と一つにする）。
- NEW の user の thread は `sched_prepare_thread` が置き場所を決め（§4.6.4）、`sched_add` までは DTB に見えても ESRCH。REAPING・ZOMBIE・DEAD・EXITING は task が壊れうるので断る。移動の印（`need_migrate`）を立てた後の thread は走れないので、移動の間に ZOMBIE・REAPING にはならない。DTB は thread の参照（`thread_ref`）を持って呼ぶので struct は消えない。
- `sched_set_cpu` そのものの lock の外の検査（REAPING を断らない）は、今の呼び手が自分で作った NEW の kernel の thread だけを渡すので表に出ない。直すかは DTB の範囲の外（Q1 に報告する）。

### 4.7 nice と priority（Q302-3）

- 前提（review B1）: kernel の thread の大半は 8（§2.1）。厳密な順の下で user の thread を 8 より上に置くと、USB の HID の decode・xHCI・pageout などの kernel の worker を待たせる。だから **DTB は user の thread を 8 より上に置かない**。代わりに、DTB が on の時の user の既定の段を 10 に下げ、上げは 8・9 の間で行う。
- 決定の段（Q302-3 の A、DTB が on の時だけ）:

| 対象 | base の priority |
| --- | --- |
| kernel の thread | 変えない（今のまま、多くは 8） |
| user の thread の既定（R3・R6・R7、HEAVY の R4・R5） | 10 |
| PRESENTER（R4、HEAVY でない） | 8 |
| GPU（R5、HEAVY でない） | 9 |
| nice -20〜-10 | 8 |
| nice -9〜-1 | 9 |
| nice 1〜9 | 11 |
| nice 10〜20 | 12 |

  R1 の thread は nice の段と R4・R5 の段の高い方。R2（nice > 0）は上げを受けない。DTB が off（`dtb=off`）と `CONFIG_DTB=n` では全部 8 のまま（今と同じ）。新しい thread は作り手の段を継ぎ（§4.6.4）、DTB の最初の周期の後に表の段になる。
- これで変わる事（Q302-3 で明記）: DTB が on の時、kernel の既定の thread（8）は user の既定（10）より先に走る。今は同じ段で順番に走る。kernel の thread が長く忙しい場面（GPU の shader の compile が kernel の thread で走る時など、未確認）で user の thread が待たされうるので、飢えの防ぎ（下）を user の全部の段に効かせ、p004 の QEMU と p006 の 5330 で確かめる。
- **飢えの防ぎ**（sched の側、`#if CONFIG_DTB`）: tick ごとに、その CPU の、今走っている thread より下の段の queue の先頭を見て（段ごとの先頭だけ、最大 16）、user の thread で `queued_tick` から `SCHED_HINT_STARVE_MS`（100 ms）以上待った物を、`max(走っている thread の段, SCHED_PRIORITY_DEFAULT)` の段（数が小さい方が高い。8 より高くはしない）の `woken[]` に 1 回だけ上げ（`priority` だけを変え `raised = 1`、§4.6.1）、`preempt` を立てる。kernel の thread は対象にしない（今の振る舞いを保つ）。
  これで、下げた user の thread は 100 ms ごとに 1 quantum（10 ms）は走る（全部の CPU が上の段の忙しい thread で埋まった最悪の時、CPU の約 9%）。nice の「重み」の意味（Unix の分け前）とは違い、段＋上限の近似。
- 選ばなかった案: (a) user の thread を 6・7 に上げる（第 1 版。kernel の worker を待たせる、review B1）、(b) nice をそのまま 16 段に写す（kernel の段に入り、飢えが深い）、(c) sched を重み（分け前）の方式に作り直す（範囲の外、ws.md「sched の作り直しは含まない」）、(d) 飢えの防ぎを DTB の周期に任せる（DTB の thread が遅れると防ぎが止まる）、(e) 上げをせず、下げ（BUSY・nice > 0）だけで GPU の app を相対に上にする（Q302-3 の案 B。user の既定は 8 のまま。nice < 0 が priority に効かず、2026-10-27 の決定の「nice は priority にも」を半分しか満たさない）。
- nice の変更が効くまで: 次の DTB の周期（最大 100 ms、AWAY では 1 s）。`setpriority` から DTB を起こす口は足さない（syscall の道を DTB に繋げない）。

### 4.8 5330（P 2・E 8）の割り付けの方針

- CPU の数: P-core 2（SMT で論理 4）・E-core 8（4 つ組 2 つ、SMT 無し）、計 12（WS333 design §3）。番号の並び（P が先か）は未確認（§12）。DTB は番号に頼らず、cpuperf の `core_class` と `core`（SMT の兄弟）で表を作る。
- `rank` の決め方: `relative` が分かれば（`CONFIG_KERN_THERMAL=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y` で HWP の strategy の後、5330 の見込み P 100・E 75）`rank = relative × 10`。兄弟が忙しい P は `sibling_rank = rank × DTB_SMT_SHARE_PERMILLE / 1000`（初め 600、p006 で測って直す）。`relative` が UNKNOWN なら種類だけで P 1000・E 700・兄弟の忙しい P 600 とおく（E の idle が P の兄弟の忙しい CPU より先、という順だけが効く）。HFI の `available` が 0 の CPU は `usable = 0`（熱で避けろと言われた CPU）。
- 定まった姿（期待、p006 で確かめる）:

| 場面 | P（論理 4） | E（8） |
| --- | --- | --- |
| desktop が何もしない | compositor の thread（眠っている） | 何もしない |
| terminal で打つ | terminal の app（GPU）・compositor・shell（R6、兄弟も idle の P がある時） | — |
| game（GPU）＋compositor | game の main と compositor の render を別の P-core に（兄弟の idle を先に） | game の worker（BUSY） |
| build（-j12）だけ | 溢れた compiler（idle の P が盗む） | compiler 8 |
| build＋GPU の app を使う | GPU の app（9）・compositor（8）が build（10）を横取り | compiler。押し出された compiler は idle の E か P の空きで |
| 背景の計算 4 本（S2） | 空く（P の電力が下がる） | 計算 4 |

### 4.9 DTB の周期の処理（dtb.c）

1. thread「dtb」（`kthread_create`、priority 5。kernel の既定の 8 より上で、短く走る）。`kern_dtb_init()` を `entry.c` の thermal の block（`#if CONFIG_KERN_THERMAL`、:385〜418）の後で、KERN_THERMAL に依らず呼ぶ。`dtb=off` の起動の parameter では作らない（計数は取り続ける。on と off の比べを同じ image でするため）。
2. 周期ごと（`kern_ms_to_ticks(DTB_PASS_MS)`、AWAY は `DTB_PASS_AWAY_MS`、`kern_sleep` で眠る）:
   1. （`CONFIG_KERN_THERMAL=y` の時）`kern_cpuperf_generation()` が前と違えば、全 CPU の `kern_cpuperf_cpu(cpu, 0, &info)`（flag 0: spinlock だけで読み CPU を起こさない）を読み、§4.8 の表を作って `sched_cpu_classes_set`。FAST と EFFICIENT が両方そろわなければ表は全部 ANY（priority の hint だけが効く）。
   2. HID の時刻から ACTIVE・QUIET・AWAY。
   3. process を巡る。`process_find_next_ref(cursor)`（process.c:773）は呼ぶたびに全 process を `process_tree_lock`（irqsave）の中で走査するので、1 周期で O(N²) になる（review m5）。N ≈ 数百で 1 周期の比べは 10 万程度、1 回の lock の中は N 程度。p004 で周期の時間を測り、1 ms を超えるなら process.c に「1 回の lock で pid と参照を最大 K 個取る」口を足す（kernel の中の変更、p004 の範囲）。process0 と終わりかけの process は飛ばす。nice・tty・GPU と表示の時刻を読み、`process->lock` の中で thread を `thread_ref` して手元の配列（`DTB_THREADS_PER_PROCESS` 64）に取り、lock を離す。64 を超える process は、前の周期に扱った最後の tid より大きい tid の thread から 64 本を取る（tid の cursor。列の位置に頼らない、review m13）。
   4. thread ごとに `sched_hint_activity` を読み、前回の値（`struct dtb_thread_state`、DTB だけが書く）と比べて印を更新し、`dtb_policy_classify()` で hint を作る。前と違えば `sched_hint_set`。PLACE・PLACE_IDLE で希望と違う種類の CPU に居り、前の移動から `DTB_MOVE_INTERVAL_MS`（500 ms）経ち、この周期の予算（移動 `DTB_MOVES_PER_PASS` 32、うち手渡し 8）が残れば `sched_hint_place`。予算は R1〜R5 の thread に先に使い、R6 は残りだけ（R6 の候補は配列に残して周期の終わりに回す）。`thread_release`。
   5. 周期の時間を計数（最大と合計）に足す。
- 起きる回数: ACTIVE・QUIET で 1 秒 10 回、AWAY で 1 回（tick の 1 秒 1000 回×12 CPU に比べて小さい）。

### 4.10 状態遷移

DTB の全体:

```
 (built n) ──── 何も無い
 (built y) ── 起動 ──> dtb=off ── 計数だけ（hint は無い、sched の振る舞いは priority も含めて今と同じ）
                 └──> RUNNING: AWAY ⇄ QUIET ⇄ ACTIVE（入力の時刻で）
                        └─ 種類の表: NONE（hybrid でない・未登録）⇄ HYBRID（世代が変わるたびに作り直す）
```

thread の hint（DTB が見る印の組で決まり、周期ごとに変わりうる）: R7（ANY）→ R5（GPU）→ R3（BUSY）→ R5 … 。移動は印が変わった時と、PLACE で種類が違う時だけ。

手渡し（sched、1 つの thread）:

```
 RUNNING ──place──> RUNNING+move_pending ──notify/tick（席が空き）──> HANDOFF（RUNNABLE, MIGRATING, cpu->handoff）
   ^                     |  行き先無し・眠る・queue に戻る: 印を消す                | 後継か tick の頭の complete_handoff
   |                     v                                                          v
   +─────────────── RUNNING / SLEEPING / RUNNABLE                       RUNNABLE on target（移動の終わり）
                                                                          | transfer 失敗 2 回: RUNNABLE on old
```

### 4.11 資源と寿命、config、起動の parameter

- 資源: DTB の thread 1 本、CPU の種類の表（`kern_calloc`、CPU 数、kernel の寿命）、thread の参照の配列（64、static）。thread と process の field は各 struct に埋める（寿命は持ち主と同じ）。DTB が持つ参照は 1 周期の中で離す。
- config（第 3 版、新しい mk。Makefile・mk は Claude が変えないので、次は**案**。承認されるまで当てない）:
  - `mk/config.mk` に `CONFIG_DTB ?= n`（既定 off、2026-10-27 の決定）。説明の行に「core の種類は thermal の core（`CONFIG_KERN_THERMAL`）、x86 では加えて `CONFIG_DRIVER_X86_POWER_CONTROL` から。無ければ priority の hint だけ」を書く。依存（`Depends:`）にはしない（KERN_THERMAL 無しでも nice と GPU の app の priority は効く）。
  - `mk/kernel.mk` に `ifneq ($(filter y,$(CONFIG_DTB)),)` → `KERN_CPPFLAGS += -DCONFIG_DTB=1`、`KERN_SOURCES += src/kern/dtb.c`（p002）。p004 で `src/kern/dtb-policy.c` を足す（その時に案を出し直す）。
  - platform の制限は置かない（全 platform で build できる。hybrid の機械は今 5330 だけで、他では種類の表が ANY）。
  - `include/kern/dtb.h` は `CONFIG_DTB` が定義されていなければ 0 とする（n の時 mk は macro を定義しない）。
  - 名前は `CONFIG_DTB` のまま案を出す（D-13: `CONFIG_KERN_DTB` に揃えるか、device tree の dtb と紛れない名前にするかはユーザーに聞く）。
  - 差分の全文は p002 の [phase.md](phase002/phase.md) の「mk の案」。
- 試験の config（p003 で案を出す、review M6）: `mk/test-config.mk` に `CONFIG_DTB_TEST ?= n`、`mk/test-kernel.mk` に `-DCONFIG_DTB_TEST=1` と `src/kern/sched-hint-test.c`。`sched-hint-test.c` は、起動の parameter `dtb.classes=PPEE…`（hybrid でない QEMU で種類の表を偽る）と `dtb.stress=1`（10 ms ごとに無作為の thread に無作為の hint と `sched_hint_place` を出す。手渡し・移動・段の付け替えの競争の試験）を読む。p004 の後は DTB の本体も同じ偽の表で動く。
- 起動の parameter（製品）: `dtb=off`（診断の switch、方針ではない）。`boot.h` の enum と `boot.c` の名前の表と値の検査の表に足す（WS333 の `cpuperf=off` と同じ形）。`dtb.classes=` は値の形が違うので自分の解析を持つ（`CONFIG_DTB_TEST` の時だけ）。

### 4.12 並行性と lock

```
 DTB の thread:  process_tree_lock（irqsave、process_find_next_ref の中だけ）
                 process->lock（rank 20、thread_ref を取るだけ）→ 離す
                 → sched_hint_set / sched_hint_place（thread の CPU の lock、rank 200）
                 → kern_cpuperf_cpu（box の spinlock、rank 125）は sched の lock を持たずに
 sched_hint_place（走っていない）: sched_set_cpu の今の lock の順（元の CPU → 離す → transfer → 元の CPU → 離す → 行き先）、確かめ直しは元の CPU の lock の中
 手渡し:          元の CPU の lock の中で印と行き先 → 離して切り替え → 後継: transfer → retarget（元の lock）→ 行き先の lock
 計数:            tick（CPU の lock の中）、syscall の入口（割り込みの窓）、RUNNABLE にする所（CPU の lock の中）、note_gpu（lock 無し、atomic）、note_input（input の device の lock の中、atomic だけ）
```

- 2 つの CPU の lock を同時に持たない（sched の今の規則を保つ）。
- DTB の読みは近似でよい（古い値は 1 周期の遅れ）。
- `struct dtb_thread_state` は DTB の thread だけが書く（1 本しか無い）。

### 4.13 割り込みと DMA

- 新しい割り込み・DMA は無い。`kern_dtb_note_input()` は input の report を配る文脈（割り込みの下半分か driver の thread。irqsave の lock の中）から呼ばれうるので、atomic の store と加算だけにする。手渡しは既存の CPU の通知（`hal_cpu_notify`）を使う。

### 4.14 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| `kthread_create` の失敗 | log 1 行、DTB は無いまま（計数は取る）。sched は hint が無いので今と同じ |
| cpuperf の provider が無い・hybrid でない | 種類の表は ANY。priority の hint だけ |
| `kern_cpuperf_cpu` が EINVAL | その CPU を ANY |
| `sched_hint_place` の EAGAIN・EBUSY・ESRCH | 動かさない。次の周期に試す（`DTB_MOVE_INTERVAL_MS` を守る） |
| 手渡しの transfer の失敗 | §4.6.5 の 5（1 回試し直し、元に戻す） |
| HFI が P を避けろと言う（熱） | P の `usable = 0`。FAST の thread は新しい thread の置き場所で E の rank の順に置かれる。MOVE は行き先が無いので EAGAIN |
| CPU が offline（system の sleep） | `cpu_online` を見て行き先にしない。sleep の間 DTB の thread は眠る |
| thread・process が周期の途中で終わる | 参照で struct は残る。`sched_hint_set`・`sched_set_cpu` は lock の中の確かめで ESRCH |
| kernel の thread が長く忙しい（user が 10 に下がった時） | 飢えの防ぎで user は 100 ms ごとに 10 ms 走る。p004・p006 の計数（上げの数）で見る |

### 4.15 観察（Q302-6）

- 5330 の評価には「どの CPU がどの種類か」と「DTB が何回寄せたか」を user から読む口が要る（ログで判定しない規則、AGENTS.md §9）。推し: 読みだけの sysctl `kern.dtb`（`KERN_DTB 11`、`include/uapi/sysctl.h` の KERN の次の番号）。形の案は [proposed/include/uapi/dtb-sysctl.h](proposed/include/uapi/dtb-sysctl.h)（version・struct_size・flag（built・running・hybrid・状態）、周期の数と時間の最大と合計、規則ごとの thread の数、`sched_hint_counters` の全部、CPU の数と CPU ごとの種類・usable・rank・core_group、予備）。process ごと・thread ごとの印は v1 に入れない。
- 書きの口（on・off の切り替え、thread の自分の hint、前面の app）は足さない（Q302-4・Q302-6）。

### 4.16 既存の framework との関係

- **cpuperf（WS333）**: 読むだけ（§2.5）。DTB は周波数・strategy に触らない。thermal の受け手にもならない。HFI の値は熱で P を避ける時に効く（§4.14）。
- **thermal**: 関係なし。
- **drv_gpu（gpu.c）**: `gpu_ioctl` の成功の後に 1 行（§4.3）。GPU の driver（i915 など）は変えない。
- **input（input.c）**: 普通の入力の 2 つの関数に 1 行ずつ。
- **cdev・PCI・USB**: 関係なし。
- **HAL**: §5。
- **Keiland（WS309）**: 変えない。thread の台帳（`kwl_thread_start`）は、将来 thread の自分の hint（UAPI）を足す時の置き場所になる（Q302-4 の条件つきの Phase）。

## 5. HAL の検討（変えない）

| 要る事 | 使う口 | 足りる理由 |
| --- | --- | --- |
| 止まっている thread を別の CPU へ | `hal_task_transfer`（hal.h:1041） | `sched_set_cpu`・盗みと同じ使い方 |
| 走っている thread を別の CPU へ | 普通の切り替え（`hal_task_context_switch`）の後に `hal_task_transfer` | 走っている task を HAL に移させない。sched が先に切り替えて task を止め、後継が移す（§4.6.5）。後継の文脈で去った task が止まっている事は x86_64 の実装（`amd64_task_finish_switch`、src/hal/x86_64/task.c:673）が保証し、arm64（task.c:625）・riscv64（task.c:627）の実装も同じ性質を comment に書いている。今の盗みが同じ性質に頼っている |
| CPU に手渡しを知らせる | `hal_cpu_notify`（sched の `notify_cpu` :2290） | 既存 |
| core の種類・相対・SMT | cpuperf（kernel の口）と x86 の driver（CPUID） | HAL を通らない |
| 時刻 | sched の tick、`kern_ms_to_ticks` | 既存 |

- `hal.h:1041` の `hal_task_transfer` の宣言には契約の文が無い。上の性質は x86_64・arm64・riscv64 の実装と盗みの前提であって、hal.h に書かれた約束ではない（review m2）。手渡しが起きるのは種類の表に FAST と EFFICIENT がそろう機械（今は x86 の hybrid だけ）なので v1 には足りる。契約の文を hal.h に足すのは HAL の API の変更（文の追加でも承認が要る）なので、この設計では足さない（§12 の 3）。
- HAL の差分の案は無い（`plan/ws302/proposed/` に hal.h は置かない）。

## 6. 試験の計画

### 6.1 host 試験（`plan/ws302/tests/dtb-host/`、p003・p004 で作る）

- 分類（dtb-policy.c を host で compile、p004）: 規則の順 R1〜R7 の全部の組（nice -20・-10・-1・0・1・10・20 × BUSY × HEAVY × PRESENTER × GPU × TTY × 状態）の表を期待の表と照らす。BUSY・HEAVY の履歴の幅（200 ms で立ち、20 ms 未満が 10 周期で消える、途中の揺れで消えない）。GPU・表示の窓の端。user の段が 8 より上にならない事。
- nice の段（-20〜20 の全部）と R1 の高い方の規則（p004）。
- 行き先の選び（`sched_hint_choose`、p003）: 5330 の形（P 論理 4・E 8）と偽の形（PPEE、全部 P、全部 UNKNOWN、P が usable 0）で、idle と backlog の bit の組を回し、MOVE が違う種類を返さない事、MOVE_IDLE_CORE が兄弟の忙しい CPU を返さない事、§4.6.3 の順と一致。
- 飢えの防ぎと kernel の中の優先の模型（p003）: queue の先頭の待ちと段の組で、上げる・上げない、上げる段が 8 より上にならない事。
- 突然変異（比べの向き・閾値を 1 つずつ壊して検出する事）を各試験の README に書く。

### 6.2 QEMU（amd64、Q1 が割り当てた名前と port、判定は SSH と boot-test.sh。console・serial の log では判定しない）

1. `CONFIG_DTB=n` の image の起動（boot-test.sh の login）。
2. `CONFIG_DTB=y` の image の起動（同）と、`dtb=off` の起動。
3. （p003）`CONFIG_DTB=y CONFIG_DTB_TEST=y`、SMP 4、`dtb.classes=PPEE dtb.stress=1` で、`plan/tools/kstress/` の iostress・credrace・sigexec と、thread の作成と exit・fork の嵐（試験の program、刈り取りと移動の競争、review B2）、時間つきの sleep を繰り返す thread（hint の段の付け替えで早く起きない事を時刻で確かめる、review M2）を 30 分。止まらない・panic しない（固まったら gdbstub と QMP で解析）、手渡しと移動の数 > 0、手渡しの失敗の数を記録。
4. （p004）同じ image で `dtb.stress` 無し: 忙しい loop 2 本（試験の program、syscall 無し）を走らせ、SSH で `hw.cputimes` の差と `kern.dtb` を読み、忙しい時間の 90% 以上が E（CPU 2・3）にある事と、手渡しの数 > 0 を確かめる。
5. （p004）nice: 忙しい loop 2 本（nice 0 と nice 10）を SMP 1 の QEMU で 20 秒。nice 10 の方の CPU 時間の割合が 5〜15%（飢えの防ぎの期待の約 9% の幅、review m9）。
6. （p004）kernel の thread との競争: `CONFIG_DTB_TEST` の試験の kernel の thread（priority 8 で 2 秒忙しい）と user の忙しい loop を SMP 1 で。user の CPU 時間の割合が 5〜15%（飢えの防ぎが user を救う）。

### 6.3 5330 の実機（1 本の script `plan/ws302/tests/5330-dtb.sh`: 配置 → 起動 → 実行 → 回収 → 要約）

- 最初に確かめる事（p006 の始め）: 5330 の CPU の番号の並びと SMT の兄弟の対（`kern.dtb`）、cursor の遅れの器具（WS075 の `plan/tools/i915-hw/hdmi/` の `measure-apps.sh`・`c6.py`）が今の 5330 で使えるか（使えなければ cursor の遅れは Keiland の `--log-frames` の入力から frame までの印で代える）。
- image: `plan/ws302/tests/config-x86_64-dtb-5330.mk`（`config/config.x86_64.mk` に `CONFIG_DTB=y`、`CONFIG_KERN_THERMAL=y`・`CONFIG_DRIVER_X86_POWER_CONTROL=y`（WS333 の前提、core の種類と相対の性能と RAPL の meter）、`CONFIG_INPUT_TEST_INJECT=y`）。比べは同じ image の `dtb=on`（既定）と `dtb=off` を交互に 5 回ずつ。`CONFIG_DTB=n` の image で負担を 5 回。
- 電力の器具: WS333 の RAPL の meter（ws333-p011）を `/dev/thermal` で読む（ws333-p005 の後半・ws333-p013 の後）。それが無い時は WS291 p022 の診断の patch（Q1、§10.2）。
- 場面（各 60 s、計測は後半 40 s）:

| 場面 | 作業 | 測る物 |
| --- | --- | --- |
| S1 何もしない desktop | — | package の電力、DTB の周期の時間 |
| S2 背景の計算 | 忙しい loop 4 本（nice 0、syscall 無し）。電力の上限に張り付かない量（review M4）。8 本の run も 1 回、記録だけ | package の電力（RAPL の PL の制限の印も記録）、loop の進み（1 秒あたりの回数）、CPU の種類ごとの忙しさ（`hw.cputimes`＋`kern.dtb` の種類）、cursor の遅れ、打鍵 → 表示（`plan/tools/frame-ops/key-latency.py`） |
| S3 terminal の build | 固定の C の source の tree を image の cc で `make -j12` | build の時間、BUSY の thread の数と移動の数（`kern.dtb`。実の compiler で BUSY が立つ割合、review m9） |
| S4 build＋GPU の app＋打鍵 | S3 ＋ mview（`plan/tools/gpu-perf/` の c600 の設定）＋ aat-input の打鍵 | mview の fps、frame の時間の中央値と p95（Keiland の `--log-frames`、`frame-times.py`）、打鍵 → 表示、build の時間 |
| S5 nice の背景 | 忙しい loop 8 本を nice 10、前で S4 の mview | mview の fps、loop の CPU 時間（0 でない事） |

- 同じ session で A/A（off と off）の揺れを 1 回測り、受け入れの「同等」の幅に使う。
- Keiland の `--log-frames`・`TERMINAL_PERF=1` の log は app の計測の出力で、kernel の console・serial の log ではない（AGENTS.md §9 の判定の規則に触れない）。

### 6.4 受け入れの数（Q302-7 の決定）

| # | 物差し | 合格 |
| --- | --- | --- |
| A1 | S2・S4 の cursor の遅れと打鍵 → 表示の中央値・p95 | on ≤ off × 1.05 ＋ 1 ms（A/A の揺れの方が大きければその幅） |
| A2 | S4・S5 の mview の fps と frame の時間の中央値・p95 | fps は on ≥ off × 0.95、frame の時間は A1 と同じ |
| A3 | S2 の忙しい時間の置き場所 | on で E の CPU に 90% 以上 |
| A4 | S2（4 本）の package の電力（40 s の平均） | on ≤ off × 0.95。off で P に載らなかった（off の E の割合が 90% 以上の）run は電力の比べから外し、その旨を記録する |
| A5 | S3 の build の時間 | on ≤ off × 1.10 |
| A6 | 負担: `CONFIG_DTB=n` と y の `dtb=off` の S1 の電力と S3 の build の時間、DTB の周期の時間の最大 | 差が 2% 以内（揺れの中）、周期の最大 < 1 ms |
| A7 | 安定 | §6.2 の 3 が 30 分 PASS、5330 の全部の run で固まり・panic 無し |

## 7. Phase の分け方と受け入れ（ws.md の表の本体はこの表。±50%、1 LW ≈ 実時間 15〜25 分）

| Phase | 内容 | 依存 | LW | 受け入れ | 担当の型 |
| --- | --- | --- | --- | --- | --- |
| p001 | この設計、design-reviewer、ユーザーの問い | — | — | review の反映、Q302-1〜7 の答え（ユーザーの review） | 設計（P5） |
| p002 | config（`CONFIG_DTB`、mk の案 §4.11）、`include/kern/dtb.h`・`sched-hint.h` の型、統計（thread の計数・syscall の入口・tick・RUNNABLE の所、process の GPU と表示の時刻、`src/kern/dtb.c` の `kern_dtb_note_gpu`・`kern_dtb_note_input` と gpu.c・input.c の通知）、`sched_hint_activity`、`base_priority` の初め | p001、Q302-5（決定済み） | 3 | x86_64・arm64（rpi5）・riscv64（vf2）の vmunix の build（n と y）warning 0（y は mk の案が当たるまで overlay で）。`CONFIG_DTB=n` が今と同じ事: 同じ config で main と branch の `vmunix` を作り、`llvm-objdump -d` の text が一致（LTO の後の最終の物で比べる。違いは build の印の文字列だけ、review M5）。QEMU の起動（y と n） | Opus（phase-runner-high） |
| p003 | sched の口: `sched_hint_set`・段の戻し・kernel の中の優先、種類の表、行き先の選び（純な関数と host 試験）、置き場所・盗み・tick の助け、`sched_hint_place`、hint の道の状態の確かめ直し（§4.6.7、`sched_set_cpu` は変えない）、走っている thread の手渡し、飢えの防ぎ、計数、`CONFIG_DTB_TEST` の偽の種類と stress | p002、Q302-2・Q302-3（決定済み） | 7 | host 試験（§6.1 の行き先・飢え）、build warning 0、QEMU §6.2 の 1〜3（30 分の stress を含む） | Opus（phase-runner-high） |
| p004 | DTB の本体: dtb.c・dtb-policy.c（分類・履歴・nice の段・HID の状態・周期・予算）、`dtb=off`、`kern.dtb`（Q302-6 の決定の形）、周期の時間が 1 ms を超えれば process の巡回の口 | p003、Q302-1・Q302-6（決定済み） | 6 | host 試験（§6.1 の分類・nice）、build warning 0、QEMU §6.2 の 2・4〜6 | Opus（phase-runner-high） |
| p005 | （条件つき）compositor からの前面の app の hint（UAPI・backend・WS309 の台帳）。Q302-4 で「作る」か、p006 で A1・A2 が通らず前面の情報で直る見込みがある時だけ立てる | p004、UAPI の決定 | 3 | — | — |
| p006 | 5330 の評価: `5330-dtb.sh`（§6.3）、閾値（`DTB_*`・`sibling_rank`・飢えの上限）の調整、5330 の CPU の番号の並びと器具の確かめ | p004、WS333 の meter と相対の性能（completed）、5330 の割当（Q1） | 4 | §6.4 の A1〜A7（Q302-7 の決定の数） | Opus＋Q1 の実機の割当 |
| p007 | 規約の全文の見直しと文書（`docs/architecture/` に DTB の説明） | p006 | 2 | 規約、文書 | conformance-reviewer・document-writer |

合計 約 22 LW（p005 を立てれば 25）。ws.md の旧い表（約 32 LW）は、thread の計数と sched の口を分け直し、WS301 の前提の Phase が WS333 で済んだので小さくなった。

## 8. 選ばなかった案（主な物）

| 案 | 選ばなかった理由 |
| --- | --- |
| DTB を sched の中に入れる（分類も sched がする） | ユーザーの決定（DTB は sched ではない）。sched の hot path に分類の代価が乗る |
| sched が cpuperf を直に読む | lock の rank（sched 200 の中で box 125 は取れない）と、sched を cpuperf に依らせる層の向き |
| DTB が行き先の CPU を決めて `sched_set_cpu` を直に呼ぶ | 決めてから移すまでに CPU の空きが変わる。行き先の選びは CPU の状態を見る sched の仕事 |
| 起床の時に種類の違う CPU へ移す | 起床の道（割り込みの文脈を含む）に移動を足すと hot path が重くなり、起床ごとに CPU の間の通知が増える。同じ priority 以上の起床は今も横取りで走れる。種類の違いは周期の移動で直す（最大 100 ms の遅れ） |
| 走っている thread を HAL に移させる（hal の契約を広げる） | §5 のとおり要らない |
| user の thread を kernel の既定（8）より上に上げる | kernel の worker を待たせる（review B1、§4.7） |
| MOVE の行き先に「backlog の無い（走っている）CPU」を許す | 走っている E に押し込んだ thread を idle の P が盗み返し、行ったり来たりする（review M3） |
| DTB を input の subscriber にする | kernel が driver の関数を呼ぶ向きになり、input.c の無い platform で link が切れる。枠（最大 8）も食い、外しと resync の report も来る |
| GPU の process ごとの busy の時間（完了の時刻から） | GPU の driver ごとに完了の道が違い、代価が大きい。提出と表示の時刻で v1 の規則は足りる |
| thread の名前（compositor の thread の種類）を kernel に足す | UAPI が要り、WS309 との調整も要る。v1 は kernel の統計だけで見分ける |
| 統計を常に取る（`CONFIG_DTB=n` でも） | n の kernel を今と同じに保つ方が受け入れ（A6）を示しやすい。Q302-5 |
| `sched_set_cpu` の確かめ直しを常に入れる | 外の呼び手が無い間は表に出ない欠けで、n の kernel を変えない方を取る。DTB の外で使う時が来たら常に入れる |

## 9. 出典と引用

- 外部の source・仕様は使っていない。値（閾値・段）は zedBSD の設計の初めの値で、p006 の実測で直す。source の comment に開発の管理情報を書かない（guardrail）。
- 5330 の CPU の数と HWP の能力は WS333 design §3（Debian での読み）から。

## 10. 判断が要る点

### 10.1 ユーザー（[user-questions.md](user-questions.md) に背景・用語・案）

2026-11-14 ユーザーから、Q302-1〜7 を全部推しの案で決める、という趣旨の答えを受けた。

| 問い | 要点 | 決定 |
| --- | --- | --- |
| Q302-1 | 規則の強さの順（ws.md の D2）。terminal の規則の読み | A: nice → 忙しい計算 → 画面を作る process → GPU の app → terminal（terminal は兄弟も空いた P だけ、人が 60 秒操作しなければ止める）。§4.4.3 |
| Q302-2 | 忙しい計算（nice 0）が空いた P に溢れてよいか | A: 溢れてよい（nice > 0 は飢えの上限を越えるまで溢れない）。「1 tick 以上待った」は単位では 1 ms だが、盗みが起きるのは idle の CPU が起きた時なので、今の amd64 の粒は約 10 ms。§4.6.4 |
| Q302-3 | priority の段、nice の段、飢えの防ぎ、GPU の上げの条件、DTB の on の時だけか | A: §4.7 の表（user の既定 10、上げは 8・9、kernel の 8 より上に置かない）、100 ms の防ぎ、CPU を半分以上使う thread は上げない、DTB が on の時だけ |
| Q302-4 | compositor から前面の app を知らせる口（D4） | B: v1 では作らず、p006 の数で要れば条件つきの p005 |
| Q302-5 | 統計を DTB の build の時だけ取るか（D5） | A: `CONFIG_DTB=y` の時だけ（`dtb=off` で起動しても取る） |
| Q302-6 | 観察の UAPI | A: 読みだけの sysctl `kern.dtb`（`KERN_DTB 11`、形は `proposed/include/uapi/dtb-sysctl.h`、p004） |
| Q302-7 | 受け入れの数 | §6.4 の表のとおり |

### 10.2 Q1

1. master.md の WS302 の行（「p001 検討（WS301 p001 の後）、約 32 LW」）を、この設計の Phase（約 22 LW、依存は WS333 の p011・p013）に直す。（第 3 版の時点で WS333 は completed。p006 の依存は WS333 の済んだ成果になった）
2. 共有の file の所有の調整（第 3 版で path を直した）: `src/kern/sched.c`・`include/kern/sched.h`・`include/kern/thread.h`・`src/kern/thread.c`・`include/kern/process.h`・`src/kern/process.c`（巡回の口が要る時）・`src/kern/syscall.c`・`src/kern/entry.c`・`include/kern/boot.h`・`src/kern/boot.c`・`src/drivers/gpu/gpu.c`・`src/drivers/generic/input.c`・`include/uapi/sysctl.h`・`src/kern/sysctl.c`。mk（`mk/config.mk`・`mk/kernel.mk`・`mk/test-config.mk`・`mk/test-kernel.mk`）は案だけ（ユーザーの承認）。
3. p006 の電力の器具: WS333 の RAPL の meter（`/dev/thermal`、completed）で読む。WS333 design の 5330 の PL1 の食い違い（§3 の 25 W と 13.0 W）は WS333 の記録の件として残る。
4. p006 の 5330 の割当。
5. 着手の順: 問いは決まったので、p002 → p003 → p004 の順に依存だけで進められる。
6. ws.md の「出典」と D1・D3 のユーザーの発言の原文（2026-10-27 の記録）を、AGENTS.md §8 の形（要点）に直すか（第 2 版からの持ち越し。担当は ws.md の原文を直していない）。
7. （第 3 版の新しい点）D-3: WS333 design §4.5 は「topology と core の種類は X86_POWER_CONTROL（旧 CPU_PERF）に依らず出す」と書くが、code は KERN_THERMAL と X86_POWER_CONTROL の両方が y の時だけ provider を登録する。DTB は 5330 の評価で両方を y にするので困らないが、記録か code のどちらを直すかはユーザー。
8. （第 3 版の新しい点）D-13: config の名前（`CONFIG_DTB` か `CONFIG_KERN_DTB` か別の名前か）。mk の案と一緒にユーザーへ。
9. （第 3 版の新しい点）`sched_set_cpu` の lock の外の状態の検査（REAPING を断らない）は、今の呼び手（自分で作った NEW の kernel の thread）では表に出ない。DTB の範囲の外なので直していない（§4.6.7）。

## 11. Resume point

- p001: 第 3 版（2026-11-14 P6）。今の code との照合と決定の書き入れ、design-reviewer（§15）の反映。cleared は Q1 の判定。
- p002: [phase002/phase.md](phase002/phase.md)。

## 12. 未確認

1. 5330 の zedBSD での CPU の番号の並び（P が先か）と、`core` の番号で SMT の兄弟が対になっているか（p006 の最初に `kern.dtb` で記録）。
2. 5330 で P の兄弟が忙しい時の 1 thread の速さ（`DTB_SMT_SHARE_PERMILLE` の 600 は仮の値）。
3. hal.h に `hal_task_transfer` の「後継の文脈で去った task を受ける」契約の文が無い（x86_64・arm64・riscv64 の実装の comment と盗みの前提にはある）。手渡しが起きるのは hybrid の機械だけなので v1 の範囲では問題にならない。
4. 各 thread の実際の負荷（compositor の thread が BUSY・HEAVY にならない事）。p006 で `kern.dtb` の印の数と移動の数で見る。
5. `make -j12` の build の中で compiler が BUSY の閾値（200 ms）を越える割合（p006 の S3 で記録）。
6. kernel の thread が長く忙しい場面（GPU の shader の compile など）が desktop でどれだけあるか。user の既定を 10 に下げた時の影響（p004 の QEMU §6.2 の 6 と p006 の計数）。
7. WS075 の cursor の遅れの器具が今の 5330 で使えるか（p006 の最初）。

## 13. 設計の判断の根拠（補い）

- 「起床の道を変えない」: 今の sched は同じ priority 以上の起床で横取りを頼む（§2.1）ので、P に戻った UI の thread は待たずに走る。UI の thread を P に置くのは DTB の周期（眠っている間に `sched_set_cpu`）で足り、起床ごとの移動の代価（CPU の間の通知・cache の冷え）を払わない。
- 「user を kernel の既定より上に置かない」: kernel の worker の大半が 8 で、入力（USB の HID）と表示（`gpu_monitor`）の道にも居る。user を上に置くと、入力から画面までの道そのものを待たせる。
- 「MOVE は idle の CPU だけ」: 移動の後すぐ走れる所だけに動かせば、盗みとの行ったり来たりも、走っている CPU の後ろでの待ちも生まない。
- 「BUSY の溢れを許す」: build の時間（A5）を守る仕組み。電力（A4）の場面 S2 は 4 本で E に余裕があるので、溢れは起きない。

## 14. design-reviewer の指摘と反映（[design-review.md](design-review.md)、第 1 版の review、2026-10-31）

| 指摘 | 反映 |
| --- | --- |
| B1 user を 6・7 に上げると kernel の worker（多くは 8）を飢えさせる | 確かめた（`kthread_create` 97 か所の大半が 8）。user の thread を 8 より上に置かず、DTB が on の時の user の既定を 10 に下げ、上げは 8・9（§4.7）。飢えの防ぎは user の全部の段に、kernel の段へは上げない。Q302-3 を書き直し、案 B（上げ無し）を足した |
| B2 `sched_set_cpu` の状態の検査が lock の外で、NEW・REAPING を断らない | 確かめた（sched.c:1704〜1712、thread.c:581〜589）。lock の中の確かめ直しを足した（§4.6.7、`#if CONFIG_DTB`）。QEMU に作成・exit の嵐を足した（§6.2 の 3） |
| M1 手渡しの席が上書きされうる | 席がふさがっている間は新しい手渡しをしない。tick の頭で `complete_handoff`（§4.6.5 の 2・4） |
| M2 `sched_hint_set` の入れ直しが sleep の queue から thread を出す | RUN・WOKEN の時だけ入れ直す（§4.5）。QEMU に時間つきの sleep の試験（§6.2 の 3） |
| M3 溢れと PLACE の行ったり来たり、FAST の選びが E を返す | MOVE と PLACE の選びを分け、MOVE は希望の種類の idle の CPU だけ（§4.6.3・§4.6.4） |
| M4 A4 は電力の上限に張り付いて測れない | S2 を 4 本に、PL の制限の印と loop の進みを記録、off で P に載らなかった run を外す（§6.3・§6.4）。WS333 の PL1 の食い違いは Q1（§10.2 の 3） |
| M5 LTO で `.o` の大きさの比べは意味が無い | 最終の `vmunix` の逆 assemble の比べに替えた（§7 の p002） |
| M6 Phase の依存が問いの答えに依る、p003 の code が p004 まで試されない | 依存に問いを書き、試験の config と stress を p003 に移した（§4.11・§7）。推しで先に始めるかは Q1（§10.2 の 5） |
| M7 nice で下げた thread が kernel の lock を持って待つ逆転 | kernel の中の thread は queue に入る時に user の既定（10）より下げない（§4.6.1 の 2）。飢えの防ぎが上限 |
| M8 GPU に仕事を出すだけで priority が上がる | CPU を半分以上使う thread（HEAVY）は上げない。上げは 9 までで kernel の段に入らない。GPU を開けるのは seat の利用者と root（§4.4.3） |
| M9 terminal の規則がほとんど何もしない | R6 を PLACE_IDLE（兄弟も idle の P がある時に寄せる、予算は最後）に。新しい thread と fork の子が hint を継ぐ（§4.4.3・§4.6.4）。terminal の応答は打鍵 → 表示（A1）で見る |
| M10 `move_pending` の後始末と判定の競争、EINVAL の二義 | 判定は thread の CPU の lock の中、印は走っている間だけ生きる（§4.6.5 の 1・7）。戻りの値を分けた（ESRCH を足した、§4.5） |
| M11 戻しの所の網羅、kernel の段への上げ、`base_priority` の初め | 戻しの所を全部挙げ 1 つの関数に（§4.6.1）。上げは 8 まで（§4.7）。thread.c の 3 か所と `kthread_create` で初める |
| m1 手渡しの間の起床の説明 | `SCHED_WAKE_PENDING` を立てる事を書き、移動の終わりで消す（§4.6.5 の 4・6） |
| m2 HAL の契約の文が無い | §5 に、amd64 の実装と盗みの前提である事、hal.h に文を足すのは承認が要るので足さない事を書いた。§12 の 3 |
| m3 input の publish は 9 か所、外しと resync を数えてしまう | 普通の入力の 2 つの関数でだけ通知する（§2.3・§4.3） |
| m4 platform の数 | 15 の vmunix.mk、使わないのは mips32-rg350 だけ（§2.3・§2.6・§4.11） |
| m5 process の巡回は O(N²) | 書いた。周期の時間が 1 ms を超えれば巡回の口を足す（§4.9 の 3、p004） |
| m6 header の細部 | `dtb_thread_state` の GPU の field を外し comment を直した。`stdbool.h` を外した。mask の長さを `KERN_CPU_MASK_WORDS` と書いた。`sched_hint_place` は眠らないと書いた |
| m7 起動の parameter の値の検査 | `boot.c` の値の検査の表に足す事、`dtb.classes=` は自分の解析（§4.11） |
| m8 記録の食い違い | user-questions.md と ws.md の「反映済み」はこの第 2 版で事実になった。ws.md の「今の事実」に design.md §2.8 への注を足した。原文の記録の形は Q1（§10.2 の 6） |
| m9 試験の穴 | 競争の試験（§6.2 の 3）、nice の割合の幅（§6.2 の 5）、kernel の thread との競争（§6.2 の 6）、実の compiler の BUSY の記録（§6.3 の S3）、器具の確かめ（§6.3 の最初） |
| m10 計数の書き手 | `wakeups` を足す所を全部挙げ、近似と書いた。`user_streak` は飽和（§4.3） |
| m11 「p005」の曖昧さ | `ws333-p005` のように書いた（§6.3・§7） |
| m12 問いの公平さと抜け | Q302-1 に terminal の規則の読みと案 E を足した。Q302-3 の kernel の段の誤りを直し、user の段の下げと GPU の上げの条件を問いに入れた。Q302-6 に形の案を付けた |
| m13 64 本を超える process の続き | tid の cursor にした（§4.9 の 3） |

## 15. design-reviewer の指摘と反映（第 3 版の review、2026-11-14）

（review の後に埋める。[design-review.md](design-review.md) の「第 3 版」）
