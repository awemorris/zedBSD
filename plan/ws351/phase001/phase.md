<!-- awesome-plan project=zedbsd record=ws351-p001 -->

# ws351-p001: 設計（時刻と期限を counter から、idle で tick を 100 Hz に、HAL の実装、HAL_TIMER_FREQUENCY の削除）

Status: cleared（2026-11-15 Q1）
Parent: [WS351](../ws.md)

数と行は 2026-11-13 の worktree `agent/p1-ws351`（main 7d95e0634f）。

## 0. 前提（ユーザーの決定、ws.md）

- tick は CPU ごとに可変。普段 1000 Hz、CPU が idle の時 100 Hz。HAL も kernel も `HAL_TIMER_FREQUENCY` を参照しない。
- HAL の口は `hal_rtc_get_tick_hz()`（呼んだ CPU の今の rate）と `hal_rtc_set_tick_hz(hz)`（呼んだ CPU の rate を近い値に、戻り値は HAL が実際に出す rate）。hal.h は変えない。
- 重要な platform は x86_64・arm64・riscv64。i386・pc9821 は参考（master-design-policy §11.1）。

## 1. 今の形と、tick の数で時間を数えている所（洗い出し）

kernel の時刻は CPU 0 の割り込みの数（`kernel_ticks`、clock.c）と、CPU 0 の tick が写す `scheduler_ticks`（sched.c）。
1 tick の長さは `tick_rate = HAL_TIMER_FREQUENCY`（clock.c:44）。CPU 0 の rate を下げるとこの二つが遅れ、全部の期限がずれる。

| # | 所 | 今の数え方 | 直し |
| --- | --- | --- | --- |
| T1 | `clock_ticks()`（clock.c）・`kernel_ticks` | CPU 0 の割り込みで +1 | counter から作る（§2） |
| T2 | `sched_ticks()`・`scheduler_ticks`（sched.c）、`queued_tick`（enqueue と steal の比べ） | CPU 0 の tick の写し | `clock_ticks()` を返す。`scheduler_ticks` は消す |
| T3 | `kern_clock_gettime()` の monotonic・realtime | `clock_ticks()` を timespec に（tick の刻み） | counter から ns で（§2）。分解能は counter の刻み |
| T4 | `kern_clock_getres()` | 1 tick | counter の 1 刻み（切り上げ）、counter の無い機械は割り込みの周期 |
| T5 | `charge_cpu_time()`（`hw.cputimes`） | 割り込みごとに +1 | 前の tick からの経過（単位の数）を足す（§4） |
| T6 | process の `cpu_ticks`・`user_ticks`・`system_ticks`（times・getrusage）、`resource_limit_cpu_tick()`（RLIMIT_CPU） | 割り込みごとに +1 | 経過を足す |
| T7 | ITIMER_VIRTUAL・ITIMER_PROF（`process_itimer_tick(proc, 1/2)`） | 割り込みごとに残りを -1 | 経過を引く（`process_itimer_tick()` に経過の引数） |
| T8 | ITIMER_REAL（`process_itimer_real_tick_all()`、CPU 0） | CPU 0 の割り込みごとに -1 | 前の pass からの経過（process.c の static の時刻）を引く |
| T9 | quantum（`sched_clock_cpu()` の `quantum--`） | 割り込みごとに -1 | 経過を引く（0 で preempt） |
| T10 | `kern_clock_idle_suspend_begin/end()`（S0ix の追い付き、`tick_rate` を読む） | counter の差を tick に直して `kernel_ticks` に足す | 要らなくなる（時刻は counter から）。関数と sleep.c の呼び出しを消す |
| T11 | `kern_random_tick()` | tick の時刻の揺らぎ | そのまま（遅い tick では標本が減るだけ） |
| T12 | process timer（`process_timer_tick(now)`、CPU 0） | 絶対の期限と now を比べる | そのまま。次の期限を報告する（§3.3） |
| T13 | CPU ごとの sleep の列（`wakeup_tick <= now`） | tick ごとに全部見る | そのまま（now が counter からになる）。次の期限を求める（§3.2） |

tick の数で時間を数えない所（変えない）: WS332 で変換の関数に寄せた期限の計算（`kern_ms_to_ticks()` などの 700 余りの `sched_ticks()`・`clock_ticks()` の利用）、
`kern_usleep_range()`（既に counter）、arm64 の stall watch（counter の時刻で判定、2 s）、x86 の timecounter。

## 2. 時刻の単位と counter

### 2.1 単位

- kernel の時間の単位（`clock_ticks()`・`sched_ticks()`・全部の期限の単位、以下「単位」）を **1 ms の固定**にする（clock.c の中の定数 `CLOCK_UNIT_RATE 1000`）。
  割り込みの rate とは別の物。`kern_clock_tick_rate()` はこの単位の rate（1000、boot の間変わらない）を返す。
  - counter の mode の単位は counter の `p = floor(freq / 1000)` 数え（`clock_ticks() = base_units + (counter - base_counter) / p`）。HAL の tick の格子（§5、`floor(freq/1000)` の倍数）と同じ式にして、
  freq が 1000 の倍数でない時に tick の時の値が格子からずれていかないようにする（review E4）。ずれは 1/p 以下（TSC で 1 ppm 未満、rpi5・VF2・QEMU は 0）。
- 1 ms を選ぶ理由: 期限を調べられるのは割り込みの時だけで、一番速い割り込みが 1000 Hz。単位をそれより細かくしても期限は細かくならず、
    1000 Hz の arch（arm64・riscv64・WS332 までの amd64）の今の挙動（`+ 1U` の補い、quantum、`*_TICKS` の macro の値）がそのまま保たれる。
    ns にすると `+ 1U` が 1 ns になるなど、tick の数の literal を全部見直すことになる（WS332 design §5 の残り）。
- `kern_clock_resolution_ns()` は「busy な CPU が期限に気付くまでの最長」= 単位と呼んだ CPU の割り込みの周期の長い方（100 Hz 固定の機械は 10 ms。review E5）。TCP の時計の粒度・boot の log が使う。
- 変換の関数（`kern_ms_to_ticks()` など）は今のまま（`tick_rate` を単位の rate に読み替えるだけ）。`hw.cputimes` の `hz` は 1000 になる（amd64 は 100 から。userland は値を読んで割る）。

### 2.2 counter の時刻（counter の mode）

- `hal_rtc_read_counter()` が true を返す機械では、CPU 0 の最初の tick（割り込みの中）で基点を決める:
  `base_counter`（その時の counter を `p` の倍数に切り下げた値。HAL の tick の時刻にそろえ、少し遅れた tick も自分の単位を読む。review E4）、`base_units`（その時の `kernel_ticks`）、`base_ns = base_units * 1e6`。基点を publish（release）した後は counter の mode。
- `clock_ticks()` = `base_units + (counter - base_counter) / p`。毎回 counter を読む（新しい値）。読みが失敗した時は CPU 0 の tick が書いた値（`kernel_ticks`）で、
  counter の mode の後に読みが失敗し続けても CPU 0 の割り込みの数えで `kernel_ticks` を進める（時刻が止まらない。review O2）。
- `clock_ticks_ceil()`: 単位の間にいる時は次の単位に切り上げた今（割り込みの mode は数え＋1 周期）。止める・待つ側が早く終わってはいけない期限（nanosleep、相対の timer_settime、setitimer）はこれに期間を足す。
  QEMU で nanosleep(1 ms) が 389 µs で戻る既存の早起き（now の切り捨て）を見つけて直した（§10）。
- monotonic の ns = `base_ns + scale_floor(counter - base_counter, 1e9, freq)`。`clock_ticks()` と同じ counter の差の切り捨てなので、`floor(ns / 1e6) == clock_ticks()`（同じ標本なら）。
- CPU 0 の tick だけが counter の mode の `kernel_ticks` に今の値を書く（共有の line への書きは CPU 0 だけ。review I4）。
- 単調性: counter は HAL の約束で CPU を跨いで linearizable なので、counter から作る値も単調。基点の前（割り込みの数え）から基点の後へは `base_units` から続くので戻らない。
- `freq` は HAL の約束で boot の間変わらない。読みで違う freq が来たら（約束違反）、その読みは失敗として扱う。

### 2.3 counter の無い機械（割り込みの mode）

- `hal_rtc_read_counter()` が一度も true を返さない機械（i386 PC/AT・PC-98 など）と、counter が使えるようになる前の boot の初め（x86_64 の AP の確かめの前）。
- CPU 0 の割り込みごとに `kernel_ticks += 1000 / hz`（余りは繰り越し、hz は `hal_rtc_get_tick_hz()`）。i386（100 Hz）は 10 単位ずつ進み、期限の早さは今と同じ（10 ms の刻み）。
- この mode では rate を下げない（§3.1）。monotonic の ns は `kern_ticks_to_ns(clock_ticks())`。
- x86_64 で timecounter の確かめが失敗した機械はずっとこの mode（1000 Hz のまま。電力は下がらないが時刻は正しい）。

### 2.4 clock_getres

- counter の mode: `ceil(1e9 / freq)`（5330 の TSC で 1 ns、rpi5 の 54 MHz で 19 ns、VF2 の 4 MHz で 250 ns）。
- 割り込みの mode: `ceil(1e9 / hz)`（CPU 0 の rate）。

### 2.5 counter を読む費用

- `clock_ticks()` は 700 余りの所から呼ばれ、enqueue（`queued_tick`）でも呼ぶ。x86_64 の読みは割り込みを止め lfence rdtsc lfence と共有の最大値の CAS（timecounter-policy.c）、
  arm64 は `cntpct_el0`、riscv64 は `rdtime`（U74 では M-mode の emulation の可能性）。それに 64 bit の除算 2 回。1 回 100 ns 前後の見込み。
- 古い値（他の CPU の tick の写し）を使うと、CPU 0 が 100 Hz の時に now が最大 10 ms 古く、期限が最大 10 ms 早く切れる（hardware の reset の待ちなどが足りなくなる）。正しさを取って毎回読む。
- 費用が目に見えたら、乗算と shift の換算（128 bit の積）に直す（今回は作らない）。

## 3. idle で rate を下げる所と条件

### 3.1 誰が呼ぶか

- rate の方針（1000・100、余裕の幅）は kernel の clock.c が持つ（`CLOCK_TICK_BUSY_HZ 1000`、`CLOCK_TICK_IDLE_HZ 100`）。HAL の口を呼ぶのも clock.c だけ。
  - `kern_clock_tick_idle(cpu, next)`: idle の loop が halt の直前に毎回呼ぶ（割り込み off）。`next` はこの CPU の sleep の列の一番早い期限（無ければ UINT64_MAX）。
  - `kern_clock_tick_busy(cpu)`: CPU が idle の thread から他の thread へ移る時（sched.c の `leave_idle()`、cpu の lock の中・割り込み off）。
- CPU ごとの状態（clock.c、`HAL_CPU_MAX` の配列）: 下げているか（lowered）。HAL を呼ぶのは状態が変わる時だけ。
- counter の mode でない時は何もしない（下げない）。

### 3.2 下げる条件（近い timer の予定があれば下げない）

- `now = clock_ticks()`。CPU 0 は `next = min(next, global_next)`（§3.3）。
- `next == UINT64_MAX` か（`next > now` かつ `next - now >= margin`）なら下げる（`hal_rtc_set_tick_hz(100)`）、そうでなければ上げたまま（下げていれば `hal_rtc_set_tick_hz(1000)`）。
  `next <= now`（0 の「すぐ」と、期限を過ぎてまだ拾われていない sleeper）は上げる側（review E1、`tick_far()`、host 試験に 0・now・now-1）。
- `margin = ceil(1000 / 100) + 2 * ceil(1000 / 1000) = 12` 単位（遅い周期 1 つ＋速い周期 2 つ）。
- idle の CPU は遅い tick（10 ms ごと）でも起き、loop がまた `kern_clock_tick_idle()` を呼ぶ。期限の前の最後の評価で残りが margin 未満になると上げる。
  HAL の実装（§5）は上げる時に今の期限を新しい周期で書き直すので、次の tick は 1 ms 以内。期限の遅れは今の 1000 Hz と同じ 1 単位ほど。
  - HAL は下げる時も arm 済みの tick を新しい周期の次の倍数へ書き直す（余る速い tick を消す。review I2）。評価と評価の間は最大 10 ms なので、margin 12 なら次の評価の時に残りは 2 以上ある。
- 費用: 期限の 12 ms 前から速い tick。100 ms 以上の周期で起きる thread だけの idle なら、時間の 9 割近くは 100 Hz。
- sleep の列の一番早い期限は、idle に入る時にこの CPU の sleep の列を lock の中で歩いて求める（列の長さは CPU の寝ている thread の数、tick は毎回同じ列を全部歩いている）。

### 3.3 CPU 0 が見る全体の期限（process timer・ITIMER_REAL）

- process timer（timer.c）と ITIMER_REAL（process.c）は CPU 0 の tick で進む。CPU 0 が 100 Hz だとこれらが最大 10 ms 遅れるので、CPU 0 の idle の判断に入れる。
- clock.c の `global_next`（atomic）:
  - CPU 0 の tick の初めに `exchange(global_next, UINT64_MAX)`。
  - `process_timer_tick(now)` は再 arm の後の一番早い期限を返す（realtime の絶対の timer は `now + (expiry - now_rt)` を ns から単位へ切り上げ、通知の retry が残れば `now + 1`）。
  - `process_itimer_real_tick_all(now)` は残りの一番小さい値から `now + min` を返す。
  - 二つを atomic の min で `global_next` に入れる（`kern_clock_timer_due(deadline)`）。
- 他の CPU で timer を arm した時（`process_timer_settime()`、`process_itimer_set(.., 0, ..)`、`kern_clock_settime()`、通知の retry）は `kern_clock_timer_armed(deadline)`:
  期限（分からない realtime の物は 0 = すぐ）を atomic の min（CAS）で `global_next` に入れ、CPU 0 が下げていて期限が margin の中なら notify（`hal_cpu_notify(0)`）。
  遠い期限は notify しない（CPU 0 は遅い tick ごとに `global_next` を読み直す。review I3）。`SIGEV_NONE` の timer は入れない。
  - 失われる更新の無い理由（pass との間）: arm は状態を書いてから CAS する。pass は exchange（CAS）の後に状態を読む。
  - 失われる notify の無い理由（review E2、store-buffering）: CPU 0 は下げを publish（`tick_lowered[0] = 1`）した後に、読んだ `global_next` の値で CAS（値は変えない）をする。
    arm の CAS が先なら CPU 0 の CAS が失敗して見直し、後なら arm は CAS の acq_rel で CPU 0 の lowered を見て notify する（kernel の atomic には full fence が無いので、同じ場所の RMW で順序を作る）。
- 列の移動: `sched_set_cpu()` が寝ている（期限付きの）thread を他の CPU の sleep の列に移す時はその CPU に、失敗して元の CPU に戻す時は元の CPU に notify して判断し直させる（review O1）。

### 3.4 上げる（起きたら 1000 に戻す）

- `leave_idle()`（idle の thread が他の thread に CPU を渡す、switch の直前）で `kern_clock_tick_busy()`。上げた時は sched.c がその CPU の idle の時間を経過で計上し直す（§4）。
- 割り込み・notify で起きたが走る thread が無い時は上げない（loop が `kern_clock_tick_idle()` で判断し直す）。

## 4. 経過で数える（T5〜T9）

- sched.c の CPU ごとの `tick_last`（単位）。`sched_clock_cpu(id, now)` で `elapsed = now - tick_last`（now が小さければ 0）、`tick_last = now`。
- `charge_cpu_time(cpu, thread, elapsed)`、`cpu_ticks`・`user_ticks`・`system_ticks` に `elapsed`、`process_itimer_tick(proc, which, elapsed)`、quantum から `elapsed`（以下なら 0 で preempt）。
- `kern_clock_tick_busy()` で上げた時: idle の間の `now - tick_last` を IDLE に計上し `tick_last = now`（起きて走る thread に 10 ms の idle を付けない）。
- `process_itimer_tick(proc, which, elapsed)`: 残り > elapsed なら引く。以下なら満了、interval が 0 なら 0、そうでなければ `interval - (elapsed - remaining) % interval`（0 にはしない）。
- ITIMER_REAL の pass の経過は「前の pass（process.c の static の `itimer_real_last`）と、その process の timer を set した時（`itimer_real_armed`、`clock_ticks_ceil()`）の遅い方」から今まで。
  set の前の時間を引いて早く満了しない（review E3。絶対の期限の形にする案より変更が小さい。getitimer の残りは最大で pass の間隔だけ古い、今の 100 Hz と同じ）。
- `tick_last` の初めは CPU が online になる時の `clock_ticks()`。

## 5. 各 arch の HAL の実装

共通: rate は「HAL の最大の rate（1000）の周期の整数倍」だけを出す。`k = clamp(round(1000 / hz), 1, 100)`、出す rate は `1000 / k`（10〜1000 Hz）。
倍数にする理由: 遅い tick の時刻が速い tick の時刻の部分集合になり、x86_64 の位相をそろえる形（WS333 p012）が rate を跨いで保たれる。最初の rate は各 HAL の中の値（1000、i386 は 100 など）で、public の macro にしない。
`hal_rtc_set_tick_hz()` は自分の中で割り込みを止める。kernel は割り込み off で、呼んだ CPU についてだけ呼ぶ。rate が変わる時は arm 済みの tick を新しい周期の次の倍数へすぐ書き直す（下げ・上げとも）。
止まっている timer（AP の開始の前、S0ix の間、park）には書かず、次の開始がその CPU の rate で始める（review E6）。

### 5.1 x86_64（bsp-pcat/lapic.c）

- calibration は 1 秒あたりの LAPIC の count（`timer_count_per_second`）と、TSC の基本の周期 `tick_base_tsc = freq / 1000` を持つ。CPU ごとの `tick_divisor[cpu]`（k、初め 1）。
- TSC-deadline の mode: `arm_next_deadline()` の周期を `tick_base_tsc * k`。期限は TSC の周期の倍数なので、遅い CPU の tick は速い CPU の tick と同じ時刻（1 ms の格子の 10 ms ごと）。
  rate が変わる時は、その CPU の tick が TSC-deadline で動いていれば `arm_next_deadline()` をすぐ呼ぶ（次の新しい周期の倍数）。
- periodic の mode: initial count を `timer_base * k`（`timer_base` は calibration の 1 ms の count）に書く（書くと数え直す）。periodic で動いている時だけ（`tick_periodic[cpu]`）。
- `amd64_lapic_timer_start()`（AP の開始、system sleep・S0ix の後）は CPU の今の k で始める（rate を保つ）。
- `hal_rtc_get_tick_hz()` は `1000 / tick_divisor[cpu]`。
- `config` の 100 Hz の固定（d2c40ed07f、`include/hal/arch/amd64.h`）は消す。

### 5.2 arm64（clock.c、CNTP）

- `timer_interval`（共通）を基本の周期 `timer_base = freq / 1000` に、CPU ごとの `timer_divisor`（percpu、初め 1）。
- 次の期限は `(now / interval + 1) * interval`（counter の周期の倍数）。今の「前の期限＋周期を now を越すまで」と同じ間隔で、全 CPU の位相がそろう（今は CPU の開始の時刻からの倍数で揃っていない）。
- rate が変わる時は `cntp_cval_el0` を書き直す（timer が始まった後だけ）。

### 5.3 riscv64（clock.c、SBI の timer）

- arm64 と同じ形（`timer_base`・percpu の divisor・倍数の期限）。rate が変わる時は `riscv64_sbi_set_timer()` で書き直す。

### 5.4 rate を変えない機械

- i386（pcat・pc98、全体で 1 つの PIT）、m68k（virt・x68k）、mips、ppc32、ppc64、sparcv9、armv7（zybo・virt）、riscv32 は今の rate のまま。
  `HAL_TIMER_FREQUENCY` の利用をその arch の HAL の中の名前（例 `I386_TICK_HZ 100U`、arch の private な header か .c）に置き換え、
  `hal_rtc_get_tick_hz()` はその値、`hal_rtc_set_tick_hz()` は何もせずその値を返す。
  armv7・riscv32・ppc は CPU ごとの timer を持つので可変にできるが、重要な platform でなく試せないので今回はしない。

## 6. HAL_TIMER_FREQUENCY の削除の範囲

- `include/hal/arch/*.h` 13 本と `include/hal/arch.h`（host 試験の 1000）の定義と説明の comment を消す。
- HAL の 17 file の利用を §5 の値に（x86_64・arm64・riscv64 は §5 の実装、他は §5.4）。
- kernel の `src/kern/clock.c:44` を §2.1 の単位の定数に。
- plan/ の記録の言及は履歴なので直さない。`plan/ws333/tests/hw5330/tick-100hz.patch`（使われなくなる試験の patch）は Q1 に消すかを頼む。
- master-design-policy §11.1 の表の「HAL_TIMER_FREQUENCY」の列は Q1 の物（直しは Q1 に頼む）。

## 7. HAL の API の文の補い（案、Q1 へ。hal.h は変えない）

hal.h の 2 つの宣言に、実装で決めた事を書き足す案（review I6 を入れた）:

```
/*
 * Get the local scheduling tick resolution.
+ *
+ * Before the tick is started it reports the rate the tick will start at.
+ * The rate is kept across hal_cpu_idle_suspend() and a system sleep.
 */
...
/*
 * Set the scheduling tick resolution of the local CPU.  ...
+ *
+ * It is called with interrupts disabled, about the calling CPU.  A HAL
+ * may move the tick already armed to the new rate at once.  A machine
+ * whose tick cannot change reports the rate it runs at and changes
+ * nothing.
 */
```

hal.h:587-588 の「The kernel brings its tick count forward from this counter after a system sleep」は、kernel が時刻を counter から直に作るようになり古い（「The kernel's time is read from this counter」の形）。
どれも無くても実装と kernel は動く（kernel は割り込み off で呼ぶだけ）。

## 8. 試験（p002）

- host: clock.c を stub の counter で compile し、単位・ns・getres・基点の前後の単調性・割り込みの mode の繰り越し・idle の判断（margin・CPU 0 の global_next・上げ下げの HAL の呼び出しの回数）、
  `process_itimer_tick()` の経過の算術（同じ式を切り出して）を確かめる（`plan/ws351/tests/clock-host/`）。
- build: x86_64（THERMAL・X86_POWER_CONTROL）・arm64（rpi5）・riscv64（vf2）の vmunix が warning 0。i386・pc98 も試す（崩れは直すか BUG）。他の arch は toolchain があれば。
- QEMU amd64（KVM、`host,+invtsc` で TSC-deadline）: 起動、guest で clock_gettime の差の最小・clock_getres が 1 ms 以下。HMP で per-CPU の rate（`tick_divisor`）を読み、idle で 100・負荷で 1000。
- 5330: idle の package・PC2・GT RC6 を WS333 p029 の数と比べ、busy の時の rate（診断の patch の log、試験だけの build）、clock の分解能。

## 9. 危険

- 期限が 1 ms の単位の固定になるので、i386 など counter の無い機械以外は「1 tick」の待ちが 1 ms（amd64 は 10 ms から 1 ms へ。WS333 p029 の残り (1) の害が無くなる方向）。
- counter を毎回読む費用（§2.5）。x86_64 は全 CPU が 1 つの cache line に CAS する。`queued_tick`（enqueue と steal の比べ）も毎回読む（review I1 の案の「CPU ごとの最後の tick の now」は今回しない。費用が見えたら直す）。
- 電力の後退の経路（review I2）: 12 ms 未満の周期で起きる thread がいる CPU は 1000 Hz のまま（今の amd64 の 100 Hz 固定なら 10 ms に丸めていた）。5330 で測る（p002）。
- 下げた CPU の IDLE の時間は 10 単位の塊で入るので、cpuperf の busy % に量子化が出る（review I5。上げた時に計上し直すので遅れは最大 10 ms）。
- idle の判断が誤ると期限が最大 10 ms 遅れる（下げる条件の誤り、§3.3 の arm の取りこぼし）。
- arm64・riscv64 の期限を倍数にそろえると、全 CPU が同じ時刻に tick を取る（今は CPU の開始の時刻ごと）。lock の取り合いが同じ時刻に集まる。x86_64 は p012 から既にこの形。

## 10. design-reviewer（2026-11-13、1 回）の扱い

| 指摘 | 扱い |
| --- | --- |
| E1 符号なしの引き算 | 直した（`next <= now` は上げる側、host 試験） |
| E2 notify の store-buffering | 下げの publish の後に `global_next` へ値を変えない CAS（§3.3） |
| E3 ITIMER_REAL の早い満了 | process ごとの set の時刻から数える（§4） |
| E4 基点の位相 | 基点を単位の倍数に切り下げ、単位を `floor(freq/1000)` の数えに（§2.1・§2.2） |
| E5 resolution_ns の約束 | 単位と呼んだ CPU の割り込みの周期の長い方（§2.1） |
| E6 止まった timer への書き | 動いている時だけ書く（§5） |
| O1 migrate の失敗の経路 | 元の CPU に notify（§3.3） |
| O2 counter の失敗 | 割り込みの数えで進め続ける（§2.2） |
| O3 HAL の中の tick の数え | arm64 の stall watch の確認の間隔は 100 Hz で 1.28 s（判定は counter の 2 s で変わらない）。tick-rate.sh は WS333 の物で消す候補（Q1 へ） |
| O4 guardrail・master-design-policy | Q1 が guardrail の行を足した（4ed437b2f4）。master-design-policy の HAL_TIMER_FREQUENCY の記述は Q1 へ |
| O5 Phase の分け方・規約の見直しの Phase | Q1 へ（ws.md の表は Q1 の判断） |
| O6 試験の計画 | QEMU amd64 の TSC-deadline と periodic、host 試験に 0・now・now-1、遠近の arm と notify、counter の失敗。arm64・riscv64 は build だけ（QEMU の起動は未実施） |
| I1 counter を読む費用 | 今回はしない（§9） |
| I2 余る速い tick | 下げる時も書き直す（§5） |
| I3 arm のたびの IPI | 期限を渡し、近い時だけ notify。SIGEV_NONE を外す |
| I4 全 CPU の max | CPU 0 だけ |
| I5 cpuperf の量子化 | §9 に記録 |
| I6 HAL の文 | §7 の案を直した |
| I7 32 bit の除算・sparcv9 | 除算は単位で 1 回に減った。sparcv9 は Q1 が了解（main に未 commit の変更は無い） |

## 結果

- 設計は本文のとおり（§1〜§9、review の後の形）。実装と試験は [p002](../phase002/phase.md)。
