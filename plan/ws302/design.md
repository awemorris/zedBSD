<!-- awesome-plan project=zedbsd record=ws302-design -->

# WS302 設計: デスクトップスレッドバランサ（DTB）

- 版: **第 1 版**（2026-10-31 P5、q1775-i01）。design-reviewer の指摘の反映は §14。
- 基点: worktree `p5-ws302`（branch `agent/p5-ws302`、main 3d820d9ca）。引用の file:行 はこの基点の物。code は書いていない。
- 読んだ物: `AGENTS.md`、`plan/agents/protocol.md`、[ws.md](ws.md)、[survey-source.md](survey-source.md)（2026-10-29、main 61630bb0b の調べ。行は §2.8 で引き直した）、
  `plan/ws333/design.md`（§4.1・§4.4・§4.5・§4.12・§9・§11）と `plan/ws333/ws.md`、`plan/ws309/ws.md` と design.md §1〜§3、`plan/guardrail.md`（範囲と構造・driver は HAL を知らない・出典・開発の管理情報）、
  `plan/master-design-policy.md`（§10・§11）、`plan/coding-style.md`（§2・§10・§13）、`plan/tools.md`（計測の道具）。source は §2 に挙げた物。
- 出典: 外部の source（Linux・FreeBSD など）は読んでいない。web も使っていない。設計は zedBSD の code の事実（§2）とユーザーの決定（§0）から立てた。

## 0. ユーザーの決定（要点）

記録の規則（2026-10-30）により、ここには要点だけを書く。2026-10-27 の原文は [ws.md](ws.md) の「出典」にある。

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-27 | config で有効にできる DTB を kernel に足す。DTB は scheduler ではなく、thread を適した core に移すよう sched に依頼し、balancing と scheduling の hint を出す。HID・GPU・process/thread の統計と nice を使う。GPU を使う app は応答の priority を上げて速い core に、syscall の無いまま長く忙しい thread は遅く省電力の core に、terminal の app（GPU を使わず tty がある）は UI の応答を落とさない範囲で速い core に。まずどの情報からどんな balancing ができるかを検討する | §3（情報 → 性質 → hint の表）、§4（DTB と sched の口） |
| 2026-10-27 09:39（クリック） | `CONFIG_DTB` の既定は off。nice は core の選びに加えて sched の priority にも効かせる | §4.11（config）、§4.7（nice の段と飢えの防ぎ） |
| 2026-10-31（Q1 経由の依頼の趣旨） | 不在の間（昼頃まで）設計だけ進められる WS は設計を進める。WS333 の cpuperf の口（P/E の種類・相対の性能）が main に入ったので WS302 の設計の時期 | この文書 |

## 1. 目標と範囲

- 目標（ws.md）: `CONFIG_DTB` で有効にできる DTB を kernel に入れ、5330（P-core 2・E-core 8）で thread の性質を統計と nice から見分けて sched に hint を出し、DTB を切った時に比べて desktop の応答を落とさず、長く忙しい計算の thread を E-core に寄せて電力を下げる事を数で示す。
- この文書が決める物: どの統計からどの性質を推すか（§3）、統計の取り方と置き場所（§4.3）、分類と規則の強さの順（§4.4）、sched への hint の口と sched の最小の変更（§4.5・§4.6）、nice と priority（§4.7）、5330 の割り付けの方針（§4.8）、DTB の周期の処理（§4.9）、状態・資源・lock・失敗（§4.10〜§4.14）、config と観察（§4.11・§4.15）、試験と受け入れ（§6）、Phase（§7）。
- 範囲の外: sched の作り直し（hint を受ける最小の変更だけ）、周波数の制御（WS333）、hybrid でない機械の評価、Keiland の thread の作りの変更（WS309）、compositor からの focus の hint（Q302-4 の答えで条件つきの Phase）。

## 2. 今の振る舞い（code の事実、main 3d820d9ca）

### 2.1 sched（`src/kern/sched.c` 2978 行、`include/kern/sched.h`）

- CPU ごとの `struct sched_cpu`（sched.c:181）: spinlock 1 本（`LOCK_RANK_SCHEDULER` 200、`include/kern/lock.h:64`）、priority 16 段ごとの `run[]` と `woken[]`、sleep の queue、idle・current・retired、`need_resched`・`preempt`・`preempt_count`、tick の分類の時間 `times[]`。global な run queue は無い。
- priority は 0 が最高・15 が最低・既定 8（sched.h の `SCHED_PRIOR_*`・`SCHED_PRIORITY_DEFAULT`）。user の thread は作る時に全部 8（`src/kern/thread.c:104`・`:190`、idle は `:410`）、kernel の thread だけが `kthread_create` の引数で決める（`:256`）。作った後に priority を変える関数は無い。
- 取り出しは厳密に上の段から（`pick_next_locked` sched.c:2192）。aging は無い。queue の出し入れは `thread->sched.priority` を添字にする（`queue_remove_thread` :2178）。
- 起床（`sched_wakeup` :520）は thread の居る CPU にだけ戻す。起こされた thread の priority が走っている thread 以上なら `woken[]` に入れ `preempt` を立てる（`wake_enqueue_locked` :2051）。全部が 8 の今は、どの起床も横取りを頼む。
- 切り替わる所: user への戻り（`kernel_user_return_handler` signal.c:401 → `sched_preempt_point` sched.c:910）、tick（`sched_clock_cpu` :1009、quantum 10 ms の終わりで `sched_yield`）、CPU の通知（`sched_cpu_notify` :1790、`clock.c:207` から。その直前に `kern_cpuperf_notify` clock.c:205）。
- 新しい thread の置き場所（`sched_prepare_thread` :398）: 作り手の CPU に backlog が無ければそこ、あれば idle の CPU（`choose_idle_cpu` :2390、番号の小さい順）、それも無ければ round robin（`choose_cpu` :2269）。
- 負荷の分け: idle の CPU が他の CPU の queue から盗む（`sched_idle` :1577 → `steal_runnable` :2554 → `take_stealable_locked` :2479、同じ tick に入った thread は取らない）と、忙しい CPU が tick で idle の CPU を 1 つ起こす（:1125〜1137 の `backlog`）だけ。core の種類・性能・SMT の兄弟を知らない。
- 移動: `sched_set_cpu`（:1691）は走っていない thread（RUNNABLE・SLEEPING）だけを移す。移動の途中の起床は `SCHED_WAKE_PENDING`（:123）で移動先に持ち越す。sched.c の外に呼び手は無い。
- 走っている thread は移せない: amd64 の `hal_task_transfer`（`src/hal/amd64/task.c:832`）は `run_cpu >= 0` の task を `HAL_ERR_BUSY` で断る。`run_cpu` は切り替えの後、次の context が走り始めた所で `amd64_task_finish_switch`（:676）が -1 にする（新しい task の入口も含む）。だから後継の context の中では、去った task は既に移せる。盗みもこの性質に頼っている（sched.c:2550 の comment）。
- `sched_exit_current`（:938）が去る thread を `cpu->retired` に置き、後継が `complete_retired`（:2237）で片付ける形が既にある。切り替えの後の 4 か所（:813・:897・:964・:2361）で呼ぶ。

### 2.2 nice・syscall・計数

- `setpriority`（`sys_setpriority_call` syscall.c:8880）は process の `nice_value`（`include/kern/process.h:129`、-20〜+20 に丸める）に入れるだけ。下げるのは root だけ、他人の process は euid の判定。fork で継ぐ（process.c:1113）。sched は nice を読まない。`/dev/system` の process の情報に `nice_value` と `has_controlling_terminal` がある（`include/uapi/system.h:81`）。
- syscall の入口（`kernel_syscall_handler` syscall.c:10420）は割り込みを止めた窓で `sched_accounting_kernel_enter()`（:10445）を呼び、出口で leave（:10516）。この窓は同じ CPU の tick と競わない。
- tick の計数（`sched_clock_cpu` :1062〜1100）: 走っている user の thread の process に `cpu_ticks`・`user_ticks`・`system_ticks` を足す（`accounting_kernel_depth` で user か kernel か）。thread ごとの時間・syscall の数・起床の数は無い。
- CPU ごとの時間は `sched_cpu_time`（:1517）と `hw.cputimes`（`include/uapi/sysctl.h:72`）。

### 2.3 HID

- input の core（`src/drivers/generic/input.c`）は、全ての device の全ての report を `drv_input_subscriber_publish`（:1602）で subscriber に配る（:826〜1081 の 6 か所）。callback は subscriber の spinlock（`LOCK_RANK_DEVICE`、irqsave）の中で、短く眠らない約束（:1600 の comment）。購読の口は `drv_input_subscribe`（:1517、`include/kern/input-device.h:111`、最大 `INPUT_SUBSCRIBER_MAX`、今は console だけ）。
- input.c は 17 の platform のうち 12 の vmunix.mk が build する（grep で 12）。最後の入力の時刻・数を持つ所は無い。

### 2.4 GPU

- `/dev/gpuN` の ioctl は全部 `gpu_ioctl`（`src/drivers/gpu/gpu.c:1840`）→ `gpu_ioctl_body`（:2113）を通る。GPU の仕事を出す ioctl は `GPU_COMMAND`・`GPU_COMMAND_SUBMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_JOB_COMMIT`、画面に出す ioctl は `GPU_PRESENT`・`GPU_DISPLAY_PRESENT`・`GPU_DISPLAY_PRESENT_SYNC`・`GPU_DISPLAY_COMMIT`（`include/uapi/gpu*.h`）。
- `struct gpu_session`（:225）に process・thread ごとの使用量は無い。device ごとの `hw.gputelemetry` だけ。
- Keiland の compositor は GPU と表示を libvulkan だけで扱い（guardrail）、client の app も libvulkan で `/dev/gpuN` に仕事を出す。画面に出す ioctl は compositor（と game mode の直の scanout）だけが呼ぶ。

### 2.5 CPU の性能（WS333 の cpuperf、main に入った物）

- `kern_cpuperf_cpu(cpu, flags, info)`（`include/kern/cpuperf.h:354`、cpuperf.c:577）: `READ_SAMPLE` 無しなら box の spinlock（`LOCK_RANK_DEVICE`、irqsave）だけで読む。`info.topology.core_class`（UNKNOWN・PERFORMANCE・EFFICIENCY）・`package/die/module/core/thread`、`relative`（0〜100、UNKNOWN）、`available`・`efficiency`（HFI の 0〜255、UNKNOWN）。
- `kern_cpuperf_generation()`（:385、lock 無し）: topology・domain・種類・能力・相対・HFI のどれかが変わると動く。
- amd64 の CPU の driver（`src/drivers/platform/pcat/cpu-x86/cpu-x86.c`）は `CONFIG_CPU_PERF` に依らず provider を登録し（:171）、CPUID 1AH で core の種類を出す（`cpu_x86_read_kind` :495、hybrid でなければ全部 PERFORMANCE）。`relative` と HFI は `CONFIG_CPU_PERF=y` で、最初の strategy の後だけ（WS333 design §4.5 の表）。`platform/amd64/vmunix.mk:512〜524` の build の分け。
- `kern_cpuperf_init()` は `src/kern/entry.c:382`、`kern_thermal_init()` は :405、x86 の driver の登録は `src/kern/platform/pcat.c:497`。

### 2.6 config と起動の parameter

- `config/kernel-options.list` の行（例 `CONFIG_CPU_PERF|bool|…|amd64|n|`、:3）、`Makefile:941` の `-DCONFIG_CPU_PERF=0/1`（`ZEDBSD_CONFIG_CPPFLAGS`、14 の vmunix.mk が使う）、`platform/amd64/vmunix.mk` の source の列（:493 に `src/kern/cpuperf.c`）。
- 起動の parameter は `include/kern/boot.h:107` の enum（`KERN_BOOT_PARAMETER_CPUPERF` など）と `src/kern/boot.c` の名前の表。

### 2.7 Keiland の thread

- WS309（design §2・§3.1）が compositor の thread を台帳（`kwl_thread_start`、名前と本数）に整理した。kernel に thread の名前は無い（`pthread_setname_np` も無い）。compositor・app の thread に priority を付ける所は無い。

### 2.8 survey-source.md との違い

- 行の引き直し: `setpriority` は syscall.c:8880（survey では 8863）、syscall の入口の accounting は :10445・:10516（survey 10427・10498）、`sched_set_cpu` は sched.c:1691（1665）、盗みは :2554（2528）。
- survey §0 の ws.md の食い違い（nice と CONFIG_DTB の既定）は、今の ws.md では既に直っている（「含む」と D1・D3 が 2026-10-27 09:39 の決定に揃っている）。
- survey §6 の 5「topology を知らない（WS301 待ち）」は解けた（§2.5）。

## 3. どの情報から何を推し、どんな hint にするか（ws.md の表を埋めた物）

| 情報（源、取り方） | 推す性質 | hint | 取る代価 | 外れる時と注意 |
| --- | --- | --- | --- | --- |
| 画面に出す GPU の ioctl の最後の時刻（GPU の core、process ごと、新） | 画面を作る process（compositor、game mode の app） | 速い core を強く、priority 6 | ioctl 1 回に atomic の store 1 つ | game mode の app も当たる（望ましい） |
| GPU の仕事を出す ioctl の最後の時刻（GPU の core、process と thread ごと、新） | GPU を使う app（desktop の client・game・video） | 速い core、priority 7 | 同上 | 背景の GPU の計算だけの process も当たる（kernel には見分けが無い）。nice > 0 で外せる |
| syscall の無いまま user で走り続けた長さ（tick と syscall の入口、thread ごと、新） | 長く忙しい計算 | 遅い省電力の core、priority の上げを外す | tick ごとに加算 1 つ、syscall ごとに store 1 つ | compiler の最適化の段も当たる。build の時間は「空いた P に溢れてよい」（§4.6.4、Q302-2）で守る |
| controlling tty があり GPU を使わない（process、既存＋GPU） | terminal の中の道具（shell・editor・短い command） | 速い core を弱く（空いている時だけ）、利用者が操作中の時だけ | 無し（既存の field） | build の compiler も tty を持つ。忙しい規則が先に効く（Q302-1） |
| nice（process、既存） | 利用者の意図（背景か急ぎか） | nice > 0 は遅い core を強く・priority を下げる、nice < 0 は速い core・priority を上げる | 無し | 他の規則より先（利用者の明示の意図、Q302-1） |
| HID の最後の入力の時刻と数（input の core、全体で 1 つ、新） | 人が操作中か | 操作中: 上の規則を全部。無操作が続く: terminal の弱い規則を止め、周期を延ばす | report ごとに atomic 2 つ | 入力の宛先の app は kernel に分からない（Q302-4） |
| 起床の数・走った tick（thread ごと、新） | 対話的か（短く走って眠る） | **v1 では規則に使わず観察だけ** | 起床ごとに加算 1 つ | P が 4 つしかなく、弱い候補を増やすと UI の席を食う。p006 の数で要れば足す |
| 待ちの長さ（既存の `queued_tick`） | 飢えているか | priority の下げの飢えの防ぎ（§4.7） | tick ごとに見るのは queue の先頭だけ | — |
| compositor からの前面の app（UAPI、新、任意） | 前面の app | 前面を速い core | — | UAPI の決定が要る。v1 には入れない（Q302-4） |

## 4. 設計

### 4.1 全体の形

```
 user:  （v1 は UAPI を足さない。観察の読みだけ Q302-6）
                                       |
 kernel:  src/kern/dtb.c（CONFIG_DTB=y だけ）: 周期の thread「dtb」、process と thread を巡って分類し、sched に hint を渡す
          src/kern/dtb-policy.c（同）: 分類の純な関数（host 試験の対象）
             |  読む: kern_cpuperf_cpu / kern_cpuperf_generation（WS333）、process の nice・tty・GPU の時刻、HID の時刻、sched の thread の計数
             v  渡す: sched_cpu_classes_set（CPU の種類の表）、sched_hint_set（thread の希望と priority）、sched_hint_place（今すぐ寄せる）
          src/kern/sched.c: hint を持ち、置き場所・盗み・idle の CPU の選び・走っている thread の手渡し・飢えの防ぎに使う（#if CONFIG_DTB）
          src/kern/syscall.c・sched.c の tick: thread の計数（#if CONFIG_DTB）
 driver:  src/drivers/gpu/gpu.c: kern_dtb_note_gpu()（成功した提出と表示の ioctl）
          src/drivers/generic/input.c: kern_dtb_note_input()（report を配る所）
          （driver は kern_* だけを呼ぶ。HAL を知らない規則のまま）
 HAL:     変えない。hal_task_transfer（止まっている task）と hal_cpu_notify を sched が今の契約のまま使う
```

- **DTB は sched ではない**: 実行の順・CPU の選び・移動の実行は sched が決める。DTB は「この thread はどの種類の core を望むか、どの強さで、どの priority か」を書き、「今寄せてほしい」を頼むだけ。sched は CPU の空き具合を見て、頼みを果たせない時は断る（EAGAIN）。
- **sched は cpuperf を呼ばない**: sched の lock（rank 200）の中で cpuperf の box の lock（rank 125）は取れない。DTB が thread の文脈で cpuperf を読み、CPU の種類の表を sched に写す。
- **`CONFIG_DTB=n` は今と同じ**: sched・syscall・gpu・input の追加は全部 `#if CONFIG_DTB` か、`include/kern/dtb.h` の空の inline。struct の field も同じ条件（Q302-5 の推し）。

### 4.2 責務と境界

| 部品 | 持つ物 | 持たない物 |
| --- | --- | --- |
| DTB（dtb.c） | 周期の処理、process と thread の巡回、分類の状態（履歴の幅）、CPU の種類の表の作成、HID の時刻、移動の予算 | CPU の選び、queue の操作、周波数 |
| 分類（dtb-policy.c） | 入力（統計・nice・tty・時刻・HID の状態）→ hint の純な関数、規則の強さの順、閾値 | kernel の object |
| sched | hint の保存、priority の付け替え、CPU の種類の表、置き場所・盗み・idle の CPU の選びの好み、走っている thread の手渡し、飢えの防ぎ、thread の計数 | 分類、cpuperf の読み |
| cpuperf（WS333） | core の種類・相対の性能・HFI・世代 | DTB の事 |
| GPU の core・input の core | 自分の ioctl・report の所で 1 行の通知 | 分類 |

### 4.3 統計（取る所・書き手・代価）

全部 `CONFIG_DTB=y` の時だけ（Q302-5）。thread の field は `struct sched` に埋める `struct sched_hint_state`（[proposed/include/kern/sched-hint.h](proposed/include/kern/sched-hint.h)）、process の field は `struct dtb_process_activity`（[proposed/include/kern/dtb.h](proposed/include/kern/dtb.h)）。

| 統計 | 書く所（書き手 1 つ） | 読む所 | 同期 |
| --- | --- | --- | --- |
| `run_ticks`（走った tick） | `sched_clock_cpu` の「Charges the running thread」の block（その CPU の lock の中） | DTB | 読みは atomic の load（u64） |
| `user_streak`（syscall の後に user で走った tick） | tick で `accounting_kernel_depth == 0` なら +1。syscall の入口の割り込みを止めた窓（syscall.c:10445 の直後）で 0 に | DTB | 同じ CPU の tick と入口は割り込みの窓で重ならない。DTB は load だけ |
| `syscalls`（数、一周してよい u32） | syscall の入口（同じ窓） | DTB | 同上 |
| `wakeups`（u32） | `sched_wakeup` の enqueue の所（CPU の lock の中） | DTB（観察） | 同上 |
| process の `gpu_submit_tick`・`present_tick`、thread の `gpu_submit_tick` | `kern_dtb_note_gpu()`（gpu_ioctl の成功の後、呼んだ thread の文脈） | DTB | atomic の store・load |
| HID の `input_tick`・`input_count`（全体で 1 つ） | `kern_dtb_note_input()`（`drv_input_subscriber_publish` の頭、irqsave の lock の中から。atomic だけ） | DTB | atomic |

- `kern_dtb_note_input()` は events のある report だけを数え、`INPUT_REPORT_DETACH` は数えない。subscriber の枠は使わない（購読でなく input の core から kernel への 1 行の通知。kernel が driver の関数を呼ぶ向きを作らず、input.c の無い platform でも DTB が link できる）。
- `kern_dtb_note_gpu()` は ioctl の番号を GPU の core が `KERN_DTB_GPU_SUBMIT`・`KERN_DTB_GPU_PRESENT` に読み替えて渡す（DTB は GPU の UAPI を知らない）。
- 代価の見積もり（p002 で測る）: tick に加算 2〜3、syscall に store 2、wakeup に加算 1、GPU の ioctl に store 2〜3、input の report に atomic 2。

### 4.4 分類（dtb-policy.c）と規則の強さの順

#### 4.4.1 thread の印（DTB が履歴を持つ）

| 印 | 立つ | 消える | 履歴の幅 |
| --- | --- | --- | --- |
| BUSY | 周期の時点で `user_streak` ≥ `DTB_BUSY_ON_MS`（初め 200 ms） | syscall が増え、かつ `user_streak` < `DTB_BUSY_OFF_MS`（20 ms）の周期が `DTB_BUSY_OFF_PASSES`（10）続いた | 立つのは速く、消えるのは遅く（行ったり来たりの防ぎ） |
| GPU（thread） | thread の `gpu_submit_tick` が `DTB_GPU_WINDOW_MS`（1000 ms）以内 | 窓を出た | 窓の長さ |
| GPU（process） | process の `gpu_submit_tick` が窓の中 | 同上 | 同上 |
| PRESENTER（process） | process の `present_tick` が `DTB_PRESENT_WINDOW_MS`（1000 ms）以内 | 窓を出た | 同上 |
| TTY（process） | `controlling_tty != NULL` かつ GPU（process）でない | どちらかが崩れた | — |

#### 4.4.2 全体の状態（HID）

| 状態 | 条件 | 効き |
| --- | --- | --- |
| ACTIVE | 最後の入力から `DTB_USER_ACTIVE_MS`（5 s）以内 | 全部の規則。周期 `DTB_PASS_MS`（100 ms） |
| QUIET | 5 s〜60 s | 全部の規則。周期 100 ms |
| AWAY | 60 s 以上入力が無い（起動の直後も） | 規則 R6（terminal）を止める。周期 `DTB_PASS_AWAY_MS`（1000 ms） |

入力の宛先が分からないので、HID は「人が見ているか」にだけ使う（Q302-4）。

#### 4.4.3 規則と強さの順（Q302-1 の推し A）

上から最初に当たった規則が core の希望を決める。priority は別に下の表の列で決める。

| 順 | 規則 | core の希望 | 強さ | priority |
| --- | --- | --- | --- | --- |
| R1 | nice < 0 | FAST | PLACE | nice の段（§4.7）、ただし R3・R5 の上げと比べて高い方 |
| R2 | nice > 0 | EFFICIENT | PLACE＋NO_OVERFLOW | nice の段（下げ） |
| R3 | BUSY | EFFICIENT | PLACE | 8（上げを外す） |
| R4 | PRESENTER の process | FAST | PLACE | 6 |
| R5 | GPU（process） | FAST | PLACE | 7 |
| R6 | TTY、かつ状態が AWAY でない | FAST | PREFER | 8 |
| R7 | 他 | ANY | — | 8 |

- 強さ: **PREFER** は sched が置き場所と盗みで好むだけ（DTB は動かさない）。**PLACE** は DTB が違う種類の core に居る thread を寄せる（§4.9 の予算の中）。**NO_OVERFLOW** は違う種類の idle の CPU に盗ませない（飢えの防ぎの待ちの上限 §4.7 を超えたら盗ませる）。
- R3 が R4・R5 より上: GPU を使う process の中でも、syscall の無い計算の thread（game の worker、compiler の内部）は E に寄せる。GPU に仕事を出す thread は出すたびに syscall を通るので BUSY にならない。compositor の render・main の thread は fence の待ちと ioctl で BUSY にならない。
- R1 が R3 より上: nice < 0 は root が付けた急ぎの印（下げるのは root だけ、syscall.c:8936）。
- 選ばなかった順（Q302-1 の案）: B「GPU・PRESENTER を BUSY より上」（game の worker が P に残り、電力が下がらない）、C「UI を nice より上」（利用者の明示の意図に反する）、D「TTY を BUSY より上」（terminal の build が P に張り付き、P が 4 つの 5330 で UI の席が無くなる）。

### 4.5 sched への hint の口（kernel の中、HAL を変えない）

案の全文は [proposed/include/kern/sched-hint.h](proposed/include/kern/sched-hint.h)。要点:

```c
#define SCHED_HINT_CORE_ANY		0U
#define SCHED_HINT_CORE_FAST		1U
#define SCHED_HINT_CORE_EFFICIENT	2U

#define SCHED_HINT_PREFER		1U	/* placement and stealing only */
#define SCHED_HINT_PLACE		2U	/* sched_hint_place() may move it */
#define SCHED_HINT_NO_OVERFLOW		0x1U	/* flag: other kinds steal it only after the starvation limit */

struct sched_hint {
	uint8_t core;		/* SCHED_HINT_CORE_* */
	uint8_t strength;	/* 0, SCHED_HINT_PREFER or SCHED_HINT_PLACE */
	uint8_t flags;		/* SCHED_HINT_NO_OVERFLOW */
	uint8_t priority;	/* the base level, SCHED_PRIOR_HIGH..SCHED_PRIOR_LOW */
};

struct sched_cpu_class {
	uint8_t core;		/* SCHED_HINT_CORE_FAST, _EFFICIENT, or _ANY for unknown */
	uint8_t usable;		/* 0 when the hardware feedback says to avoid the CPU */
	uint16_t rank;		/* 0..1000, the CPU's speed for one thread, its sibling idle */
	uint16_t sibling_rank;	/* the same with its SMT sibling busy */
	uint16_t core_group;	/* CPUs of one core share it (SMT siblings) */
};

void sched_cpu_classes_set(const struct sched_cpu_class *classes, unsigned count);	/* thread context */
int  sched_hint_set(struct thread *thread, const struct sched_hint *hint);			/* any thread context */
int  sched_hint_place(struct thread *thread);	/* 0, EALREADY, EAGAIN, EBUSY, EINVAL */
void sched_hint_activity(const struct thread *thread, struct sched_hint_activity *activity);
void sched_note_syscall(void);			/* the syscall entry, interrupts masked */
void sched_hint_counters(struct sched_hint_counters *counters);
```

- `sched_cpu_classes_set`: 表を CPU ごとの field に写し、写し終えてから世代を release で進める。表が無い（呼ばれていない）か、全部の CPU が同じ種類なら、core の希望は全部 ANY と同じに扱う（sched の振る舞いは priority 以外いまと同じ）。
- `sched_hint_set`: thread の居る CPU の lock を、`sched_wakeup` と同じ「読み直して確かめる」loop で取り（:531〜540 の形）、hint と base の priority を置く。queue に居れば新しい段に入れ直す（`queue_remove_thread` → `queue_append`）。走っていて priority が下がり、上の段に待ちがあれば `preempt` を立てる。ZOMBIE・DEAD・idle の thread は EINVAL。
- `sched_hint_place`: 希望の種類の CPU に居なければ寄せる。走っていない thread は §4.6.3 の選びで行き先を決めて `sched_set_cpu`。走っている thread は `move_pending` を立てて居る CPU に `notify_cpu`、その CPU が §4.6.5 の手渡しをする。行き先の種類に「idle の CPU か backlog の無い CPU」が無ければ EAGAIN（動かさない。行ったり来たりの防ぎ）。
- 走っていない thread の移動は既存の `sched_set_cpu`（:1691）を使うので、移動の途中の起床の扱い（`SCHED_WAKE_PENDING`）はそのまま。

### 4.6 sched の変更（最小、全部 `#if CONFIG_DTB`）

#### 4.6.1 priority の 2 つの値

`struct sched` に `base_priority`（hint の段）を足す。`priority`（今の field、queue の添字）は普段 `base_priority` と同じで、飢えの防ぎ（§4.7）が 1 回だけ上げる。上げた thread は次に queue に戻る所（`sched_yield` の入れ直し :776、`sched_preempt` の前への戻し :877、`wake_enqueue_locked` :2051、`sched_add` :468）で `base_priority` に戻す。

#### 4.6.2 CPU の種類の表

`struct sched_cpu` に `class`（§4.5 の `struct sched_cpu_class` の写し）。読みは lock 無しの atomic の load（古い値は 1 回の好みの外れで済む）。

#### 4.6.3 行き先の選び（共通の小さな関数、host 試験の対象）

希望 FAST の thread の行き先の順: 使える FAST で SMT の兄弟も idle（`rank`）→ 使える EFFICIENT で idle（`rank`）と、使える FAST で兄弟が忙しい（`sibling_rank`）を rank の大きい順 → backlog の無い希望の種類の CPU → 無し（EAGAIN）。
希望 EFFICIENT: 使える EFFICIENT で idle → backlog の無い EFFICIENT → 無し。
`rank` の値は §4.8。純な関数 `sched_hint_choose(classes, count, idle_bits, backlog_bits, want)` にして host で試す（[proposed](proposed/include/kern/sched-hint.h) の末尾）。

#### 4.6.4 置き場所・盗み・idle の CPU の起こし

| 所 | 今 | 変更 |
| --- | --- | --- |
| 新しい thread（`sched_prepare_thread` :398） | 作り手の CPU、backlog があれば idle の CPU（番号順） | 作り手の hint を継ぐ（同じ process の時）。作り手の CPU に backlog がある時だけ、§4.6.3 の順で idle の CPU を選ぶ。作り手の CPU で済む普通の道は変えない |
| tick の助け（:1125〜1137） | 番号の小さい idle の CPU を 1 つ起こす | 待ちの先頭の thread の希望の種類の idle の CPU を先に。無ければ今どおり |
| 盗み（`take_stealable_locked` :2479） | 上の段から、同じ tick の thread は取らない | 同じ段の中で、盗む CPU の種類を望む thread を先に取る。NO_OVERFLOW の thread は、違う種類の CPU には待ちが飢えの上限を超えるまで渡さない。PLACE EFFICIENT（BUSY）の thread は、idle の FAST の CPU が盗んでよい（**溢れ**、Q302-2 の推し） |
| 起床（`sched_wakeup` :520） | 居る CPU に戻す | **変えない**（起床の道に移動を足さない。種類の違いは DTB の周期の移動で直す。理由は §13） |

#### 4.6.5 走っている thread の手渡し（handoff）

BUSY の計算の thread は眠らず、CPU に他の仕事が無ければ quantum の終わりにも自分に戻るので、`sched_set_cpu` では一生移せない。今の `retired` の形（§2.1）にならい、去った後に後継が移す:

1. `sched_hint_place` が走っている thread に `move_pending = 1` を立て（CPU の lock の中）、その CPU に通知する。
2. その CPU の `sched_cpu_notify`（:1790）か tick（`sched_clock_cpu` の終わり、`sched_yield` を呼ぶのと同じ所）が、current に `move_pending` があり `preempt_count == 0` なら `sched_handoff()` を呼ぶ。
3. `sched_handoff()`（`sched_yield` の形）: CPU の lock の中で §4.6.3 の選びで行き先を決める。無ければ `move_pending` を消して戻る。あれば current を RUNNABLE・`need_migrate = SCHED_MIGRATING`・queue に入れずに `cpu->handoff`・`cpu->handoff_target` に置き、次の thread（無ければ idle）に切り替える。
4. 後継は `complete_retired` を呼ぶ 4 か所と tick の頭で `complete_handoff(cpu)` を呼ぶ: `hal_task_transfer(task, target)`（去った task は `amd64_task_finish_switch` の後なので止まっている）→ `retarget_migrating`（:2533）→ 行き先の lock で `run[priority]` に入れ `need_resched` → `notify_cpu(target)`。
5. 断られた（`HAL_ERR_BUSY` など）時は、次の tick で 1 回だけ試し直し、それでも駄目なら元の CPU の run queue に戻して `move_pending` を消し、counter に数える。行き先が offline になっていたら元に戻す。
6. 手渡しの間の起床・割り込み: thread は RUNNABLE なので `sched_wakeup` は何もしない（:552）。`sched_interrupt` の通知は元の CPU に行くが害は無く、thread は行き先で走り始めた時に user への戻りで signal を見る。`sched_set_cpu`・盗みは `need_migrate != 0` を見て触らない（:1721・:2496）。

新しい task の入口から始まる後継は `complete_retired` を呼ばない（:960 の comment）。その時の手渡しは次の tick（1 ms 以内）の `complete_handoff` が片付ける。

#### 4.6.6 計数

`sched_hint_counters`: 置いた hint の数、`sched_hint_place` の移動の数・EAGAIN の数、手渡しの数・試し直し・失敗、飢えの防ぎの上げの数。CPU ごとの field に足し、読む時に合わせる（lock 無し、近似でよい）。

### 4.7 nice と priority（Q302-3）

- 推しの段（DTB が on の時だけ）:

| nice | base の priority |
| --- | --- |
| -20〜-10 | 6 |
| -9〜-1 | 7 |
| 0 | 8（R4・R5 の上げは 6・7） |
| 1〜9 | 9 |
| 10〜20 | 10 |

  R1 の thread は nice の段と R4・R5 の上げの高い方。R2（nice > 0）は上げを受けない。kernel の thread（`USB_BT_SCO_PRIORITY 4` など 0〜5）より上には user の thread を置かない（6 を上限）。
- **飢えの防ぎ**（sched の側、`#if CONFIG_DTB`）: tick ごとに、その CPU の `base_priority` が 8 より下（数が大きい）の段と、今走っている thread より下の段の queue の先頭を見て、`queued_tick` から `SCHED_HINT_STARVE_MS`（推し 100 ms）以上待った thread を、今走っている thread の段の `woken[]` に 1 回だけ上げて（`priority` だけを変える、§4.6.1）`preempt` を立てる。見るのは段ごとの先頭だけ（最大 16）。
  これで、厳密な priority の下でも、下げた thread は 100 ms ごとに 1 quantum（10 ms）は走る（全部の CPU が上の段の忙しい thread で埋まった最悪の時、CPU の約 9%）。nice の「重み」の意味（Unix の分け前）とは違い、段＋上限の近似。
- 選ばなかった案: (a) nice をそのまま 16 段に写す（-20〜20 を 0〜15 に。kernel の thread を越え、飢えが深い）、(b) sched を重み（分け前）の方式に作り直す（範囲の外、ws.md「sched の作り直しは含まない」）、(c) 飢えの防ぎを DTB の周期に任せる（DTB の thread が遅れると防ぎが止まる）。
- nice の変更が効くまで: 次の DTB の周期（最大 100 ms、AWAY では 1 s）。`setpriority` から DTB を起こす口は足さない（syscall の道を DTB に繋げない）。

### 4.8 5330（P 2・E 8）の割り付けの方針

- CPU の数: P-core 2（SMT で論理 4）・E-core 8（4 つ組 2 つ、SMT 無し）、計 12（WS333 design §3）。番号の並び（P が先か）は未確認（§12）。DTB は番号に頼らず、cpuperf の `core_class` と `core`（SMT の兄弟）で表を作る。
- `rank` の決め方: `relative` が分かれば（`CONFIG_CPU_PERF=y` で strategy の後、5330 の見込み P 100・E 75）`rank = relative × 10`。兄弟が忙しい P は `sibling_rank = rank × DTB_SMT_SHARE_PERMILLE / 1000`（初め 600、p006 で測って直す）。`relative` が UNKNOWN なら種類だけで P 1000・E 700・兄弟の忙しい P 600 とおく（E の idle が P の兄弟の忙しい CPU より先、という順だけが効く）。HFI の `available` が 0 の CPU は `usable = 0`（熱で避けろと言われた CPU）。
- 定まった姿（期待、p006 で確かめる）:

| 場面 | P（論理 4） | E（8） |
| --- | --- | --- |
| desktop が何もしない | compositor の thread（眠っている） | 何もしない |
| terminal で打つ | terminal の app（GPU）・compositor・shell（R6 の PREFER） | — |
| game（GPU）＋compositor | game の main と compositor の render を別の P-core に（兄弟の idle を先に） | game の worker（BUSY） |
| build（-j12）だけ | 溢れた compiler（idle の P が盗む） | compiler 8 |
| build＋GPU の app を使う | GPU の app・compositor（priority 6・7 が build の 8 を横取り） | compiler。押し出された compiler は idle の E か P の空きで |
| 背景の計算 8 本（S2） | 空く（P の package の電力が下がる） | 計算 8 |

### 4.9 DTB の周期の処理（dtb.c）

1. thread「dtb」（`kthread_create`、priority 5。user のどの段より上で、kernel の既定の 8 の thread より上）。`kern_dtb_init()` を `entry.c` の `kern_thermal_init()` の後で呼ぶ。`dtb=off` の起動の parameter では作らない（計数は取り続ける。on と off の比べを同じ image でするため）。
2. 周期ごと（`kern_ms_to_ticks(DTB_PASS_MS)`、AWAY は `DTB_PASS_AWAY_MS`、`kern_sleep` で眠る）:
   1. `kern_cpuperf_generation()` が前と違えば、全 CPU の `kern_cpuperf_cpu(cpu, 0, &info)` を読み、§4.8 の表を作って `sched_cpu_classes_set`。FAST と EFFICIENT が両方そろわなければ表は全部 ANY（priority の hint だけが効く）。
   2. HID の時刻から ACTIVE・QUIET・AWAY。
   3. `process_find_next_ref(cursor)`（process.c:773）で process を巡る。process0（kernel の thread）と終わりかけの process は飛ばす。nice・tty・GPU と表示の時刻を読み、`process->lock` の中で thread を最大 `DTB_THREADS_PER_PROCESS`（64）まで `thread_ref` して手元の配列に取り、lock を離す（64 を超える process は次の周期に続きから）。
   4. thread ごとに `sched_hint_activity` を読み、前回の値（`struct dtb_thread_state`、DTB だけが書く）と比べて印を更新し、`dtb_policy_classify()` で hint を作る。前と違えば `sched_hint_set`。PLACE で希望と違う種類の CPU に居り、前の移動から `DTB_MOVE_INTERVAL_MS`（500 ms）経ち、この周期の予算（移動 `DTB_MOVES_PER_PASS` 32、うち手渡し 8）が残れば `sched_hint_place`。`thread_release`。
   5. 周期の時間を計数（最大と合計）に足す。
- 代価の見積もり: desktop の thread 数百で、process ごとに lock 1 回・thread ごとに atomic の load 数個。1 周期 数十 µs（p004 で最大を測り、受け入れは 1 ms 未満）。
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
 RUNNING ──place──> RUNNING+move_pending ──notify/tick──> HANDOFF（RUNNABLE, MIGRATING, cpu->handoff）
   ^                     |  行き先無し: 印を消す                 | 後継の complete_handoff
   |                     v                                       v
   +─────────────── RUNNING                           RUNNABLE on target（移動の終わり）
                                                        | transfer 失敗 2 回: RUNNABLE on old
```

### 4.11 資源と寿命、config、起動の parameter

- 資源: DTB の thread 1 本、CPU の種類の表（`kern_calloc`、CPU 数、kernel の寿命）、thread の参照の配列（64、static）。thread と process の field は各 struct に埋める（寿命は持ち主と同じ）。DTB が持つ参照は 1 周期の中で離す。
- config: `config/kernel-options.list` に `CONFIG_DTB|bool|Desktop thread balancer (core-kind and priority hints to the scheduler)|amd64|n|`、`Makefile` の `ZEDBSD_CONFIG_CPPFLAGS` に `-DCONFIG_DTB=0/1`、`platform/amd64/vmunix.mk` に `src/kern/dtb.c src/kern/dtb-policy.c`（`CONFIG_DTB=y` の時）。`include/kern/dtb.h` は `CONFIG_DTB` が定義されていなければ 0 とする（`ZEDBSD_CONFIG_CPPFLAGS` を使わない 3 つの platform のため）。platform の欄は amd64 だけ（hybrid の対象が 5330 だけ。code は arch に依らない）。
- 試験の config: `CONFIG_DTB_TEST`（bool、amd64、n、試験の build だけ）。有効の時だけ起動の parameter `dtb.classes=PPEE…`（hybrid でない QEMU で種類の表を偽る）と `dtb.stress=1`（周期 10 ms で thread を無作為に寄せ合う。手渡しの競争の試験）を読む。
- 起動の parameter（製品）: `dtb=off`（診断の switch、方針ではない）。`boot.h` の enum と `boot.c` の名前の表に足す（WS333 の `cpuperf=off` と同じ形）。

### 4.12 並行性と lock

```
 DTB の thread:  process->lock（rank 20、thread_ref を取るだけ）→ 離す
                 → sched_hint_set / sched_hint_place（thread の CPU の lock、rank 200）
                 → kern_cpuperf_cpu（box の spinlock、rank 125）は sched の lock を持たずに
 sched_hint_place（走っていない）: sched_set_cpu の今の lock の順（元の CPU → 離す → transfer → 元の CPU → 離す → 行き先）
 手渡し:          元の CPU の lock の中で印と行き先 → 離して切り替え → 後継: transfer → retarget（元の lock）→ 行き先の lock
 計数:            tick（CPU の lock の中）、syscall の入口（割り込みの窓）、wakeup（CPU の lock の中）、note_gpu（lock 無し、atomic）、note_input（subscriber の lock の中、atomic だけ）
```

- 2 つの CPU の lock を同時に持たない（sched の今の規則を保つ）。
- DTB の読みは近似でよい（古い値は 1 周期の遅れ）。書き手が 1 つの field だけを atomic で読む。
- `struct dtb_thread_state` は DTB の thread だけが書く（1 本しか無い）。

### 4.13 割り込みと DMA

- 新しい割り込み・DMA は無い。`kern_dtb_note_input()` は input の report を配る文脈（割り込みの下半分か driver の thread。irqsave の lock の中）から呼ばれうるので、atomic の store と加算だけにする。手渡しは既存の CPU の通知（`hal_cpu_notify`）を使う。

### 4.14 失敗と回復

| 失敗 | 振る舞い |
| --- | --- |
| `kthread_create` の失敗 | log 1 行、DTB は無いまま（計数は取る）。sched は hint が無いので今と同じ |
| cpuperf の provider が無い・hybrid でない | 種類の表は ANY。priority の hint だけ |
| `kern_cpuperf_cpu` が EINVAL | その CPU を ANY |
| `sched_hint_place` の EAGAIN・EBUSY | 動かさない。次の周期に試す（`DTB_MOVE_INTERVAL_MS` を守る） |
| 手渡しの transfer の失敗 | §4.6.5 の 5（1 回試し直し、元に戻す） |
| HFI が P を避けろと言う（熱） | P の `usable = 0`。FAST の thread は E の rank の順で置かれる。PLACE の移動は行き先が無いので EAGAIN |
| CPU が offline（system の sleep） | `cpu_online` を見て行き先にしない。sleep の間 DTB の thread は眠る |
| thread・process が周期の途中で終わる | 参照で struct は残る。`sched_hint_set` は ZOMBIE・DEAD に EINVAL |

### 4.15 観察（Q302-6）

- 5330 の評価には「どの CPU がどの種類か」と「DTB が何回寄せたか」を user から読む口が要る（ログで判定しない規則、AGENTS.md §9）。推し: 読みだけの sysctl `kern.dtb`（`KERN_DTB 11`、`include/uapi/sysctl.h` の KERN の次の番号）。中身: version・size・flag（built・enabled・hybrid・状態）、周期の数と時間の最大、印ごとの thread の数、`sched_hint_counters` の全部、CPU の数と CPU ごとの種類・rank。process ごと・thread ごとの印は v1 に入れない。
- 書きの口（on・off の切り替え、thread の自分の hint、前面の app）は足さない（Q302-4・Q302-6）。

### 4.16 既存の framework との関係

- **cpuperf（WS333）**: 読むだけ（§2.5）。DTB は周波数・strategy に触らない。thermal の受け手にもならない。HFI の値は熱で P を避ける時に効く（§4.14）。
- **thermal**: 関係なし。
- **drv_gpu（gpu.c）**: `gpu_ioctl` の成功の後に 1 行（§4.3）。GPU の driver（i915 など）は変えない。
- **input（input.c）**: `drv_input_subscriber_publish` の頭に 1 行。
- **cdev・PCI・USB**: 関係なし。
- **HAL**: §5。
- **Keiland（WS309）**: 変えない。thread の台帳（`kwl_thread_start`）は、将来 thread の自分の hint（UAPI）を足す時の置き場所になる（Q302-4 の条件つきの Phase）。

## 5. HAL の検討（変えない）

| 要る事 | 使う口 | 足りる理由 |
| --- | --- | --- |
| 止まっている thread を別の CPU へ | `hal_task_transfer`（hal.h:1035） | 契約は「止まっている task を移す」。`sched_set_cpu`・盗みと同じ使い方 |
| 走っている thread を別の CPU へ | 普通の切り替え（`hal_task_context_switch`）の後に `hal_task_transfer` | 走っている task を HAL に移させない。sched が先に切り替えて task を止め、後継が移す（§4.6.5）。後継の文脈で去った task が止まっているのは HAL の今の実装（amd64 の `amd64_task_finish_switch`）が既に保証し、盗みが頼っている |
| CPU に手渡しを知らせる | `hal_cpu_notify`（sched の `notify_cpu` :2290） | 既存 |
| core の種類・相対・SMT | cpuperf（kernel の口）と x86 の driver（CPUID） | HAL を通らない |
| 時刻 | sched の tick、`kern_ms_to_ticks` | 既存 |

HAL の差分の案は無い（`plan/ws302/proposed/` に hal.h は置かない）。

## 6. 試験の計画

### 6.1 host 試験（`plan/ws302/tests/dtb-host/`、p002〜p004 で作る）

- 分類（dtb-policy.c を host で compile）: 規則の順 R1〜R7 の全部の組（nice -20・-1・0・1・20 × BUSY × PRESENTER × GPU × TTY × 状態）の表を期待の表と照らす。BUSY の履歴の幅（200 ms で立ち、20 ms 未満が 10 周期で消える、途中の揺れで消えない）。GPU・表示の窓の端。
- nice の段（-20〜20 の全部）と R1 の高い方の規則。
- 行き先の選び（`sched_hint_choose`）: 5330 の形（P 論理 4・E 8）と偽の形（PPEE、全部 P、全部 UNKNOWN、P が usable 0）で、idle と backlog の bit の組を回し、§4.6.3 の順と一致。
- 飢えの防ぎの模型: queue の先頭の待ちと段の組で、上げる・上げないの判定。
- 突然変異（比べの向き・閾値を 1 つずつ壊して検出する事）を各試験の README に書く。

### 6.2 QEMU（amd64、Q1 が割り当てた名前と port、判定は SSH と boot-test.sh。console・serial の log では判定しない）

1. `CONFIG_DTB=n` の image の起動（boot-test.sh の login）。
2. `CONFIG_DTB=y` の image の起動（同）と、`dtb=off` の起動。
3. `CONFIG_DTB=y CONFIG_DTB_TEST=y`、SMP 4、`dtb.classes=PPEE`: 忙しい loop 2 本（試験の program、syscall 無し）を走らせ、SSH で `hw.cputimes` の差と `kern.dtb` を読み、忙しい時間の 90% 以上が E（CPU 2・3）にある事と、手渡しの数 > 0 を確かめる。
4. 同じ image に `dtb.stress=1` で `plan/tools/kstress/` の iostress・credrace・sigexec を 30 分。止まらない・panic しない（固まったら gdbstub と QMP で解析）、手渡しと移動の数 > 0。
5. nice: 忙しい loop 2 本（nice 0 と nice 10）を 1 CPU の QEMU（SMP 1）で 10 秒。nice 10 の方の CPU 時間が 0 でなく（飢えの防ぎ）、nice 0 より小さい。

### 6.3 5330 の実機（1 本の script `plan/ws302/tests/5330-dtb.sh`: 配置 → 起動 → 実行 → 回収 → 要約）

- image: `plan/ws302/tests/config-amd64-dtb.mk`（`CONFIG_DTB=y`、`CONFIG_CPU_PERF=y`、`CONFIG_INPUT_TEST_INJECT=y`）。比べは同じ image の `dtb=on`（既定）と `dtb=off` を交互に 5 回ずつ。`CONFIG_DTB=n` の image で負担を 5 回。
- 電力の器具: WS333 の RAPL の meter（p011）を `/dev/thermal` で読む（p005 の後半・p013 の後）。それが無い時は WS291 p022 の診断の patch（Q1、§10）。
- 場面（各 60 s、計測は後半 40 s）:

| 場面 | 作業 | 測る物 |
| --- | --- | --- |
| S1 何もしない desktop | — | package の電力、DTB の周期の時間 |
| S2 背景の計算 | 忙しい loop 8 本（nice 0、syscall 無し） | package の電力、CPU の種類ごとの忙しさ（`hw.cputimes`＋`kern.dtb` の種類）、pointer → cursor の flip の遅れ（`plan/tools/i915-hw/hdmi/c6.py` の物差し）、打鍵 → 表示（`plan/tools/frame-ops/key-latency.py`） |
| S3 terminal の build | 固定の C の source の tree を image の cc で `make -j12` | build の時間 |
| S4 build＋GPU の app＋打鍵 | S3 ＋ mview（`plan/tools/gpu-perf/` の c600 の設定）＋ aat-input の打鍵 | mview の fps、frame の時間の中央値と p95（Keiland の `--log-frames`、`frame-times.py`）、打鍵 → 表示、build の時間 |
| S5 nice の背景 | 忙しい loop 8 本を nice 10、前で S4 の mview | mview の fps、loop の CPU 時間（0 でない事） |

- 同じ session で A/A（off と off）の揺れを 1 回測り、受け入れの「同等」の幅に使う。

### 6.4 受け入れの数（Q302-7 の推し）

| # | 物差し | 合格 |
| --- | --- | --- |
| A1 | S2・S4 の cursor の遅れと打鍵 → 表示の中央値・p95 | on ≤ off × 1.05 ＋ 1 ms（A/A の揺れの方が大きければその幅） |
| A2 | S4・S5 の mview の fps と frame の時間の中央値・p95 | fps は on ≥ off × 0.95、frame の時間は A1 と同じ |
| A3 | S2 の忙しい時間の置き場所 | on で E の CPU に 90% 以上 |
| A4 | S2 の package の電力（40 s の平均） | on ≤ off × 0.95 |
| A5 | S3 の build の時間 | on ≤ off × 1.10 |
| A6 | 負担: `CONFIG_DTB=n` と y の `dtb=off` の S1 の電力と S3 の build の時間、DTB の周期の時間の最大 | 差が 2% 以内（揺れの中）、周期の最大 < 1 ms |
| A7 | 安定 | §6.2 の 4 が 30 分 PASS、5330 の全部の run で固まり・panic 無し |

## 7. Phase の分け方と受け入れ（ws.md の表の本体はこの表。±50%、1 LW ≈ 実時間 15〜25 分）

| Phase | 内容 | 依存 | LW | 受け入れ | 担当の型 |
| --- | --- | --- | --- | --- | --- |
| p001 | この設計、design-reviewer、ユーザーの問い | — | — | review の反映、Q302-1〜7 の答え（ユーザーの review） | 設計（P5） |
| p002 | config（`CONFIG_DTB`・`CONFIG_DTB_TEST`・Makefile・kernel-options・amd64 の vmunix.mk）、`include/kern/dtb.h`・`sched-hint.h` の型、統計（thread の計数・syscall の入口・tick・wakeup、process の GPU と表示の時刻、`kern_dtb_note_gpu`・`kern_dtb_note_input` と gpu.c・input.c の 1 行）、`sched_hint_activity` | p001 | 3 | 全 platform の vmunix の build（n と y）warning 0、`CONFIG_DTB=n` の object が今と同じ（sched.o・syscall.o・gpu.o・input.o の text の大きさが変わらない）、QEMU の起動（y と n） | Opus（phase-runner-high） |
| p003 | sched の口: `base_priority` と `sched_hint_set`、種類の表、行き先の選び（純な関数と host 試験）、置き場所・盗み・tick の助けの好み、`sched_hint_place`、手渡し、飢えの防ぎ、計数 | p002 | 6 | host 試験（§6.1 の行き先と飢え）、build warning 0、QEMU の起動（y、DTB の本体なしで hint 無し＝今と同じ） | Opus（phase-runner-high） |
| p004 | DTB の本体: dtb.c・dtb-policy.c（分類・履歴・nice の段・HID の状態・周期・予算）、`dtb=off`、`CONFIG_DTB_TEST` の偽の種類と stress、`kern.dtb`（Q302-6 の答えの形） | p003、Q302-1〜3・5・6 の答え | 6 | host 試験（§6.1 の分類・nice）、build warning 0、QEMU §6.2 の 1〜5 | Opus（phase-runner-high） |
| p005 | （条件つき）compositor からの前面の app の hint（UAPI・backend・WS309 の台帳）。Q302-4 で「作る」か、p006 で A1・A2 が通らず前面の情報で直る見込みがある時だけ立てる | p004、UAPI の決定 | 3 | — | — |
| p006 | 5330 の評価: `5330-dtb.sh`（§6.3）、閾値（`DTB_*`・`sibling_rank`・飢えの上限）の調整、5330 の CPU の番号の並びの記録 | p004、WS333 p011（meter と相対）・p013（読み）、5330 の割当（Q1） | 4 | §6.4 の A1〜A7 | Opus＋Q1 の実機の割当 |
| p007 | 規約の全文の見直しと文書（`docs/architecture/` に DTB の説明） | p006 | 2 | 規約、文書 | conformance-reviewer・document-writer |

合計 約 21 LW（p005 を立てれば 24）。ws.md の旧い表（約 32 LW）は、thread の計数と sched の口を分け直し、WS301 の前提の Phase が WS333 で済んだので小さくなった。

## 8. 選ばなかった案（主な物）

| 案 | 選ばなかった理由 |
| --- | --- |
| DTB を sched の中に入れる（分類も sched がする） | ユーザーの決定（DTB は sched ではない）。sched の hot path に分類の代価が乗る |
| sched が cpuperf を直に読む | lock の rank（sched 200 の中で box 125 は取れない）と、sched を cpuperf に依らせる層の向き |
| DTB が行き先の CPU を決めて `sched_set_cpu` を直に呼ぶ | 決めてから移すまでに CPU の空きが変わる。行き先の選びは CPU の状態を見る sched の仕事 |
| 起床の時に種類の違う CPU へ移す | 起床の道（割り込みの文脈を含む）に移動を足すと hot path が重くなり、起床ごとに CPU の間の通知が増える。同じ priority の起床は今も横取りで走れる。種類の違いは周期の移動で直す（最大 100 ms の遅れ） |
| 走っている thread を HAL に移させる（hal の契約を広げる） | §5 のとおり要らない |
| DTB を input の subscriber にする | kernel が driver の関数を呼ぶ向きになり、input.c の無い platform で link が切れる。枠（最大 8）も食う |
| GPU の process ごとの busy の時間（完了の時刻から） | GPU の driver ごとに完了の道が違い、代価が大きい。提出と表示の時刻で v1 の規則は足りる |
| thread の名前（compositor の thread の種類）を kernel に足す | UAPI が要り、WS309 との調整も要る。v1 は kernel の統計だけで見分ける |
| 統計を常に取る（`CONFIG_DTB=n` でも） | n の kernel を今と同じに保つ方が受け入れ（A6）を示しやすい。Q302-5 |

## 9. 出典と引用

- 外部の source・仕様は使っていない。値（閾値・段）は zedBSD の設計の初めの値で、p006 の実測で直す。source の comment に開発の管理情報を書かない（guardrail）。
- 5330 の CPU の数と HWP の能力は WS333 design §3（Debian での読み）から。

## 10. 判断が要る点

### 10.1 ユーザー（[user-questions.md](user-questions.md) に背景・用語・案・推し）

| 問い | 要点 | 推し |
| --- | --- | --- |
| Q302-1 | 規則の強さの順（ws.md の D2） | A: nice → 忙しい計算 → 画面を作る process → GPU の app → terminal |
| Q302-2 | 忙しい計算（nice 0）が空いた P に溢れてよいか | A: 溢れてよい（nice > 0 は溢れない） |
| Q302-3 | nice の段と飢えの防ぎ、nice を DTB の on の時だけ効かせるか | A: §4.7 の 5 段、100 ms の防ぎ、DTB の on の時だけ |
| Q302-4 | compositor から前面の app を知らせる口（D4） | B: v1 では作らず、p006 の数で要れば条件つきの Phase |
| Q302-5 | 統計を DTB の build の時だけ取るか（D5） | A: `CONFIG_DTB=y` の時だけ |
| Q302-6 | 観察の UAPI | A: 読みだけの sysctl `kern.dtb` |
| Q302-7 | 受け入れの数 | §6.4 の表のとおり |

### 10.2 Q1

1. master.md の WS302 の行（「p001 検討（WS301 p001 の後）、約 32 LW」）を、この設計の Phase（約 21 LW、依存は WS333 p011・p013）に直す。
2. 共有の file の所有の調整: `src/kern/sched.c`・`include/kern/sched.h`・`include/kern/thread.h`・`include/kern/process.h`・`src/kern/syscall.c`・`src/kern/entry.c`・`include/kern/boot.h`・`src/kern/boot.c`（WS333 p011 も触る）・`src/drivers/gpu/gpu.c`・`src/drivers/generic/input.c`・`Makefile`・`config/kernel-options.list`・`platform/amd64/vmunix.mk`・`include/uapi/sysctl.h`・`src/kern/sysctl.c`。
3. p006 の電力の器具: WS333 p011・p013 を待つか、WS291 p022 の診断の patch で先に測るか。
4. p006 の 5330 の割当。
5. p004 の着手は Q302-1〜3・5・6 の答えの後（p002・p003 は答えに依らず進められる。p003 の盗みの溢れは Q302-2 の推しで作り、答えが B なら flag 1 つで止める）。

## 11. Resume point

p001: この文書と問いを書いた。design-reviewer の反映（§14）の後、ユーザーの答えを待つ。p002・p003 は答えに依らず着手できる（Q1 が Queue を立てる）。

## 12. 未確認

1. 5330 の zedBSD での CPU の番号の並び（P が先か）と、`core` の番号で SMT の兄弟が対になっているか（p006 の最初に `kern.dtb` で記録）。
2. 5330 で P の兄弟が忙しい時の 1 thread の速さ（`DTB_SMT_SHARE_PERMILLE` の 600 は仮の値）。
3. amd64 以外の HAL で、後継の文脈で去った task が `hal_task_transfer` を受けるか（盗みと同じ前提。DTB は amd64 だけで有効にするので v1 の範囲では問題にならない）。
4. 各 thread の実際の負荷（compositor の thread が BUSY にならない事）。p006 で `kern.dtb` の印の数と移動の数で見る。
5. `make -j12` の build の中で compiler が BUSY の閾値（200 ms）を越える割合。

## 13. 設計の判断の根拠（補い）

- 「起床の道を変えない」: 今の sched は同じ priority の起床でも横取りを頼む（§2.1）ので、P に戻った UI の thread は待たずに走る。UI の thread を P に置くのは DTB の周期（眠っている間に `sched_set_cpu`）で足り、起床ごとの移動の代価（CPU の間の通知・cache の冷え）を払わない。
- 「PREFER と PLACE を分ける」: P が 4 つしかない 5330 で、弱い候補（terminal）まで能動的に寄せると、UI の thread の席が無くなる。弱い候補は空いている時だけ。
- 「BUSY の溢れを許す」: build の時間（A5）を守る仕組み。電力（A4）の場面 S2 は 8 本で E がちょうど埋まるので、溢れは起きない。

## 14. design-reviewer の指摘と反映

（review の後に書く）
