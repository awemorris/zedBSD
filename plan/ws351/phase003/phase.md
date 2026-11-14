<!-- awesome-plan project=zedbsd record=ws351-p003 -->

# ws351-p003: 規約の全文の見直し

Status: 実装済み・試験 PASS（arm64・riscv64 の QEMU は未実施、下）。cleared は Q1。
Parent: [WS351](../ws.md)

## 対象

WS351 が変えた C: 68910dea12（p002 の実装）と 62a11167d3（system call の待ちの切り上げ）の行。
`include/kern/{clock,process,process-timer}.h`、`src/kern/{clock,process,sched,sleep,syscall,timer}.c`、
HAL: x86_64 `bsp-pcat/lapic.c`、arm64 `clock.c`・`percpu.[ch]`、riscv64 `clock.c`・`percpu.[ch]`、i386 pcat・pc98 `pit.c`・`clock.h`、armv7、m68k virt・x68k、mips、ppc32、ppc64、sparcv9、riscv32。
`plan/tools/style-check.py` は file 全体で数えるので、`git blame` で上の 2 commit の行だけに絞って数えた（絞った件数: 前 20、後 0）。他の WS の既存の違反（process.c・sched.c・syscall.c・clock.c の他の行）は直していない。

## 直した事（動作・API・UAPI・error の値は変えていない。commit 9b46e2ab8a）

| 規則 | 件数 | 例 |
| --- | --- | --- |
| 条件の中で関数を呼ばない | 13 | `counter_mode` の読みを `mode` に、`atomic_u64_compare_exchange`・`tick_far`・`hal_cpu_current`・`process_itimer_tick` の結果を変数に（clock.c・sched.c・process.c） |
| 閉じ括弧の後の空行と comment | 5 | lapic.c の `hal_printf`、clock.c の slowdown、process.c の `left != remaining`、sched.c の `timed` の 2 か所 |
| 本体が複数行の if の brace | 1 | process.c |
| paragraph の comment | 1 | clock.c の `*ticks = ...` |
| 引数の中の入れ子の呼び出し（task の指示の「名前の付いた変数に」） | 14 | `arm_next(cpu, counter())`、`write_reg(.., periodic_count(cpu))`、`cpu_divisor(hal_cpu_current())`、`kern_deadline_after(clock_ticks_ceil(), ..)`（syscall.c 3・timer.c 1）、`kern_clock_timer_due(process_itimer_real_tick_all(now))`、`kern_clock_tick_idle(cpu, next_deadline(cpu))`、`x68k_timer_init(hal_rtc_get_tick_hz())` ほか |

`while (!compare_exchange)` を `exchanged` に取って同じ回数・同じ順序で回す形にした（clock.c `global_next_swap`）。短絡の順序は kern_clock_tick_idle の `cpu != 0 ||` を分けても保った（CPU 0 以外は exchange を呼ばない）。
`return f(...)` の形・comment の中の開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照）は、2 commit の追加行に 0（grep で確認）。

## 結果

- build（`make -j4 CONFIGMK=config/config.<機種>.mk vmunix`）: x86_64・rpi5（arm64）・vf2（riscv64）・i386・pc9821 全て rc 0、compiler の warning 0。
- m68k x68k の cmain.c: `clang --target=m68k-unknown-none -fsyntax-only -Wall -Wextra` で 0（link は未実施）。
- host 試験 `sh plan/ws351/tests/clock-host/run.sh`: clock-host 42 checks・itimer-host 10 checks、PASS。
- QEMU amd64（KVM、4 CPU、`qemu-amd64.sh`、port 2358、image 91a4bc69b19bceea。rootfs などは p1-ws353m の build/x86_64 の写し）:
  - TSC-deadline（host,+invtsc）: ssh 9 s、counter_mode=1、LVTT は 4 CPU とも tsc-deadline、getres 1 ns、nanosleep(1 ms)×200 平均 1994 µs・最小 1378 µs、
    idle の divisor は CPU 0 が 1、他が 10（最多 19/20）、spin 4 本の間は 1 1 1 1、idle-again は 1 10 10 10。PASS（p002 の表と同じ）。
  - periodic（host）: idle 1 10 10 10（17/20）、busy 1 1 1 1（13/20）、nanosleep(1 ms) 平均 1995〜2041 µs・最小 1829 µs。PASS。
  - 起動の確認は SSH の login（`boot-test.sh` の画面の login prompt の確認ではない）。

## 未実施

- arm64・riscv64 の QEMU の起動と tick の divisor: 今の tree に QEMU の virt の platform が無い（arm64 は rpi4/rpi5、riscv64 は vf2 のみ）。
  rpi4 の QEMU raspi4b は image の build（`disk`）が `rpi4-boot-firmware` の firmware の license の同意を要求し（この worktree に同意済みの cache が無い）、
  `plan/tools/boot-images/config-rpi4-boot.mk` と `plan/tools/rpi4/config-rpi4-sd.mk` は今の build system の変数（`CONFIG_USERLAND_PROGRAMS`）では firmware を外せず同じ所で止まった。license は担当が代わりに同意しない。
  vf2 は vf2 の image が QEMU の virt で起動する根拠が無く、main の vf2 image の kernel は WS351 より前。
  Q1 の選択肢: (1) 同意済みの main の firmware cache を使う build を Q1 が頼む、(2) 5330 以外の実機（rpi5・vf2）の試験を p003 とは別に割り当てる、(3) 上の config を今の変数に直す（Makefile・mk は変えない）。
- i386・pc9821 の起動（build のみ）、mk の無い arch の link。
- `git diff --check` は commit 前に流していない（commit hook は通った）。
