<!-- awesome-plan project=zedbsd record=ws331-p009 -->

# ws331-p009: kernel の header から HAL を分ける

Status: cleared（2026-10-30 Q1）
Parent: [WS331](../ws.md)
Queue: q1721（P2、p008 の後）

## 目的と範囲

2026-10-30 ユーザーから次の趣旨の指示を受けた: 整数の型は kernel で定義し直す。`sched.h` は `sched_private.h` と分ける。`sleep.h`・`random.h`・`clock.h` が HAL に依るのはよくない。`atomic.h`・`thread.h`・`vmspace.h` はそれぞれ `_private.h` を足し、HAL は `_private.h` からだけ参照する。構造体の pointer（hal_task_t・hal_space_t）の扱いは検討する。

1. `kern_cpu_id_t` などの kernel の整数の型。`sleep.h`・`random.h` から HAL を外す。
2. `sched.h` と `sched_private.h`、`atomic.h`・`thread.h`・`vmspace.h` の `_private.h`。driver は `_private.h` を include しない。
3. HAL の名前を使わない 6 個（io-scratch・pmem・buf・vm-object・input-device・input-keymap）から HAL の include を外す。
4. `clock.h` の `HAL_TIMER_FREQUENCY` は WS332 で KERN_CLOCK_HZ と一緒に無くす（ここでは扱わない）。
5. 確かめ: 20 port の build、grep で `include/kern/` の公開の header が HAL を include しない事（`_private.h` を除く）。

## 決めた形（P2、q1721）

- kernel の型: 新しい `include/kern/types.h` に `kern_cpu_id_t`（uint32_t、hal_cpu_id_t と同じ表現）、`kern_task_handle_t`・`kern_space_handle_t`（どちらも `void *`、hal_task_t・hal_space_t と同じ）。
- 構造体の pointer（hal_task_t・hal_space_t）の扱い（案と採った形）: どちらも HAL では `void *` の handle なので、公開の header の構造体の field（`struct thread.task`、`struct vmspace.space`）は kernel の opaque な handle の型にし、HAL の型と同じ事は各 `_private.h` の `_Static_assert(__builtin_types_compatible_p(...))` で確かめる。kernel の core は今まで通り HAL の関数に渡せる（型が同じなので cast は要らない）。別の案は 2 つ: (a) `struct thread` の定義ごと `thread_private.h` に移し、driver には不完全型だけを見せる（driver が field を読む所を全部 accessor にする必要があり大きい）、(b) field を HAL の型のまま残し header で HAL を include する（今の形、規則に反する）。(a) は driver の field の利用を数えてから別に決めればよい。
- `sched.h` と `sched_private.h`: kernel の core と HAL の境だけが呼ぶ物（sched_wait_task・sched_exit_current・sched_clock_cpu・sched_accounting_kernel_enter/leave・sched_idle・sched_secondary_init・sched_wait_others_online・sched_cpu_notify・試験の sched_test_cpu_online）を `sched_private.h`（HAL を include、型の assert）へ。driver が使う sched_notify_task は `sched.h` に残し、引数を `kern_task_handle_t` に。他の `hal_cpu_id_t` は `kern_cpu_id_t` に。`sched_private.h` を include するのは src/kern の sched・entry・process・main・clock・user-probe・signal・syscall・thread。
- `thread.h` と `thread_private.h`: `SIGNAL_NEST_MAX`（8）と新しい `THREAD_SYSCALL_ARGS`（6）を kernel の定数にし、HAL の値と同じ事を `thread_private.h` で assert。`thread_fork`（hal_task_t）・`thread_init_secondary`（hal_cpu_id_t）は `thread_private.h` へ（entry.c・process.c・thread.c）。
- `vmspace.h` と `vmspace_private.h`: field を `kern_space_handle_t` に。`vmspace_private.h`（assert）は `src/kern/vm/vmspace-internal.h` から。
- `sleep.h`・`random.h`: HAL の代わりに `kern/types.h`、引数は `kern_cpu_id_t`。
- HAL の名前を使わない 6 個（io-scratch・pmem・buf・vm-object・input-device・input-keymap）から `<hal/hal.h>` を外した（pmem.h・io-scratch.h は `<stddef.h>` を足した）。それで HAL の宣言が見えなくなった kernel の .c（tmpfs-pages・elf・random・tmpfs・sleep・readahead・buf・net/net-device・net/inet-socket・net/route）は `<hal/hal.h>` を直に include（kernel の core は HAL を使ってよい）。
- `clock.h` は WS332（P3）が HAL の include を外した（main の merge で入った）。

### atomic.h（未実施、判断が要る）

`include/kern/atomic.h` は kernel の atomic の inline 関数（refcount_t を含む）を `hal_atomic_*`（`include/hal/atomic.h` の macro。amd64・arm64・riscv64・mips64・armv7・sparcv9 は compiler の `__atomic`、m68k・i386・ppc32・ppc64・riscv32・mips32 は `hal_atomic_enter/leave` で囲む ENTER 型）で書いていて、公開の header が 20 個 include し、driver は 24 file が直に include する。HAL を `_private.h` だけにするには次のどれかが要り、どれも小さくない:

1. **out-of-line**: `atomic.h` は関数の宣言と refcount の inline だけにし、本体は新しい `src/kern/atomic.c`（`atomic_private.h` で HAL を include）。kernel は既定で full LTO なので call は link の時に inline に戻る見込み。欠点: host 試験のうち kernel・driver の source を compile する物（試験の script・mk が src/kern・src/drivers の .c を名指す物が 148 個）の多くが `atomic.c` の link を足す必要がある。
2. **kernel が style を持つ**: `atomic.h` は compiler の `__atomic` で書き、ENTER 型の port だけ build が `-DKERN_ATOMIC_SERIALIZED` を足して kernel の `kern_atomic_enter/leave`（HAL を包む、out-of-line）で囲む。`atomic_private.h` で HAL の `HAL_ATOMIC_STYLE` と一致を assert。host 試験は今のまま。欠点: HAL の atomic の実装（ENTER 型の列）を kernel に写す事になり、arch の事実（style）を build と HAL の 2 か所が持つ。
3. **今のまま**（atomic.h だけ例外）: 規則から外れる。

P2 の勧めは 2（試験の手直しが無く、driver から HAL が消える）。Q1・ユーザーの判断の後に別の commit で行う。

## 結果（2026-10-30）

- commit: 0e4a71447（p008 の追加: watch.c の HVS を Pi 4 の hook へ、下記）、22e324fc9（p009 の 1〜3）、e5e316da5（main c1ced3014 の merge、WS332 を含む。衝突なし）。
- build: merge の後に `plan/ws331/tests/build-vmunix-all.sh build/p009c` で 19 port が rc=0・warning 0、pcat は並列の時に `build/i386/sysroot` の header が見つからず rc=2（p007 と同じ、pc98 と sysroot を共有する並列の競合と見ている）、pcat だけ流し直して rc=0・warning 0（build/p009d）。merge の前（22e324fc9）も 20 port rc=0・warning 0（build/p009b）。
- grep: `grep -n '#include <hal/' include/kern/*.h include/kern/*/*.h` は atomic.h と 3 つの `_private.h` だけ。driver の `_private.h` の include は 0。
- QEMU（画面の PNG を boot-test が読む）:
  - merge の後（e5e316da5）: raspi4b（config-rpi4-boot.mk）login PASS、amd64 uefi-usb（config-amd64-boot.mk）login PASS、m68k-virt・riscv64-virt の vmunix だけの起動で `VFS initialization failed (12)`（root が無いため。画面の文字で `register overlayfs` の失敗は無く、overlayfs の表の kern_calloc はこの 2 port で通った）。
  - merge の前（22e324fc9）: amd64 login PASS、`plan/ws331/tests/end-rpi4-qemu.sh build/p009-end` PASS（owner の画面・login・描き直し・end）。
- host 試験 PASS（22e324fc9）: ws290 の text-end-boards・text-end・ghci、ws215 rpi4-bt、ws206 gpio・sdio。

### p008 の追加（Q1 の指示: arm64 の共通に Pi の物を残さない）

`src/hal/arm64/watch.c` の HVS の display list の読み（planes の列）と一枚の plane の list への差し替えを、Pi 4 の `src/hal/arm64/bsp-rpi4/watch-hvs.c` へ移した。board の hook は bsp.h の `struct arm64_watch_plane`・`arm64_bsp_watch_init()`・`arm64_bsp_watch_planes()`・`arm64_bsp_watch_show_alone()`。Pi 5（HVS6 は形が違う）と S905X5M（`bsp-s905x5m-common/soc.c`）は「無し」。watch.c に残るのは plane の選び方（大きさ・pitch・手放した framebuffer）と描き方と、一番大きい plane を選ぶ事。Q1 の例は bsp-rpi-common だったが、HVS の形を知るのは Pi 4 だけなので bsp-rpi4 に置いた。確かめは build（rpi4・rpi5・odroid-c5、warning 0）と raspi4b の起動（watch の map は boot で通る）。stall は起こしていない（plane を描く道は code の読みだけ）。

## 未実施・残り

- atomic.h（上の案、判断待ち）。
- 20 port のうち QEMU で起動を見たのは amd64・rpi4・m68k-virt・riscv64-virt。他は build だけ。
- 148 個の host 試験の全部は流していない（流したのは上の 6 つ）。kernel の header から HAL の宣言が見えなくなったので、HAL の名前を kernel の header 経由で使っていた host 試験が compile で落ちる可能性がある。
- 判定は Q1。
