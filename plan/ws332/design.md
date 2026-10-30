<!-- awesome-plan project=zedbsd record=ws332-design -->

# WS332 設計: 時間の単位の API と tick の率を取る口

Parent: [WS332](ws.md)。Phase: p001（2026-10-30 P3、q1722、記録は git の履歴）。
数は 2026-10-30 の worktree `agent/p3-ws332`（main fe1d64379 と同じ source）の grep。

**範囲（2026-10-30 Q1 経由のユーザーの指示で絞った）**: (1) `KERN_CLOCK_HZ` の置き換え（時間の単位の API）、(2) 今の tick の率を走っている時に
取る kernel の口（値は今は固定、呼び手は定数を仮定しない）。周期を実際に変える実装・mode・電源管理との関係は今回は作らず、§5 に将来の拡張として短く書く。
HAL の timer は変えない。

## 1. 今の形

- **tick** は CPU ごとの周期の timer の割り込み 1 回。周期は arch の header の `HAL_TIMER_FREQUENCY`（compile の時の定数）で、
  amd64・aarch64・armv7・riscv32/64・mips32/64 が 1000 Hz、i386・m68030/040・ppc32/64・sparcv9 が 100 Hz。
  各 BSP は boot の時にこの値から divisor・interval を一度だけ決め、走りながら変える口は無い（`include/hal/hal.h` にも無い）。
- kernel の時刻は CPU 0 の `kernel_timer_handler()` が割り込みごとに 1 足す `kernel_ticks`（`clock_ticks()`）と、
  CPU 0 の `sched_clock_cpu()` が写す `scheduler_ticks`（`sched_ticks()`）。全ての期限（`sched_sleep()`・`waitq_sleep()` の deadline、
  `thread->sched.wakeup_tick`、process の timer）はこの tick の数の絶対値。
- `KERN_CLOCK_HZ`（`include/kern/clock.h`）は `HAL_TIMER_FREQUENCY` の別名。clock.h はそのために `<hal/arch.h>` を include する。
  kernel と driver が直に掛け算・割り算に使う。変換の関数 `kern_ms_to_ticks()`・`kern_ticks_to_ms()`・`kern_ticks_to_rate()`・
  `kern_duration_to_ticks_ceil()` と、定数の macro `KERN_MS_TO_TICKS()` も clock.h の inline・macro で `KERN_CLOCK_HZ` を使う。
- UAPI は tick を出さない作りが既にある: `times()` の単位は `KERN_PROCESS_TIMES_HZ`（100、libc の `sysconf(_SC_CLK_TCK)`）、
  `CLOCKS_PER_SEC` は 1,000,000。例外は `hw.cputimes` の header の `hz`（今は `KERN_CLOCK_HZ`、userland は値を読んで割る）と、wlan の `deadline_ticks`（§2 の最後、p002 で ns に変えた）。
- 割り込みの数としての tick の利用（`KERN_CLOCK_HZ` は使わない。周期を変える時に直す物、§5）: `sched_clock_cpu()` の quantum の 1 減らし、
  `charge_cpu_time()`（`hw.cputimes`）と process の `cpu_ticks`・`user_ticks`・`system_ticks` の 1 足し（`times()`・`getrusage`・`RLIMIT_CPU`）、
  ITIMER_VIRTUAL・ITIMER_PROF の期限、`kern_random_tick()`、S0ix の後の `kern_clock_idle_suspend_end()` の追い付き（`HAL_TIMER_FREQUENCY` を直に読む）。

## 2. 利用の分類（KERN_CLOCK_HZ 170 行・HAL_TIMER_FREQUENCY の kernel の 2 行・KERN_MS_TO_TICKS 59 行）

`KERN_CLOCK_HZ` の 170 行のうち clock.h の 16 行・clock.c の 9 行（説明の comment を含む）・hal.h の comment 1 行を除く利用は約 145 行・55 file。
行の数は分類ごとに目視で数えた約の数。

| 類 | 何をしているか | 例 | 約の行数 | 置き換え先（p002） |
| --- | --- | --- | --- | --- |
| A 期間の定数 | `#define X_TICKS (5U * KERN_CLOCK_HZ)`、`KERN_CLOCK_HZ / 10U` | buf.c・arp.c・wlan.c・usb-rtl8822bu.c・各 GPU の firmware の root 待ち・i915・venus | 38 | `kern_ms_to_ticks(5000U)` を展開する macro（値の意味の名前は残す）。定数の式の中で使う所は無い（p002 で compile が確かめる） |
| B 期限・待ち時間 | `sched_ticks() + (ms * HZ + 999) / 1000`、`(us * HZ + 999999) / 1000000`、`(ns * HZ + NSEC - 1) / NSEC`、`now + KERN_CLOCK_HZ` | tty.c・pageout.c・net/core.c・pcat.c・tcp.c の RTO・wlan.c・nvme・powervr・venus・i915・bcm2711・gpu.c | 55 | `kern_ms_to_ticks()`・`kern_us_to_ticks()`（新）・`kern_ns_to_ticks()`（新）。どれも切り上げ・飽和 |
| C 経過の計測・log | `ticks * 1000 / HZ`、`(now - began) / HZ` 秒、`ticks * 1000000 / HZ` | usb-rtl8822bu.c・ax211・amdrdna training・powervr firmware-boot・tcp の RTT の標本・disk.c・usb.c の ns | 20 | `kern_ticks_to_ms()`・`kern_ticks_to_us()`（新）・`kern_ticks_to_ns()`（新）。どれも切り捨て・飽和 |
| D UAPI に出す時刻 | `present_time_ns = tick * (NSEC / HZ)` | amdrdna・powervr・ghci・nvrtx・venus・i915・mali・bcm2711 の display | 15 | `kern_ticks_to_ns()` |
| E POSIX の時間の型 | timespec・timeval と tick の往復 | socket.c の SO_RCVTIMEO、syscall.c の nanosleep の残り・setitimer、timer.c の timer_settime | 20 | timespec → tick は既存の `kern_duration_to_ticks_ceil()`、tick → timespec は `kern_ticks_to_timespec()`（新）、timeval は us の変換で |
| F 率そのもの | tick の 1 秒の数・1 tick の長さを値として使う | sysctl.c の `hw.cputimes` の `hz`、clock_getres、tcp.c の時計の粒度、resource.c の RLIMIT_CPU（秒と比べる）、powervr の timing の率、venus の frame の位相（`HZ * 1000` と mHz の refresh） | 11 | 長さは `kern_clock_resolution_ns()`（新）、率は `kern_clock_tick_rate()`（新）。venus の `rate * 1000` は `uint64_t` で計算する（率が大きくなっても溢れない）。RLIMIT_CPU は `kern_ticks_to_ms()` で秒にして比べる |
| G HAL_TIMER_FREQUENCY を kernel が直に | entry.c の boot の log、clock.c の S0ix の追い付き | 2 | log は `kern_clock_resolution_ns()`。clock.c は §3 の 1 か所に寄せる |
| H 定数の macro | `KERN_MS_TO_TICKS(ms)`（59 行・26 file、include/kern/net/wlan.h の 7 個と sched.h の `SCHED_QUANTUM_TICKS` を含む） | tcp.c の RTO の定数・pci-ehci・pci-uhci・ethernet | 59 | `kern_ms_to_ticks()` に置き換えて macro を消す。`SCHED_QUANTUM_TICKS` は `(uint32_t)kern_ms_to_ticks(SCHED_QUANTUM_MS)`（切り上げなので 0 にならず、今の「最低 1」の分岐は要らない） |

host 試験の stub: `tests/cadence-gem-host-test.h` が `KERN_CLOCK_HZ` と `KERN_MS_TO_TICKS` を自前で定義している。p002 で `kern_ms_to_ticks()` の stub に直す。
clock.h の inline を使っていた host 試験（plan/ws220/tests/tcp-model、plan/tools/latency/kd-host-test.sh）は、変換が out-of-line になるので stub の関数を足す。
userland に `KERN_CLOCK_HZ`・`HAL_TIMER_FREQUENCY` の利用は無い。

`KERN_CLOCK_HZ` を使わずに率を仮定している所（design-reviewer の指摘。p002 の範囲の外、§5 の拡張の前に直す物）:

- tick の数の literal: `VM_FREE_WAIT_TICKS 10U`（src/kern/vm/reclaim-internal.h、1000 Hz を前提の「10 秒」が 100 Hz では 100 秒）、
  rpi4-vchiq.c の `sched_ticks() + 2U`、i915 の `I915_WORKER_WAIT_TICKS 1U`・`I915_XD_SCREEN_EVADE_TICKS 2L`。
- 「次の tick まで」の `+ 1`（約 8 か所、exec.c・process.c は回数で待つ loop）。率を変えない今は正しい。
- UAPI の `include/uapi/wlan.h` の `deadline_ticks`（`wlan_scan_status_request`・`wlan_status_request`）は kernel の tick の絶対値を user に
  出していた。userland/base/wifi は `clock_gettime(CLOCK_MONOTONIC)` を `sysconf(_SC_CLK_TCK)`（100）の単位にした自分の時刻と比べており、
  1000 Hz の arch では 10 倍ずれて kernel の期限が効いていなかった（既存の不具合）。Q1 の指示（2026-10-30）で p002 に入れ、
  `deadline_ns`（CLOCK_MONOTONIC の ns、0 は無し。kernel は `kern_ticks_to_ns()` で入れる）に変え、`WLAN_ABI_VERSION` を 2 に上げた。
  field の大きさと位置は同じ。使う所は kernel の wlan.c と userland/base/wifi だけ（networkd・Keiland の zedBSD の backend は version だけを使う、grep で確認）。
  wifi の表示は `deadline=` を `deadline-ns=` に変えた（読む道具は無い、grep で確認）。

### 2.1 置き換えの規則

- 期間は意味の単位（ms・us・ns・秒）で書き、tick への変換は clock.c の関数だけが行う。driver・kernel の他の file は率を掛けない・割らない。
- 待つ側（期限・timeout）は切り上げ（早く起きない）、報告する側（経過・時刻）は切り捨て（過ぎていない時間を言わない）。今の変換と同じ向き。
- 率そのもの（F 類）は、1 tick の長さが要る所は `kern_clock_resolution_ns()`、tick の単位の 1 秒の数が要る所だけ `kern_clock_tick_rate()`。
  後者の利用者は sysctl.c・timer.c（timespec の単位の domain）・venus display.c・powervr render-queue.c に限る（周期を変える時の見直しの対象、§5）。

## 3. p002 で足す時間の API（`include/kern/clock.h`、実装は `src/kern/clock.c`）

率は clock.c の中の 1 か所（`static` の値。p002 では初期値を `HAL_TIMER_FREQUENCY` から取る。kernel は HAL を include してよい）に閉じる。
clock.h は `<hal/arch.h>` を include しなくなる（WS331 の「kernel の header が HAL を include する件」の 1 つが減る）。
inline の変換は out-of-line の関数にする（率を header に出さないため。呼ぶ回数の多い所は context switch の quantum だけで、1 回の 64 bit の乗除）。

```c
uint64_t kern_ms_to_ticks(uint64_t milliseconds);     /* 既存。切り上げ・飽和 */
uint64_t kern_us_to_ticks(uint64_t microseconds);     /* 新。切り上げ・飽和 */
uint64_t kern_ns_to_ticks(uint64_t nanoseconds);      /* 新。切り上げ・飽和 */
uint64_t kern_ticks_to_ms(uint64_t ticks);            /* 既存。切り捨て・飽和 */
uint64_t kern_ticks_to_us(uint64_t ticks);            /* 新。切り捨て・飽和 */
uint64_t kern_ticks_to_ns(uint64_t ticks);            /* 新。切り捨て・飽和 */
uint64_t kern_ticks_to_rate(uint64_t ticks, uint64_t rate);  /* 既存 */
void     kern_ticks_to_timespec(uint64_t ticks, struct timespec *result); /* 新。切り捨て */
int      kern_duration_to_ticks_ceil(const struct timespec *, uint64_t *); /* 既存 */
uint64_t kern_clock_resolution_ns(void);              /* 新。tick と tick の間の長さ */
uint32_t kern_clock_tick_rate(void);                  /* 新。tick の単位の 1 秒の数 */
```

`KERN_CLOCK_HZ`・`KERN_MS_TO_TICKS()` は消す。`KERN_NSEC_PER_SEC` は残す（率でなく単位の定数）。
名前（`*_ticks`）は p002 では変えない。

## 4. 今の tick の率を取る口

- `uint32_t kern_clock_tick_rate(void)`: tick の単位の 1 秒の数（今は boot の時の HAL の率、`HAL_TIMER_FREQUENCY`）。
- `uint64_t kern_clock_resolution_ns(void)`: tick と tick の間の長さ（切り上げ）。clock_getres・TCP の時計の粒度・boot の log が使う。
- 約束: 値は走っている間に変わりうる物として扱う。呼び手は値を定数の式・static の初期値・配列の大きさに使わず、使う時に呼ぶ。
  今の実装は clock.c の `static const` の 1 つの値を返すだけで、HAL の口は増やさない（hal.h の差分は無し）。
- `kern_clock_tick_rate()` を呼ぶのは、ABI が率で単位を言う物（`hw.cputimes` の `hz`）と、tick の整数のまま計算する所（timer.c の
  timespec の単位、venus の frame の位相、powervr の timing）だけ。時間は §3 の変換で扱う。
- `KERN_NSEC_PER_SEC` は単位の定数として残す。`HAL_TIMER_FREQUENCY` は HAL の中（BSP の divisor の計算）と clock.c の 1 か所だけに残る。
- hal.h の RTC の節の comment の「the kernel's KERN_CLOCK_HZ is the same number」は、2026-10-30 ユーザーの承認で語だけを
  `kern_clock_tick_rate()` に直した（宣言・約束は変えない。p002 とは別の commit）。

## 5. 将来の拡張（今回は作らない）

走りながら周期を変える（ゲーム 1000 Hz・デスクトップ 100 Hz・待機 10 Hz）時に要る事。design-reviewer の指摘を含む:

- 期限の単位を割り込みの周期から切り離す（固定の単位の時刻を counter から作る）。単位の選び（1 ns か 1 ms か）、counter の無い機械
  （i386・pc98・x68k、invariant TSC の無い amd64）の扱い、S0ix から出た直後の時刻の追い付き（今の `kern_clock_idle_suspend_end()` の役目）を決める。
- 期限は最後の tick の時刻に足して作るので最大 1 周期早く起きうる（今は tcp.c・i915 が `+ 1` tick で補う）。周期が変わると補いの幅も変わる。
- 周期そのもの（`kern_clock_resolution_ns()` の意味）と単位（`kern_clock_tick_rate()`）が別の値になる。clock_gettime は counter から直に読む。
- quantum・CPU 時間（`hw.cputimes`・times・getrusage・RLIMIT_CPU）・ITIMER・乱数の種の tick の数えを、割り込みの数から経過の時間に直す。
- 周期を変える口は HAL に無い（timer を program するのは各 BSP だけ）。作る時は hal.h の差分を、既存の HAL と kernel で足りない理由とともに承認に出す。
- 誰が変えるか（Keiland のゲームモード、WS306 のゲーム最適化、画面の blank）、`/dev/system` の口と権限、WS052（S0ix）・WS301（DVFS）との関係。
- §2 の最後の、率を仮定する literal・`+ 1`・wlan の UAPI を先に直す。

## 6. design-reviewer（2026-10-30、1 回）の扱い

広い範囲の版（可変の tick の実装まで）を見た。範囲を絞ったので、§5 の実装の指摘（E1・E4〜E9、O2〜O8、I1〜I4）は §5 に要点だけを残した。
p002 に効く指摘は次のとおり扱った:

| 指摘 | 扱い |
| --- | --- |
| E2 wlan の UAPI の `deadline_ticks` | Q1 の指示で p002 に入れ、`deadline_ns`（CLOCK_MONOTONIC の ns）に変えた（§2） |
| E3 tick の literal | §2 に列挙。p002 では変えない（100 Hz の arch の挙動が変わるため） |
| E7 venus の `uint32_t` の溢れ | `(uint64_t)kern_clock_tick_rate() * 1000U` にした |
| O1 host 試験の追従 | cadence-gem・tcp-model・kd-host-test の stub を直して PASS。meson-mmc・dw-mmc は変更なしで PASS |
| O9 変換の同値の試験 | 新しい変換（clock.c の scale_floor・scale_ceil）と古い式を 100 Hz・1000 Hz で 0〜5,000,000 の値と境界で比べた（scratchpad の使い捨て、一致） |
| C4 数の食い違い | HEAD の src・include の grep で 170 行・65 file（ws.md の 175 は概数） |

## 7. Phase の分け方の案

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | KERN_CLOCK_HZ の削除（§2・§3・§4）。挙動は変えない | p001 |
| 最後 | 規約の全文の見直し | p002 |

§5 の拡張は、ユーザーが求めた時に新しい Phase（か WS）として立てる。

## 8. 危険

- p002 は 80 を超える file の機械的な置き換えで、WS331（driver の HAL の include の除去）と同じ file に触る。行の衝突は小さいが、merge の順は Q1 が決める。
- 変換を out-of-line にしたので、context switch ごとに quantum の変換の関数呼び出しが 1 回増える。m68k・i386 の 64 bit の除算は libgcc の関数になる。
- `*_TICKS` の macro の多くは定数でなく関数の呼び出しに展開される（名前は変えていない）。定数の式の中で使う所が無いことは全 port の build で確かめた。
