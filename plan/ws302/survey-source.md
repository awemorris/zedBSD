<!-- awesome-plan project=zedbsd record=ws302-survey-source -->

# WS302 の source の棚卸し（設計の入力、2026-10-29）

Parent: [WS302](ws.md)。q1666 の P5 の配下の調べ（Opus、調べだけ）の結果を P5 がまとめた。tree は main 61630bb0b、行はその時の物。
「推測」と書いた所は確かめていない。p001 の設計はこれを出典の一つにして、行を引き直してから書く。

## 0. ws.md の食い違い（Q1 へ）

- ws.md:21 のユーザーの決定「nice は core の選択に加えて sched の priority にも効かせる」「CONFIG_KERN_DESKTOP_BALANCER の既定は off」と、:45「含まない: nice を sched の priority に効かせる事（D3）」・:93 の D1・:95 の D3 の判断待ち・:64「既定は p001 の後に決める」が食い違う。原文（:21）が正のはず。直しは p001 の版で行う（Q1 の確かめの後）。
- ws.md:34 の `setpriority` の行は今 `syscall.c:8863-8935`（getpriority 8808）、:36 の `syscall_dispatch_body` は 9796。

## 1. kernel の scheduler（`src/kern/sched.c`、`include/kern/sched.h`）

- CPU ごとの `struct sched_cpu`（sched.c:177-208）、CPU ごとの spinlock 1 本（:371-373）。16 段の priority ごとの `run[]`・`woken[]`、時間付きの sleep の queue、idle、current。global な queue は無い。
- priority 0 が最高・15 が最低・既定 8（sched.h:22-25）。厳密に上の段から取る（`pick_next_locked` :2166-2191）。aging・減衰は無い。
- user の thread は全部 8（thread.c:102, 188, 408）。kernel の thread は `kthread_create` の引数（thread.c:222-254）、既定でないのは `USB_BT_SCO_PRIORITY 4`（usb-bt.c:101）だけ。走った後に変える関数は無い。class（FIFO・RR）は無い。
- quantum 10 ms（sched.h:31-34）、tick で減らし 0 で yield（sched.c:1068-1074, 1134-1135）。amd64 は 1000 Hz。
- preemption: 起こされた thread が同じか高い priority なら woken の queue と `preempt`（`wake_enqueue_locked` :2024-2051、全部 8 の今はどの wakeup も preempt を頼む）。切り替わる所は user への戻り（signal.c:415 → `sched_preempt_point` :884-901）・tick（:1083-1088）・IPI（`sched_cpu_notify` :1764-1800）。
- 新しい thread の置き場所 `sched_prepare_thread`（:396-440、`choose_cpu` :2243-2261）。wakeup は元の CPU にしか戻さない（`sched_wakeup` :519-575）。
- load balance は idle の CPU の盗み（`sched_idle` :1551-1593 → `steal_runnable` :2528-2599、`take_stealable_locked` :2453-2505）と、忙しい CPU が tick で idle の CPU を 1 つ起こす（:1097-1116）だけ。idle の CPU は番号の小さい順（`choose_idle_cpu` :2364-2389）。
- 移動: `sched_set_cpu`（:1665-1758、走っていない thread だけ）、盗み、`retarget_migrating`（:2507-2522）。2 つの CPU の lock を同時に持たない。amd64 の `hal_task_transfer` は走っている task と IRQ の待ちを持つ task を `HAL_ERR_BUSY`（`src/hal/amd64/task.c:831-866`）。
- affinity・cpuset は無い。hybrid（P/E core）を知らない（`hybrid`・`core_type`・`capacity` の grep が 0 件）。

## 2. user の口

- `setpriority`・`getpriority`（syscall 152・153、syscall.c:8808-8935、libc posix.c:1899-1964）は process の `nice_value`（process.h:129、fork で継ぐ process.c:1107）に入れるだけで sched は読まない。範囲は -20〜+20（POSIX は 19）。
- `sched_yield` はある（syscall.c:8674）。`sched_setscheduler`・`pthread_setschedparam`・affinity・`pthread_setname_np` は無い（include/libc/sched.h:20-29）。`pthread_attr_setschedparam` は priority 0 以外 ENOTSUP（pthread.c:1014-1027）。
- `thread_create` の args[4] は 0 以外 EINVAL（syscall.c:7686-7692、pthread.c:496）: 作る時の属性に使える空き。`thread_self` は op の形（uapi/thread.h:11-13、syscall.c:7938-7957）。
- `/dev/system` の ioctl は 'S' 1〜21（uapi/system.h:272-464、system-device.c:500-560 付近）。backend は既に開いている（events-zedbsd.c:46、monitor-zedbsd.c:96、power-zedbsd.c:226）。

## 3. 計測

- process ごとの tick の時間（sched.c:1036-1052）、`KERN_SYSTEM_GET_PROCESS`（system.h:68-88、`reserved[4]` あり）、CPU ごとの `hw.cputimes`（sysctl.h:71-104）。
- thread ごとの時間・待ち・移動・switch の数は取れない。`getrusage` は utime・stime だけ（syscall.c:8974-8987）。`kern.perf=1` は klog の文字（sched.c:131-176, 2609-2950）。
- 足す所: syscall の入口と出口（syscall.c:10427, 10498）、`perf_switch_locked` の 4 か所、`wake_enqueue_locked`、移動の最後。

## 4. Keiland の thread

compositor・backend・libvulkan に priority を設定する所は無い（全部 `pthread_create(..., NULL, ...)`）。今の thread: main（wayland/main.c）、render（render-thread.c:136）、cursor（cursor-thread.c:89）、input（input-thread.c:133）、glyph（glyph-worker.c:64）、glass の prefetch・loader（glass.c:424, 1916）、keyboard-hand の loader（:301）、sysmon（sysmon.c:379）、heads・settings の writer（heads.c:1859、settings-store.c:442）、system の job（system.c:1836, 3339）、machine-shell（:339）、game の scanout（scanout-zedbsd.c:538）、libvulkan の present worker（wsi-swapchain.c:3687）・overlay の watcher（wsi-display.c:3515）。WS309 の未実装: 画面の制御・daemon ×5・blur・照合の pool・spawner。
hint は各 thread の入口で自分に付けるのが最小。OS 固有の呼び出しは backend に閉じる（guardrail.md:34）。thread の種類の enum は WS309 と同時に決めると手戻りが少ない。

## 5. hint の UAPI の案（調べの案、推しは推測）

| 案 | 形 | 長所・短所 |
| --- | --- | --- |
| A | `thread_self` の op `SET_HINT`・`GET_HINT`（自分の thread だけ） | 番号も権限の判定も要らない。他の thread には付けられない |
| B | 新しい syscall（tid と構造体） | 一般的。権限の規則が要る（setpriority の euid の判定 syscall.c:8904-8925 の形） |
| C | `/dev/system` の ioctl（SET と thread ごとの GET） | WS301 の UAPI の置き場所と揃い、thread ごとの観察の穴も埋める。権限は呼び手の cred で |
| 補い | `thread_create` の args[4] と `pthread_attr_*_np` | ABI と、compositor に OS 固有の API を書けない点 |

調べの推し（推測）: 設定は A、観察は C の GET。nice → priority は UAPI が要らない。

## 6. 設計に効く制約

1. 厳密な priority で aging が無い: nice をそのまま段にすると飢えが起きる（重み・上限の設計が要る）。
2. wakeup で CPU を選び直さない。hint が効く所は wakeup・新しい thread・盗み・idle の CPU の選びの 4 つ。
3. 走っている thread を移せない（HAL も断る）。switch の後に移す仕組みが要る。
4. thread ごとの計測が無い（1 ms の tick だけ）。
5. topology を知らない（WS301 待ち）。
6. thread の名前が kernel に無い。
7. `CONFIG_KERN_DESKTOP_BALANCER` の配線は 14 の platform の vmunix.mk か `config/kernel-options.list`。
8. Linux・FreeBSD の backend にも同じ関数（空でよい）。

## 7. 未調査（調べを止めた、2026-10-29 Q1 の指示）

5330 の CPU の番号の並び（実機）、amd64 以外の `hal_task_transfer`、system-device.c の権限と node の mode、WS309 の未実装の thread の置き場所、各 thread の負荷（計測なし）。
