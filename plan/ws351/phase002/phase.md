<!-- awesome-plan project=zedbsd record=ws351-p002 -->

# ws351-p002: 実装と試験（可変の tick、HAL_TIMER_FREQUENCY の削除）

Status: cleared（2026-11-15 Q1。5330 の電力の受け入れは WS352・WS353 p016 へ）
Parent: [WS351](../ws.md)。設計は [p001](../phase001/phase.md)。

## 変えた所（agent/p1-ws351、基点 main 7d95e0634f）

- kernel:
  - `src/kern/clock.c`・`include/kern/clock.h`: 単位 1 ms の固定（`CLOCK_UNIT_RATE`）、counter の mode（CPU 0 の最初の tick で基点を単位の倍数に）、`clock_ticks()` を毎回 counter から、
    `clock_ticks_ceil()`、clock_gettime を counter の ns から、clock_getres を counter の 1 刻み、`kern_clock_resolution_ns()` を単位と割り込みの周期の長い方、
    idle の rate（`kern_clock_tick_idle()`・`kern_clock_tick_busy()`、1000／100 Hz、margin 12 単位）、CPU 0 の timer の期限（`global_next`、`kern_clock_timer_due()`・`kern_clock_timer_armed()`）、
    `kern_clock_idle_suspend_begin/end()` を消した。
  - `src/kern/sched.c`: `scheduler_ticks` を消し `sched_ticks()`・`queued_tick` は `clock_ticks()`、CPU ごとの `tick_last` から経過で CPU 時間・quantum・ITIMER_VIRTUAL/PROF、
    idle の loop の `next_deadline()` と `kern_clock_tick_idle()`、`leave_idle()` で busy の rate と idle の時間の計上、`sched_set_cpu()` の期限付きの sleeper の移動で notify。
  - `src/kern/process.c`・`include/kern/process.h`: `process_itimer_tick(.., elapsed)`、ITIMER_REAL を前の pass と set の時刻（`itimer_real_armed`）の遅い方から、次の満了を返す。
  - `src/kern/timer.c`・`include/kern/process-timer.h`: `process_timer_tick()` が次の期限を返す（SIGEV_NONE を除く、retry は次の tick）、arm と retry で `kern_clock_timer_armed()`、相対の期限は `clock_ticks_ceil()` から。
  - `src/kern/syscall.c`: nanosleep・system call の待ちの期限（`syscall_restart_deadline_rearm()`、poll・ppoll・socket・tty が使う）・usync の絶対の時刻の期限を `clock_ticks_ceil()` から（早起きを無くす。早起きは userland の poll の loop に 1 ms の poll をもう一度させ、起きる数を増やしていた）。
  - `src/kern/sleep.c`: S0ix の追い付きの呼び出しを消した。
- HAL:
  - x86_64 `bsp-pcat/lapic.c`: `timer_base`（1 ms の LAPIC の count）・`tick_base_tsc`（1 ms の TSC）、CPU ごとの `tick_divisor`、`hal_rtc_get_tick_hz()`・`hal_rtc_set_tick_hz()`（1000〜10 Hz、1 ms の整数倍、TSC-deadline は倍数の期限を書き直し、periodic は initial count）。
  - arm64 `clock.c`・`percpu.[ch]`、riscv64 `clock.c`・`percpu.[ch]`: 同じ形（期限を counter の周期の倍数に、CPU ごとの divisor）。
  - 固定の rate（get はその値、set は何もせずその値）: i386 pcat・pc98（`I386_TICK_HZ 100`）、m68k virt・x68k、mips、ppc32、ppc64、sparcv9（100 か 1000、今までの値）、armv7（`ARMV7_TICK_HZ 1000`）、riscv32。
  - `include/hal/arch.h` と `include/hal/arch/*.h` 13 本の `HAL_TIMER_FREQUENCY` を消した（src・include に参照は 0）。
- 試験: `plan/ws351/tests/`（下）。

## 試験（2026-11-13）

試験の道具（`plan/ws351/tests/`）:

- `clock-host/run.sh`: host 試験。clock.c を freestanding に compile し偽の HAL・scheduler と link（単位・繰り越し・counter の基点の切り下げ・ns・getres・resolution・`clock_ticks_ceil()`・idle の rate の判断（margin、0・now・now-1、CPU 0 の timer、遠近の arm と notify）・counter の失敗）、`process_itimer_tick()` を process.c から切り出して経過の算術。
- `clocktool/main.c`: guest の道具（`res`・`sleep N US`・`spin S`）。static-tool.sh で静的に link。
- `qemu-amd64.sh OUT CPU [PORT]`: QEMU+KVM の amd64（4 CPU、8 GiB、NVMe、USB の CDC-ECM）。build/x86_64 の root の tree の写しに guest の net.conf と clocktool を足した image。SSH で clocktool、HMP で `counter_mode`・`tick_divisor`・`tick_lowered`・`global_next`・LVTT。console は読まない。
- `5330-tick.sh VMUNIX DIAG STATIC NAME`: 5330 の 1 本の script（/tmp/i915-hw.lock、~/boot-zedbsd.sh の一回だけの起動、watchdog=600、最後に p4 を戻して照合）。tree の部分は WS333 p029 と同じ 60 s の窓（held・held2・quiet）、`STOPCOMP=1` で compositor を止めた窓。diag の部分は診断の kernel の割り込みの数。
- 試験だけの patch: `tick-count.patch`（CPU ごとの割り込みの数を 10 s ごとに klog）、`wake-reason.patch`（tick-count に加え、idle の CPU を速いまま保った sleeper を pid・tid・待ちの名前・期間で数える）、`near-thread.patch`（QEMU 用の古い版、wake-reason に置き換わった。消す候補）。

### build（warning 0）

- x86_64（CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y）・rpi5・vf2（`CONFIGMK=config/config.visionfive2.mk PLATFORM=vf2`。config の PLATFORM 名 visionfive2 が mk/platform-vf2 と食い違うので上書き）・i386・pc9821（`PLATFORM=pc9821`）の vmunix: rc 0・warning 0（68910dea12 の tree と、最後の tree（system call の待ちの切り上げの後）で 2 回。x86_64 は各 kernel で毎回 0）。
- mk の無い arch の変えた HAL file（armv7 の clock.c・cmain.c・zybo と virt の timer.c、riscv32、mips の clock.c・cmain.c・rg350、ppc32、ppc64、sparcv9、m68k virt・x68k）: `clang -fsyntax-only -Wall -Wextra` で 0（vmunix の link は未実施）。

### host 試験

- `sh plan/ws351/tests/clock-host/run.sh`: clock-host 42 checks・itimer-host 10 checks、PASS。

### QEMU amd64（KVM、kernel a06353e78dc4a9ee、診断の kernel でも）

| 項目 | TSC-deadline（host,+invtsc） | periodic（host、LVTT 4 periodic） |
| --- | --- | --- |
| 起動・SSH | 8 s | 9 s |
| counter の mode | 1 | 1 |
| clock_getres（monotonic・realtime） | 1 ns | 1 ns |
| clock_gettime の連続の差（最小・中央） | 1041・1629 ns | 931・953 ns |
| nanosleep(1 ms) ×200 idle（平均・最大・最小） | 1995・2022・1752 µs | 1993・2003・1390 µs |
| 同 4 本の spin の中 | 1990・2037・1419 µs | 1993・2166・1927 µs |
| idle の tick_divisor（CPU 0〜3、20 回の読みの最多） | 1 10 10 10 | 1 10 10 10 |
| busy（spin ×4） | 1 1 1 1 | 1 1 1 1 |

- 直す前（clock_ticks_ceil の前）は nanosleep(1 ms) の最小が 389 µs（早起き）。直した後は 1 ms を下回らない（最小 1049〜1927 µs）。
- QEMU の最小の image では CPU 0 か 1 つの CPU が速いまま（init が無い service を再起動し続け、networkd などが短く待つ。image の作りの事で、5330 で別に見た）。

### 5330（10.0.30.3、Debian 6.19.13 から一回だけの起動、watchdog=600、build/hw5330/ws351/）

| run | kernel | 窓 | package | PC2 | GT RC6 |
| --- | --- | --- | --- | --- | --- |
| WS333 p029（100 Hz 固定、比べの基準） | | held・2 回目・quiet | 4.395・4.388・4.339 W | 86.0・87.3・87.9 % | 99.9〜100 % |
| run1 | a06353e78dc4a9ee | held・held2・quiet | 4.733・4.726・4.721 W | 15.7・25.5・11.1 % | 99.9 % |
| run4 | 同 | held・held2・quiet・nocomp（compositor を SIGSTOP） | 4.659・4.670・4.663・4.651 W | 30.6・21.4・6.2・11.4 % | 99.9 % |
| run6 | 8ea9cb2a3963a4ae（system call の待ちの切り上げ） | held・held2・quiet | 4.608・4.607・4.663 W | 46.0・46.4・6.8 % | 99.9 % |

- clock: clock_getres 1 ns、clock_gettime の連続の差 307〜454 ns（最小）、nanosleep(1 ms) idle 平均 1988〜1992 µs・最小 1375〜1846 µs、spin の中 平均 1978〜2018 µs・最大 4073〜5639 µs。
- busy で 1000 Hz に戻る: 診断の kernel（7c952eb2d5fd7d41・b4f0d0c7f7c0336d・a9fc7b18c7a01261）で、全 CPU に spin を置いた 10 s の割り込みの数が CPU ごとに 977〜997/s（run1・run5・run6）。
- idle の割り込み（同じ診断、10 s）: CPU 0 が 618〜999/s、他は 100〜970/s（速い間と遅い間が混ざる）。CPU 0 を速く保ったのは CPU 0 の process timer・ITIMER_REAL ではない（global-fast 0）。
- idle の CPU を速いまま保った sleeper（run5・run6、10 s の idle の評価の数）: compositor（/bin/wayland、pid 7）の 2 つの thread が poll を 1〜11 ms の timeout で繰り返す（tid 71・49、各 8000〜10000 回）。input thread は touch pad がある間 poll の timeout を 10 ms に切る（`userland/desktop/wayland/input-thread.c` の `INPUT_PAD_TICK_MS 10`）。他は kernel の 100 ms の sleeper 2 つ（entry 0xffffffff80436f50）、compositor の usync の 100 ms、client（files・settings・ime）の 2〜22 ms の poll。
- 判定: **idle の電力は WS333 p029 と同等でない（4.61 W・PC2 46 % 対 4.34〜4.40 W・86〜88 %）**。100 Hz 固定では 10 ms 未満の待ちが 10 ms の格子にまとまっていた。可変の tick で待ちが 1 ms の精度になり、12 ms より短い周期で起きる thread（主に compositor の poll）がいる CPU は 1000 Hz のまま。全 CPU の tick は 1 ms の格子にそろうので、1 つの CPU が速いと package は 1 ms ごとに起きる。
  quiet の窓（Wi-Fi を落とし audiod を止めた後）が held より悪い（PC2 6〜11 %）理由は未解析。
- 実機の状態: 各 run の最後に Debian に戻り、p4 は vmunix 4c98d617279bf96b・zedbsd.cfg 1b33a395b0ff4bd7 に戻して sha256 を照合（run1〜run6 の summary の「p4 restored」）。

### 未実施

- arm64（rpi5）・riscv64（vf2）の起動と rate の確認（QEMU・実機とも）。build だけ。
- i386・pc9821 の起動（build だけ）。mk の無い arch の link。
- system sleep・S0ix の後の rate と時刻（指示どおり使っていない）。
- counter を読む費用の計測。

## 残り（Q1 へ）

- 受け入れ条件の「idle の package・PC2 が WS333 p029 と同等」は未達。選択肢: (1) Keiland の compositor の idle の poll（1〜11 ms の timeout、touch pad の 10 ms）を直す（別の WS、userland）。
  (2) idle の CPU で、近い期限を遅い格子（10 ms）にまとめる timer slack（期限が最大 10 ms 遅れる。「近い timer の予定があれば下げない」と食い違うので、ユーザーの判断）。(3) 今のまま。
- quiet の窓の悪化の解析（Wi-Fi の down の後に短く待つ物がある見込み）。
