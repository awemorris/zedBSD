<!-- awesome-plan project=zedbsd record=ws302-p002 -->

# ws302-p002: config・型・統計（DTB の土台）

Status: in-progress
Parent: [WS302](../ws.md)
Queue: q2093-i01（P6）

## 範囲

[design.md](../design.md) 第 3 版の §4.3（統計）・§4.5（型）・§4.6.1（`base_priority`）・§4.11（config）のうち、sched の振る舞いを変えない土台だけ。

- config `CONFIG_DTB`（既定 n）の mk の案（下）。Makefile・mk は変えない。
- `include/kern/dtb.h`: `CONFIG_DTB` の既定 0、GPU の通知の種類、process の GPU と表示の時刻（`struct dtb_process_activity`）、`kern_dtb_note_input()`・`kern_dtb_note_gpu()`・`kern_dtb_input_activity()`（n では空の inline）。
- `include/kern/sched-hint.h`: hint の型（core の種類・強さ・flag、`struct sched_hint`・`struct sched_cpu_class`・`struct sched_hint_activity`・`struct sched_hint_counters`・`struct sched_hint_state`）と、この Phase の関数 `sched_hint_activity()`・`sched_note_syscall()`（n では空の inline）。hint を書く・動かす関数（`sched_hint_set`・`sched_hint_place`・`sched_cpu_classes_set`・`sched_hint_choose`・`sched_hint_counters`）は p003 で足す。
- `src/kern/dtb.c`（`CONFIG_DTB=y` の時だけ build）: 入力の時刻と数、GPU の時刻の書き込み。DTB の thread は p004。
- 統計: thread の `run_ticks`・`user_streak`（tick、`elapsed` で）、`syscalls`（syscall の入口）、`wakeups`（`wake_enqueue_locked` と `sched_set_cpu` の持ち越しの起床）。process の `gpu_submit_tick`・`present_tick`（gpu.c の `gpu_ioctl` の成功の後）。入力の時刻と数（input.c の普通の入力の 2 つの関数の最後）。
- `base_priority` の初め（thread.c の 3 か所と `kthread_create`）。
- 受け入れ: `CONFIG_DTB=n` の kernel は今と同じ（統計も取らない、Q302-5）。x86_64・arm64（rpi5）・riscv64（vf2）の vmunix の build が n と y で warning 0。QEMU の起動の確かめ（boot-test.sh）。

## mk の案（ユーザーの承認まで当てない）

`mk/config.mk`（「Thermal Management」の節の後に節を足す）:

```make
################################################
# Scheduling
################################################

# [Desktop Thread Balancer]
#  - Description: Hint the scheduler which kind of core and which priority
#    each user thread should have, from what threads do and from the input
#    and GPU activity.  Core kinds come from the thermal core
#    (CONFIG_KERN_THERMAL; on x86 also CONFIG_DRIVER_X86_POWER_CONTROL);
#    without them only the priority hints work.
CONFIG_DTB                        ?= n
```

`mk/kernel.mk`（「[Kernel Thermal core]」の block の後）:

```make
# [Desktop Thread Balancer]
ifneq ($(filter y,$(CONFIG_DTB)),)
KERN_CPPFLAGS += -DCONFIG_DTB=1
KERN_SOURCES += \
	src/kern/dtb.c
endif
```

- 名前: ユーザーの決定（2026-10-27）の `CONFIG_DTB` で出す。新しい mk の作法（`CONFIG_KERN_*`）に揃えるか、device tree の dtb と紛れない名前にするかを一緒に聞く（design.md の D-13）。名前を変えるなら source の `CONFIG_DTB` も同じ commit で変える。
- p004 で `src/kern/dtb-policy.c`、p003 で試験の `CONFIG_DTB_TEST`（`mk/test-config.mk`・`mk/test-kernel.mk`）の案を足す。

### 案が当たる前の y の build の確かめ方

案と同じ効果を、mk を変えずに make の command line で与える: 試験の makefile [../tests/dtb-overlay.mk](../tests/dtb-overlay.mk) を Makefile の後に読ませる（`make -f Makefile -f plan/ws302/tests/dtb-overlay.mk CONFIG_DTB=y …`）。overlay は `CONFIG_DTB=y` の時に `KERN_CPPFLAGS` に `-DCONFIG_DTB=1` を足し、`$(BUILD)/src/kern/dtb.o` を `VMUNIX_OBJS` と link の前提に足す（各 platform の link の recipe は `$(VMUNIX_OBJS)` を実行の時に展開するので入る）。flag の stamp（mk/tracking.mk）は recipe の時の `KERN_CPPFLAGS` を書くので、n と y を同じ `build/<機種>` で交互に作っても全部が作り直される。

## 進み

（実装の後に書く）

## 結果

（試験の後に書く）
