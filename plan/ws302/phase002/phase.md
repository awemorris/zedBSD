<!-- awesome-plan project=zedbsd record=ws302-p002 -->

# ws302-p002: config・型・統計（DTB の土台）

Status: in-progress（実装済み・試験 PASS、mk の案の承認待ち。cleared は Q1）
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

2026-11-14 P6（q2093-i01、基点 main fd18bd61ee）。

- 新しい file: `include/kern/sched-hint.h`（hint の型、`sched_hint_activity()`・`sched_note_syscall()`）、`include/kern/dtb.h`（GPU の通知の種類、`struct dtb_process_activity`、`kern_dtb_note_input()`・`kern_dtb_note_gpu()`・`kern_dtb_input_activity()`）、`src/kern/dtb.c`（入力の時刻と数、GPU の時刻の書き込み）。どちらの header も `CONFIG_DTB` が無ければ 0 とし、n では呼びが空の inline。
- `include/kern/sched.h`: `sched-hint.h` を include、`struct sched` の最後に `#if CONFIG_DTB` の `struct sched_hint_state hint`。
- `include/kern/process.h`: `dtb.h` を include、`struct process` の最後に `#if CONFIG_DTB` の `struct dtb_process_activity dtb_activity`（`kern_calloc` で 0、fork の子は継がない）。
- `src/kern/sched.c`（全部 `#if CONFIG_DTB`）: tick の「走っている thread の計数」の所で `hint_charge_tick_locked()`（`run_ticks` に `elapsed`、user の時は `user_streak` に `elapsed` を飽和で）、`wake_enqueue_locked()` と `sched_set_cpu()` の持ち越しの起床 2 か所で `hint_note_wakeup_locked()`、公開の `sched_hint_activity()`・`sched_note_syscall()`。
- `src/kern/syscall.c`: 入口の割り込みを止めた窓で `sched_accounting_kernel_enter()` の直後に `sched_note_syscall()`（n では空）。
- `src/kern/thread.c`: `thread_create`・`thread_fork`・`kthread_create`・`thread_init_secondary` で `base_priority` を作った時の priority に（`#if CONFIG_DTB`）。
- `src/drivers/gpu/gpu.c`: `gpu_ioctl()` の成功の後、何も持たない所で `gpu_note_balancer()`（`#if CONFIG_DTB`、提出 5 つと表示 4 つの ioctl を `KERN_DTB_GPU_SUBMIT`・`_PRESENT` に）。
- `src/drivers/generic/input.c`: `drv_input_device_emit_at()`・`drv_input_device_emit_key_event()` の最後、lock を全部離した後で `published` の時に `kern_dtb_note_input()`（n では空）。device の外し・reader の resync の report では呼ばない。
- 試験の道具: [../tests/p002-build.sh](../tests/p002-build.sh)（n・y の build と基点との比べ）、[../tests/dis-compare.py](../tests/dis-compare.py)（逆 assemble の比べ: 同じ行・同じ番地・同じ命令で、違うのが基点と今の file の同じ行を指す fatal の __LINE__ だけなら同じ kernel）、[../tests/p002-boot.sh](../tests/p002-boot.sh)（text login の image で n・y の boot-test）、[../tests/dtb-overlay.mk](../tests/dtb-overlay.mk)。

- overlay の注（review 第 3 版 m7）: x86_64・vf2 では recipe の無い `$(BUILD)/kernel.elf` の target が出来る（害は無い）。rpi4・rpi5 では `dtb.o` が `VMUNIX_OBJS` の最後に付くので、mk の案を当てた後の y とは link の順が違う。overlay の y の vmunix を、案の後の比べの基準にしない。
- p001 の cleared の前に実装した（Q1 の依頼の順、設計の第 3 版の review は p002 の範囲には影響が小さい: M2・M3 は p003 の事）。

## 結果

2026-11-14 P6。commit 741444c99a（実装と設計の途中）。host は Debian、toolchain は `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`、各機種の `sysroot` を先に作った。

### build（`plan/ws302/tests/p002-build.sh OUT REF x86_64 rpi5 vf2`、make -j4）

基点 fd18bd61ee の n の vmunix（変更の前に同じ worktree で作った物）を REF に置いて比べた。741444c99a の tree で 2 回目の実行の要約（1 回目は comment と #define の位置を直す前の tree で同じ結果）:

```
x86_64 n rc=0 warnings=0
x86_64 n-vs-base same-text-but-line-numbers 40
x86_64 y rc=0 warnings=0
x86_64 y-thermal rc=0 warnings=0
rpi5 n rc=0 warnings=0
rpi5 n-vs-base same-text-but-line-numbers 40
rpi5 y rc=0 warnings=0
vf2 n rc=0 warnings=0
vf2 n-vs-base same-text-but-line-numbers 40
vf2 y rc=0 warnings=0
p002-build: PASS
```

- n（既定）は基点と同じ kernel: 逆 assemble の行の数・番地・命令が同じで、違うのは `hal_fatal` に渡す行の番号の即値 40 個だけ（sched.c・thread.c・syscall.c で足した行より後の `HAL_FATAL`）。dis-compare.py が各即値について、基点と今の file の同じ行が fatal の行である事を確かめた。
- y は `-f Makefile -f plan/ws302/tests/dtb-overlay.mk CONFIG_DTB=y`。y-thermal は加えて `CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y`（5330 の評価の構成）。
- rpi4 は build していない（arm64 は rpi5 で見た）。

### QEMU の起動（`plan/ws302/tests/p002-boot.sh OUT y n`、x86_64、boot-test.sh の既定 uefi-nvme・KVM・SMP 4）

```
y image rc=0 warnings=0
y boot rc=0
n image rc=0 warnings=0
n boot rc=0
p002-boot: PASS
```

- image は `plan/tools/boot-images/config-amd64-boot.mk`（text の login）。y・n とも login prompt を画面で読み、emulate した keyboard で root で login して `uname -s` を打ち、画面に `zedBSD` の答えが出た（`login-after.txt` で打った command の次の行が `zedBSD` である事を目で確かめた）。y の kernel では入力の通知・syscall の入口・tick・起床の新しい道が通った。
- 判定は boot-test.sh の画面の読み（console・serial の log は読んでいない）。

### 未実施

- 統計の値そのものの確かめ（`run_ticks`・`user_streak`・`syscalls`・`wakeups`・入力と GPU の時刻）: 読む口（`kern.dtb`）は p004。p002 では値を読まずに道が通る事だけ。
- 統計の負担の計測（A6）: p006。
- arm64・riscv64 の QEMU の起動（範囲の問い、design.md §10.2 の 10）。
- host 試験: 足した関数は static か kernel の struct に依るので host に切り出していない。

### 受け入れの判定（担当の推し）

範囲（config の案・型・統計・`base_priority` の初め）を実装し、`CONFIG_DTB=n` が今と同じ事（逆 assemble）、3 機種の n・y の build の warning 0、x86_64 の QEMU の起動（n・y）を確かめた。mk の案はユーザーの承認待ちで、当たるまで y は overlay でしか build できない。推し: 「実装済み・試験 PASS」（mk の案の承認の後に、案を当てた tree で y の build を 1 回流し直す）。
