<!-- awesome-plan project=zedbsd record=ws333-design -->

# WS333 設計: /dev/thermal で統一する電力・熱の枠組み（CPU の idle・P-state・P/E core、熱・fan・strategy・非常の停止）

- 版: **第 1 版**（2026-10-31 P5、q1734-i01）。WS301 の design.md 第 3 版の改訂 3（CPU の DVFS と P/E core）と WS303 の design.md 第 5 版（熱と fan）を入力にし、1 つの枠組みにまとめた。WS303 第 5 版の review（[../ws303/design-v5-review.md](../ws303/design-v5-review.md)、blocker 0・major 9・minor 25、WS303 では未反映）はこの版に反映した（行き先は §8.3）。
- review: [design-review.md](design-review.md)（この版の design-reviewer、§14 に反映の表）。
- 基点: worktree `p5-ws333`（branch `agent/p5-ws333`、main aa112273a）。引用の file:行 はこの基点の物（WS301・WS303 の引用は main 68ee65381 の物で、その後に変わった file は §2 で読み直した）。code は書いていない。
- 入力（読んだ物）: WS301 の ws.md・design.md・user-questions.md・hal-review.md・design-v3-review.md、WS303 の ws.md・design.md・user-questions.md・design-v5-review.md、WS302 の ws.md、WS299 の ws.md の「結果（2026-10-28 21:50）」、WS291 p022 の phase.md、WS332 の ws.md・design.md §5、WS331 の ws.md、`plan/guardrail.md`（HAL、driver は HAL を知らない、出典）、`plan/master-design-policy.md`、`docs/agent/spec-module-delegation.md`、`include/hal/hal.h`、source（§2）。

## 0. ユーザーの決定（要点）

原文は WS301・WS303 の ws.md と design.md §0 に記録済み（この文書では要点だけ）。

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-31 | WS301 と WS303 を廃止して統合し、新しい WS にする。検討を始めた時は /dev/thermal という統一の device を想定しておらず、検討が進んでこの形になったので、統一して設計しないといけない（Q1 の中継、ws.md） | この文書。2 つの WS の口（受け手・上限・ioctl の範囲）を 1 つの kernel の口にまとめる（§4.2） |
| 2026-10-31 | CPU の省電力（idle の C-state・P-state）を先に直す。amd64 の idle（MWAIT）は p002 で先に実装する（ws.md） | Phase の順を CPU の道が先になるように組む（§9）。idle の状態の選びは HAL の実装に置き、p002 と食い違わない（§4.3） |
| 2026-10-29（WS301 Q301-1・Q301-8） | HAL は変えない。HWP の無い CPU も P-state を driver で操作する | §4.4・§6 |
| 2026-10-29（WS301 Q301-2） | 相対の性能の既定は最大の周波数の比、後で写し方を変えられる口にする | §4.5（WS301 §4.7 のまま） |
| 2026-10-29（WS301 Q301-3・4・5・7） | `_OSC` は呼ばない（WS310）、`CONFIG_CPU_PERF` は試験が通るまで既定 n、arm64・riscv64 は枠だけ、WS301 は自分の policy を持たない（直の値は root の診断だけ） | §4.4 |
| 2026-10-29（WS301 の Q1 の中継） | CPU の速さの UAPI は /dev/system でなく /dev/thermal | §4.10 |
| 2026-10-29（WS303 Q303-1〜14） | `/dev/thermal`、変更は root と sessiond 経由（root か wheel）、`/sbin/thermal` が strategy を選ぶ、fan は上げる段だけ（必要な回転数を下回らせない）、非常の水準で電源を落とす、制御できない機械は firmware に任せ strategy だけを伝える、passive を入れる、strategy は performance・balanced・powersave・manual（manual は speed・noise・gating の 3 軸 0〜100、写し方は driver が決める）、`hw.thermal` は後で外す、SMM の値は GPL の file から出典つきで、非常の水準は zone の critical だけ、passive は仕組みと host 試験まで | §4.6〜§4.11 |

未回答の問い（WS303 の Q303-15〜19）は、統合で前提が変わった物を含めて Q333-1〜5 として聞き直す（§11、[user-questions.md](user-questions.md)）。

## 1. 目標と範囲

- 目標（ws.md）: CPU の idle（C-state）・P-state（HWP と、HWP の無い CPU の driver の P-state）・P/E core の相対の性能・熱の監視・fan・非常の停止を `/dev/thermal` と `/sbin/thermal` の下で統一して設計し、実装する。
- 最初の対象: 5330（Latitude 5330、i5-1245U、P-core 2 と E-core 8）。熱と fan は Pi 5 も（WS303 のまま）。
- 範囲の外: `_OSC`（WS310）、ASPM・APST・GPU の power gating（WS300・各 GPU の WS）、tick の周期を変える仕組み（WS332 の将来の拡張）、DTB の本体（WS302）、Keiland の UI。
- 受け入れの柱（§8）: (1) 5330 の idle の package の電力が p002 の前より下がり、i915 の GT が PL1 で 800 MHz に抑えられる件（WS291 p022）が和らぐかを測る、(2) strategy の 4 つで CPU の要求が変わる、(3) 熱・fan・strategy を `/sbin/thermal` で読める・選べる、(4) 非常の停止が QEMU の注入で 2 つの道とも動く。

## 2. 今の振る舞い（code の事実、基点 aa112273a）

### 2.1 CPU の idle と tick

- amd64 の `hal_cpu_idle()` は `sti; hlt; cli` だけ（`src/hal/amd64/task.c:707〜712`）。HAL の契約は「Atomically enable interrupts and halt, then return with IRQs disabled」（`include/hal/hal.h:194〜197`）で、どの状態で待つかは HAL の実装が決める（kernel は状態を名指さない）。p002 はここを MWAIT にする（hal.h は変えない）。
- 系の S0 idle（suspend-to-idle）は別の口 `hal_cpu_idle_suspend()`（`hal.h:213〜232`、amd64 は `src/hal/amd64/bsp-pcat/idle-suspend.c`）。LPIT の MWAIT の hint（5330 は C10）で MWAIT、ECX bit 0（割り込みで抜ける）、待つ間は LAPIC の tick を止め、kernel は `hal_rtc_read_counter()` から tick を進める（`src/kern/clock.c` の `kern_clock_idle_suspend_begin/end`）。
- idle の loop: `sched_idle()`（`src/kern/sched.c:1562〜1599`）が `kern_sleep_idle(cpu)`（`src/kern/sleep.c:211〜`）を呼び、系の sleep でない時は `hal_cpu_idle()`。
- tick: amd64 は `HAL_TIMER_FREQUENCY` 1000 Hz（`include/hal/arch/amd64.h:16`）、各 CPU の LAPIC の周期の timer（`src/hal/amd64/bsp-pcat/lapic.c:252〜`）。`kernel_timer_handler()`（`clock.c:96〜`）は CPU 0 だけが `kernel_ticks` と process の timer を進め、全 CPU が `sched_clock_cpu()` を呼ぶ。**CPU ごとの時間つきの sleep の起床はその CPU の tick で行う**（`sched.c:1025〜1039`、thread は CPU に固定）。tick を止める口・周期を変える口は HAL に無い（`hal.h:546〜556` は周期の tick だけ、WS332 design §5）。
- → 5330（論理 CPU 12）は idle でも 1 秒に約 12000 回の tick の割り込みを受ける。

### 2.2 CPU の性能

- P-state・HWP・CPPC・RAPL・C-state の滞在の計数の code は無い（`grep -rni 'rapl\|0x611\b\|0x770\|HWP' src include` で無関係の物だけ）。
- CPU の口: `hal_cpu_notify()`（`hal.h:154〜171`、受け手 `kernel_cpu_notify_handler()` は `clock.c:193〜`）、`hal_cpu_count()`、`hal_cpu_current()`。kernel の側の型 `kern_cpu_id_t`（`include/kern/sched.h`、`hal_cpu_id_t` と同じ型を `sched_private.h:23` が確かめる）と `kern_cpu_current()`（`sched.h:202`）。
- 新しい規則（guardrail「driver は HAL を知らない」、2026-10-30）: driver は `hal/hal.h` を include せず `hal_*` を呼ばない。kernel の口だけを使う。WS301 の provider の口（`hal_cpu_id_t` を使う）は、この規則に合わせて直す（§4.4）。driver の中の CPU の命令の inline asm は HAL ではない（前例: `src/drivers/gpu/i915/device.c:2265` の CPUID、`src/drivers/platform/pcat/pcat-ide.c:218` の port の命令）。
- amd64（Intel Mac の config も含む、`config/ci/config-intelmac.mk:3`）は `src/drivers/platform/pcat/` の driver を build する（`platform/amd64/vmunix.mk:483` ほか）。

### 2.3 熱

- `hw.thermal`（`include/uapi/sysctl.h:243〜313`、`HW_THERMAL 9`、`thermal_header` 24 byte・`thermal_entry` 64 byte）、registry `src/kern/sysctl.c:152〜245`（source 4、1 source 16 sensor）。源は `acpi-thermal.c`（5 秒の周期 38・252 行、`kern_thermal_register` 112 行）と `rpi5-fan.c`。読み手は `userland/base/sysctl/main.c`（99・226・749〜803）と `userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c`（643〜681）の 2 つ。
- `/dev/thermal`・`include/uapi/thermal.h`・`include/kern/thermal.h`・`src/kern/thermal.c` は無い。cdev の番号 0x00010008 は空き（使用中は 0x00010000〜0x00010007・0x00010010〜・0x00010020、`grep` で確かめた）。ioctl の group 'T' は空き。
- init の signal: SIGHUP・SIGINT・SIGTERM（`userland/base/init/main.c:293〜295`）。sync の口 `mount_sync_all()`（`src/kern/mount.c:946`）。
- WS303 第 5 版の §1 の他の事実（ACPI WMI の無い事、AML の interpreter の lock、`drv_acpi_poweroff` が `_PTS` を評価する事、devfs の mode、sessiond の POWER の判定）は、その後に変わった file（`acpi-kern.c`・`system-device.c`・`devfs.c`・`vfs.c`・`sysctl.c`）の行番号がずれている。意味は p004・p010 の着手の時に読み直す（行番号を固定の事実として使わない）。

## 3. 調べた事実（引き継ぎ）

- 5330 の CPU（WS301 design §2.3、Debian で読みだけ、2026-10-29）: hypervisor 無し、CPUID.06H:EAX 0x00DF8FF7（HWP・EPP・HFI・HWP_CTL・Thread Director 有り）、P-core 2（SMT 4 thread）・E-core 8（4 つ組 2 つ）、HWP の能力 P（56・21・20・1）・E（33・12・15・1）、測った Highest の MHz は P 4388・E 3292（P 78.6・E 100 MHz/単位）、TjMax 100 °C、PL1 25 W・PL2 55 W（Dell の performance の時、未確認）。
- 5330 の熱（WS303 design §2.1〜2.5）: ACPI の thermal zone は無い（DPTF の participant だけ）、fan は EC が回し、OS の口は Dell の WMI（DDV の読み、SMBIOS の thermal mode、SMM）だけ。
- 5330 の電力（WS291 p022、2026-10-30〜31）: zedBSD の idle（desktop の session、GPU は止まっている）で package 12.99 W、Linux は 3.2〜3.5 W。mview c600 で zedBSD 13.0 W（PL1 13.0 W に張り付き、GT 800 MHz、GT0_PERF_LIMIT_REASONS は PL1・PL2）、Linux 14.6 W（GT 1200 MHz）。Linux の cpuidle を C1E だけにしても Linux は 1200 MHz。読みは GT の MMIO の MCHBAR の写し（診断の patch、source に入れていない）。
- WS299 の結果（2026-10-28）: zedBSD の 1 thread の速さは Debian の 66〜90%、idle は `sti; hlt` だけ、HWP 無し、hybrid を知らない置き方。
- 見立て（推定、§12 で確かめる）: idle の電力の差の大きな部分は (a) HLT の C1 では core の電圧が下がらない事、(b) 1000 Hz の tick が全 CPU で揃わずに来るため package の深い C-state（PC6〜PC10）に入る間が短い事、(c) P-state の要求が firmware のまま（HWP 無効）の事。(a) は p002、(c) は p008、(b) は §4.3.3 の条件つきの項目。

## 4. 設計

### 4.1 全体の形

```
 user:   /sbin/thermal（表示・root の設定）   sessiond（root か wheel の利用者の strategy・boost）   Keiland の monitor（p016 から）
                 \                                  |                                             /
 UAPI:                      /dev/thermal（0666、SET は ioctl の中で root）   /dev/thermal-inject（0600、試験の build だけ）
                                                    |
 kernel: src/kern/thermal.c（core: 写し・strategy・受け手・meter・事象・ioctl の振り分け・worker の thread）
         src/kern/thermal-guard.c（見張りの thread・非常の停止）
         src/kern/cpuperf.c（CPU: topology の表・domain・要求の箱と notify の道・相対の性能・governor・受け手の 1 つ・CPU の ioctl）
                 ^ 登録（kern_* の口だけ）
 driver: src/drivers/platform/pcat/cpu-x86/（CPUID・MSR、HWP・legacy・HFI・RAPL・C-state の滞在、amd64）
         src/drivers/acpi/acpi-thermal.c（sensor の push）  src/drivers/acpi/acpi-wmi.c ＋ src/drivers/platform/pcat/dell-wmi.c（fan・Dell の mode・boost）
         src/drivers/platform/rpi5/rpi5-fan.c（sensor・fan・受け手・boost）
 HAL:    変えない。hal_cpu_idle() の状態の選びは HAL の実装（p002）。kernel の cpuperf.c が hal_cpu_notify() を使う（driver は使わない）
```

- 層の責務: kernel の core は「写しと方針の配り」、driver は「hardware の読み書き」。strategy の意味（4 つと 3 軸）は core が定め、各 driver が自分の hardware への写し方を決める（Q303-11）。
- `CONFIG_CPU_PERF`（既定 n、Q301-4）は **MSR を書く・読む道だけ**を入れる。CPUID だけで分かる topology と core の種類は常に入れる（§4.5、§13 の判断）。熱の core（`thermal.c`・`thermal-guard.c`）は全 platform で常に入る（`hw.thermal` を置き換えるため）。

### 4.2 kernel の口の統一（2 つの WS の口を 1 つに）

WS301 と WS303 は別の WS だったため、`cpuperf.c` から `thermal.c` へ 3 つの別の登録（strategy の受け手、熱の上限の受け手、ioctl の範囲）と `is_custom` の callback を作り、引数が食い違っていた（WS303 v5 review の M2）。統合で次の 2 つにする。

1. **受け手の口**（strategy・上限・CUSTOM・firmware の名前を 1 つの ops に）。CPU（`cpuperf.c`）・Dell・Pi 5 が同じ口で登録する。
2. **ioctl の範囲の口**（CPU の ioctl 32〜63 を `cpuperf.c` が持つ）。

```c
/* include/kern/thermal.h（案。名前は p003 で coding-style に合わせる） */
struct kern_thermal_strategy {
	uint32_t value;			/* KERN_THERMAL_STRATEGY_* */
	uint8_t speed, noise, gating;	/* 0..100, 100 = most of it; 0 unless MANUAL */
	uint8_t reserved;
};

#define KERN_THERMAL_APPLY_UNCONFIRMED	0x1U	/* accepted, the hardware has not confirmed yet */

struct kern_thermal_receiver_ops {
	/* worker thread, may sleep, no thermal lock held; 0, EOPNOTSUPP (does not apply here) or an errno */
	int (*apply)(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);
	/* worker thread; domain KERN_THERMAL_DOMAIN_ALL or the receiver's own; permille of its range; NULL: no passive */
	int (*limit)(void *context, uint32_t domain, uint32_t permille);
	/* lock-free read, may be NULL */
	bool (*is_custom)(void *context);
	/* may be NULL; the firmware's current mode as text */
	int (*firmware_name)(void *context, char *name, size_t size);
};
int kern_thermal_receiver_register(const char *name, const struct kern_thermal_receiver_ops *ops, void *context);

typedef int (*kern_thermal_ioctl_t)(struct file *file, unsigned long request, uintptr_t argument);
int kern_thermal_ioctl_range_register(unsigned first, unsigned last, kern_thermal_ioctl_t handler);

/* sensors, fans, meters (energy counters) and events */
int kern_thermal_sensor_register(const char *name, uint32_t kind, uint32_t flags, struct kern_thermal_sensor **sensor);
void kern_thermal_sensor_update(struct kern_thermal_sensor *sensor, int32_t millicelsius, bool valid);
void kern_thermal_sensor_levels(struct kern_thermal_sensor *sensor, int32_t passive, int32_t critical);
int kern_fan_register(const char *name, uint32_t rpm_max, const struct kern_fan_ops *ops, void *context);
int kern_thermal_meter_register(const char *name, uint32_t kind, const struct kern_thermal_meter_ops *ops, void *context);
void kern_thermal_event(uint32_t type, uint32_t index, int64_t value);	/* thread context only */
```

- `kern_fan_ops`・sensor の値の規則・fan の写し・STALLED は WS303 design §4.3・§5.1 のまま（STALLED は「実際の段 = max(自動, boost) > 0 で rpm 0 が 3 周期」に直す、v5 m9）。
- 登録は全部 static の表（受け手 16、ioctl の範囲 4、sensor 32、fan 8、meter 8）で外さない。登録と振り分けの公開は release・acquire の順（v5 m20）。
- 受け手は apply の中で thermal の関数を呼ばない（事象の `kern_thermal_event` を除く）。thermal は受け手を lock を持たずに呼ぶ（§4.12）。
- ioctl の範囲: core が 1〜31、CPU（`cpuperf.c`）が 32〜63、64〜 は予備。登録の無い番号は ENOTTY。SET の root の判定は各 handler の中（CPU の SET は `cpuperf.c` が `cred_is_superuser` で）。

### 4.3 CPU の idle（C-state）

#### 4.3.1 誰が状態を選ぶか（p002 との関係）

- **状態の選びは HAL の実装**（`hal_cpu_idle()`）。hal.h の契約は待ち方を名指さないので、HLT から MWAIT の C-state に替えても契約の中（p002、hal.h は変えない）。kernel はどの状態に入ったかを知らず、名指しもしない（`hal_cpu_idle_suspend()` と同じ考え方、`hal.h:220〜221`）。
- p002 の範囲（phase.md）: CPUID で MONITOR/MWAIT と ARAT を確かめ、最深の C-state の hint、ECX bit 0（割り込みで抜ける）。MWAIT の無い CPU は HLT のまま。
- この設計が p002 に求める条件（矛盾しないための約束。p002 の phase.md の範囲の中で P1 が決める物を、統一の設計の側から書き出した）:
  1. HAL の counter の契約（`hal.h:570〜582`: `hal_rtc_read_counter()` の counter は HAL が入る全ての idle の状態で進む。進まない状態には入らない）を守る: C1 より深い状態は invariant TSC の CPU だけ。
  2. 周期の tick の契約: LAPIC の timer が止まる状態（ARAT の無い CPU の C3 以上）に入ると tick が止まり、CPU ごとの時間つきの sleep（§2.1）が遅れる。ARAT の無い CPU は C1（か C1E）まで。
  3. `hal_cpu_notify()` は待ちを必ず終わらせる（`hal.h:158〜163`）。MWAIT は割り込みで抜けるので満たす。
  4. hypervisor の下（QEMU・KVM）は今の HLT のまま（MWAIT を出さない・exit を起こす）。
- WS333 の kernel と driver は idle の状態を**書かない**。読む物は §4.3.2 の統計だけ。よって p002 の実装は WS333 の後の Phase と独立に merge できる。

#### 4.3.2 kernel と UAPI が持つ物（読みだけ）

- x86 の driver（`CONFIG_CPU_PERF=y` の時）が C-state の滞在の計数を読み、UAPI に出す:
  - package の滞在（PC2・PC3・PC6・PC7・PC8・PC9・PC10）: package の MSR（SDM Vol 4 の model ごとの MSR。番号の候補 0x60D・0x3F8〜0x3FA・0x630〜0x632、ADL の表にあるかは p006 で確かめる、未確認）を CPU 0 で読む。
  - core の滞在（CC6・CC7）: core の MSR（候補 0x3FD・0x3FE、未確認）。その CPU の上でしか読めないので、GET の ioctl の時だけ notify の道で読む（周期の読みで CPU を起こさない、WS301 §4.2 の M5 の規則）。
  - 使える状態の一覧（CPUID.05H:EDX の C-state ごとの sub-state の数、architectural）。HAL が実際に使う状態は分からないので「CPU が持つ状態」として出す。
- 計数の単位と model の allow list は p006 の module の設計で SDM から決める（計数が TSC の rate で数えるかは未確認）。
- これは idle の電力の受け入れ（§8.3）と、§4.3.3 の判断の数（package の深い状態の滞在の率）の源になる。今は診断の patch（WS291 p022 の `i915-stats-diag.patch`）でしか電力を読めない。

#### 4.3.3 OS が idle を制御する口（条件つき、今は作らない）

二つの事は、既存の HAL の口では足りない。ただし要るかは測りで決まるので、**今は HAL の差分の案を作らない**（AGENTS.md §7: まず HAL を変えない形を探し、足りない理由を具体的に示した時だけ案にする）。p009 の 5330 の測りの後に、数とともに判断を求める（Q333-7）。

| 事 | 何のためか | なぜ既存の HAL・kernel・driver では足りないか |
| --- | --- | --- |
| A. idle の CPU の tick を止める（tickless idle） | 12 CPU × 1000 Hz の割り込みが core を起こし続け、package が深い C-state に入れない（§3 の見立て (b)、推定） | tick は各 BSP が周期で program し（`hal.h:546〜556`）、kernel から止める・遅らせる口が無い。`hal_cpu_idle_suspend()` は tick を止めるが、契約は系の S0 idle 用（`hal.h:213〜215`、LPIT の状態、局所の割り込み源も静める）で、普段の idle に使うのは契約の読み替えになる。kernel の側だけでは、CPU ごとの時間つきの sleep をその CPU の tick で起こす作り（`sched.c:1025〜1039`）のため、tick が要らない CPU を kernel が知っても止められない。driver が LAPIC を直に触るのは HAL の持ち物を壊す |
| B. idle の深さの上限を OS が選ぶ（latency の要る場面、gating の軸） | 深い C-state の出る時間（数百 µs の見込み、未確認）が入力・音・ゲームの応答を遅らせる時に浅くする。strategy の gating の軸の受け手にもなる | `hal_cpu_idle()` は引数を持たず、HAL の実装は kernel の値を読めない（HAL → kernel は宣言された callback だけ）。上限を渡すには宣言か callback の追加（hal.h の変更）が要る |

- A は WS332 の将来の拡張（可変の tick、WS332 design §5）と重なる。どちらの WS で扱うかも Q333-7 で聞く。
- p009 で測る判断の数: idle の package の電力、PC6 以深の滞在の率、1 秒あたりの割り込みの数（tick）、Linux（同じ機械、`turbostat` が無ければ `/sys` の cpuidle と RAPL）との比較。

### 4.4 CPU の P-state（WS301 の設計を引き継ぐ）

WS301 design §4.1〜§4.12 を引き継ぐ。変える点だけを書く。

- **driver の置き場と規則**: `src/drivers/platform/pcat/cpu-x86/`（amd64 だけ。i386 の pcat は性能を考えない対象（master-design-policy §11.1）なので入れない。WS301 の「i386 は build だけ」を外す）。driver は `hal/hal.h` を include しない。CPU の番号は `kern_cpu_id_t`、今の CPU は `kern_cpu_current()`。CPUID・RDMSR・WRMSR は driver の中の 1 つの小さな file の inline asm に閉じる（host 試験で模擬に替える）。
- **provider の口**（WS301 §5.1 の型を kernel の型に直した）:

```c
/* include/kern/cpuperf.h（案） */
struct kern_cpuperf_provider_ops {
	int (*topology)(kern_cpu_id_t cpu, struct kern_cpuperf_topology *topology);
	int (*describe)(kern_cpu_id_t cpu, struct kern_cpuperf_levels *levels);	/* RANGE or STEPS */
	uint32_t relative_sources;
	int (*relative)(kern_cpu_id_t cpu, uint32_t source, uint32_t *value);
	void (*apply)(const struct kern_cpuperf_request *request);	/* on the target CPU, interrupts masked */
	void (*sample)(struct kern_cpuperf_sample *sample);		/* on the target CPU, interrupts masked */
	int (*feedback)(kern_cpu_id_t package_cpu, struct kern_cpuperf_feedback *copy);
	int (*idle_package)(kern_cpu_id_t package_cpu, struct kern_cpuperf_idle *idle);	/* any CPU of the package */
	void (*idle_core)(struct kern_cpuperf_idle *idle);		/* on the target CPU */
};
int kern_cpuperf_provider_register(const struct kern_cpuperf_provider_ops *ops);
```

- **notify の道**: `kernel_cpu_notify_handler()`（`clock.c:193〜`）の EOI の後・`sched_cpu_notify()` の前に `kern_cpuperf_notify(cpu)` を足す（kernel の変更、WS301 §4.10 のまま）。`hal_cpu_notify()` を呼ぶのは `cpuperf.c`（kernel）だけ。
- **受け手**: `cpuperf.c` は §4.2 の受け手の口で「cpu」として登録する（WS301 の p004 の登録を、統合で枠組みの Phase p005 に移す）。apply の戻り: 対応する provider が無い → EOPNOTSUPP、1 秒の確かめの待ちが切れた → 0 と `KERN_THERMAL_APPLY_UNCONFIRMED`（要求は箱に残り後で効く。失敗に数えない、v5 M1）。
- **熱の上限**: §4.2 の受け手の `limit(context, domain, permille)`（WS301 の `kern_cpuperf_limit` の source の引数は、source が熱の 1 つだけなので外す。将来に別の源が要る時に足す）。
- **strategy の写し**（HWP、WS301 §4.4 の表のまま）: PERFORMANCE min = その時の guaranteed・max = highest・EPP 0、BALANCED lowest・highest・0x80、POWERSAVE lowest・max(guaranteed, efficient)・0xC0、MANUAL は speed と noise の式（WS301 §4.4）。legacy の段（WS301 §4.9）も同じ。値は p009 の測りで直す。
- **起動の時**: kernel は何も書かず HWP も enable しない（WS301 §4.4、firmware のまま）。省電力の strategy を起動の時に入れるかは Q333-1（推しは rc が保存した strategy を渡す、既定 balanced）。
- **`CONFIG_CPU_PERF=n`**: MSR の道は compile しない。`cpuperf.c` は topology の表と CPU の ioctl の GET だけを持ち、SET と strategy は EOPNOTSUPP（受け手の結果は「対応外」）。

### 4.5 P/E core の相対の性能と topology（WS302 が使う口）

- topology と core の種類は CPUID だけで分かる（CPUID は #GP を起こさない）ので、`CONFIG_CPU_PERF` に依らず amd64 の driver が常に出す（WS301 §10.8・Q301-1 の B の件を、MSR を伴わない部分を分ける形で解いた。§13）。
- 相対の性能（FREQUENCY は APERF/MPERF の測り、FEEDBACK は HFI、PROVIDER）は MSR を使うので `CONFIG_CPU_PERF=y` の時だけ。n の時は UNKNOWN。基準の選び・既定 FREQUENCY・5330 の見込み P 100・E 75 は WS301 §4.7 のまま。
- WS302 の口:

```c
int kern_cpuperf_cpu(kern_cpu_id_t cpu, struct kern_cpuperf_cpu *info);	/* topology, core type, relative, HFI, levels */
uint32_t kern_cpuperf_generation(void);	/* changes when relative, HFI or capabilities change */
```

  WS302 の DTB は周期の評価で世代を見て、変わった時に読み直す（callback を持たず、DTB の thread の文脈で読む）。sched への hint は WS302 の物。
- arm64・riscv64: provider 無しの既定の答え（CPU ごとに 1 core、種類 UNKNOWN、Q301-5）。

### 4.6 電力の読み（meter）（Q333-6 の推しで入れる）

- 「meter」は電力の積算（energy、µJ）を読む口。core の `kern_thermal_meter_register(name, kind, ops, context)`、kind は PACKAGE・CORES・GRAPHICS・PLATFORM・DRAM。読みだけで、制御（電力の上限の書き）はしない。
- x86 の driver（`CONFIG_CPU_PERF=y`）が RAPL の energy の MSR（候補 0x606 単位・0x611 package・0x639 PP0・0x641 PP1・0x64D PSYS、SDM Vol 4 の model ごとの MSR、allow list、p006 で確かめる）を meter として登録する。32 bit の計数は 5330 の 25 W で数時間で一周する見込みなので、core の worker が 60 秒ごとに読み 64 bit に積む。
- 用途: idle の電力の受け入れ（§8.3）、strategy の写しの調整、利用者の表示。i915 の診断の patch の読みはこれで置き換えられる（i915 の WS の判断）。

### 4.7 熱の sensor・fan・boost（WS303 の設計を引き継ぐ）

WS303 design §4.1（ACPI WMI）・§4.2（Dell）・§4.3（registry）・§4.5（fan と boost）・§4.9（Pi 5）を引き継ぐ。変える点:

- sensor の源（acpi-thermal、rpi5-fan）の push への変更と `hw.thermal` を写しから出すのは、両方とも core の Phase（p004）で行う（v5 M7: p016 が arm64 の build を落とさない）。
- `hw.thermal` の 1 source は 16 sensor まで（`sysctl.c`）なので、registry の 32 は source を 2 つに分けて出す（v5 m7）。
- WMI の attach の失敗の試し直し（1 秒おきに 3 回）は worker の thread で行い、起動を延ばさない（v5 m21）。
- BIOS が boost を戻した事の判定に周期の SMI を使わない。DDV の rpm で推し、SMM の get は SET の時だけ（v5 m10）。

### 4.8 strategy（統一の意味）

- 値: FIRMWARE（0、起動の時だけ。SET(FIRMWARE) は EINVAL、v5 m3）、PERFORMANCE、BALANCED、POWERSAVE、MANUAL。manual の時だけ 3 軸（0〜100、向きは Q333-4）。manual でない SET で 3 軸が 0 でなければ EINVAL。
- 受け手ごとの写し（各 driver が決める、Q303-11）:

| 受け手 | 写す物 | 出典の節 |
| --- | --- | --- |
| cpu（HWP） | domain ごとの min・max・EPP | WS301 §4.4 |
| cpu（legacy） | 段の範囲と governor の閾値 | WS301 §4.9 |
| dell-wmi | BIOS の thermal mode（UltraPerformance・Optimized・Cool・Quiet） | WS303 §4.2、Q333-5 |
| rpi5-fan | trip の組（DT より遅くしない） | WS303 §4.9 |
| （無し） | gating の軸 | Q333-2（推しは保存と表示だけ。CPU の idle の深さは §4.3.3 B が無いと効かせられない） |

- **SET の手順**（v5 M1 を反映）: (1) 引数の検査。(2) strategy の mutex の下で世代 g を進め、写しを置き、mutex を離す。(3) worker に依頼し 5 秒まで待つ。(4) worker は受け手を登録の順に lock を持たずに呼ぶ。**1 周が終わって最新の世代が今の周と違えば、全部の受け手にもう 1 周する**（最新が勝つ）。受け手ごとに「適用済みの世代」を写しに置き GET に出す。(5) ioctl の戻り: 受け手 0 → EOPNOTSUPP（strategy は記録）。全部が失敗 → 最初の errno。一部 → 0 と PARTIAL。自分の世代が上書きされた → 0 と SUPERSEDED。5 秒で終わらない → 0 と PENDING。EOPNOTSUPP（その受け手に当たらない）と UNCONFIRMED は失敗に数えない。
- **速さの制限は受け手の中で**: Dell の「1 秒に 1 回まで」は EBUSY を返さず、受け手が次の窓まで眠ってから書く（v5 M1）。
- **CUSTOM**: root の直の設定（CPU の domain の値）が効いている間、GET は strategy を変えずに CUSTOM の flag。次の SET で直の設定は消える（Q333-3）。
- **後から登録された受け手**: 今が FIRMWARE でなければ worker が今の世代を渡す。

### 4.9 passive（WS303 §4.7 を引き継ぐ）

- CPU の sensor（`KERN_THERMAL_FLAG_CPU`）が passive の水準を超えたら、見張りが worker に依頼し、worker が `limit` を持つ受け手（今は cpu だけ）に 5 秒ごとに 100‰ ずつ下げた上限を渡す（下限は guaranteed、WS301 §4.6）。水準 − 3 °C で戻す。事象の hysteresis も 3 °C に揃える（v5 m4）。
- 5330・Pi 5 では動く場面が無い。受け入れは host 試験まで（Q303-14）。

### 4.10 UAPI `/dev/thermal`

- device: cdev `/dev/thermal`（0x00010008、node 0666）。試験の注入は `/dev/thermal-inject`（0600、`CONFIG_THERMAL_TEST_INJECT`、platforms は amd64・arm64、v5 m11）。ioctl の group 'T'。
- 名前の接頭辞: UAPI の struct は `thermal_*`、macro は `KERN_THERMAL_*`（`include/uapi/sysctl.h` の接頭辞の無い `THERMAL_KIND_*` などとぶつけない、v5 m7）。
- ioctl の一覧（番号は p003 で確定。権限: GET は誰でも、SET は root、sessiond 経由の strategy・boost は root か wheel の利用者に代わって sessiond（root）が呼ぶ）:

| 範囲 | ioctl | 中身 | 出典 |
| --- | --- | --- | --- |
| core | GET_STATUS | sensor・fan・meter・受け手の数、strategy の要約、非常の状態（固定の欄） | WS303 §5.2 |
| core | GET_SENSOR・GET_FAN・GET_METER（index） | 1 つの sensor・fan・meter | WS303 §5.2、§4.6 |
| core | GET_STRATEGY・SET_STRATEGY | 値・3 軸・flags（CUSTOM・PARTIAL・PENDING・SUPERSEDED）・世代・firmware の mode の名前 | §4.8 |
| core | GET_RECEIVER（index） | 受け手の名前・最後の結果（errno・flags・適用済みの世代・時間） | §4.8（WS303 の strategy の struct に埋めた 16 の配列を分けた） |
| core | SET_BOOST | fan の index（全部）と段 | WS303 §4.5 |
| core | SUBSCRIBE | 事象の種類の mask | WS303 §4.8 |
| core | EMERGENCY_ACK | init が非常の停止を始めた印（§4.11） | v5 M4 |
| cpu | GET_CPU・GET_CPU_DOMAIN・SET_CPU_DOMAIN・GET_CPU_RELATIVE・SET_CPU_RELATIVE | WS301 §5.2 のまま（96・240・16・32 byte） | WS301 |
| cpu | GET_CPU_IDLE（cpu）・GET_PACKAGE_IDLE（package） | 使える C-state と滞在の時間（ns）、読んだ時刻 | §4.3.2 |
| inject | INJECT（`/dev/thermal-inject`） | 試験の sensor（同じ名前は同じ sensor に push、v5 m19）、試験の受け手、非常の道の選び（signal を送らない道。boot の parameter でなく ioctl の flag、v5 m12） | WS303 §5.2 |

- 事象: `read`・`poll` で 128 byte の record（`struct system_event` と同じ大きさと頭の形、`/dev/thermal` の自分の種類の名前空間）。種類: STRATEGY、BOOST、PASSIVE、CRITICAL、EMERGENCY、FAN_STALLED、CPU（能力・相対の性能・HFI・domain の変化）、OVERFLOW（ring が溢れた、v5 m22）。open ごとに 64 の ring。
- 大きさは `_Static_assert` と `offsetof` で固め、ILP32 と LP64 で同じ。
- **p003 で UAPI の全文の案（header）を plan に置き、ユーザーの review に出してから実装する**（WS303 の決めのまま。review の待ちで実装の Phase を止めないよう、独立の小さな Phase にした、v5 m23）。

### 4.11 非常の停止（WS303 §4.6 を引き継ぎ、v5 M3〜M6 を反映）

- **判定**（見張りの thread `thermal-guard`、1 秒ごと、写しの spinlock だけを取る、優先度は既定より高く、v5 m8）: ZONE の sensor の critical に「時刻の違う 2 つの有効な値」が続けて達したら非常。写しが 30 秒以上更新されない ZONE で最後の有効な値が critical 以上なら非常。
- **水準の検査**（v5 M6）: critical が 50 °C 未満、または起動の時の最初の値 + 5 °C 未満なら「無し」と扱い log 1 行。止める起動の parameter `thermal.emergency=off`（`include/kern/boot.h` の固定の key の表に足す、kernel の変更）。
- **S0 idle と時計**（v5 M3）: 時間（30 秒・10 秒・120 秒）は kernel の tick で数える。S0 idle の後は tick が眠った分だけ跳ぶ（`kern_clock_idle_suspend_end()`）。よって resume の時（`sleep.c` の終わりの通知）に見張りは古さの基準を置き直し、新しい読みが来るまで古さの判定をしない。非常の開始と S0 idle の入口は 1 つの atomic の flag で排他にする: 非常の後は `KERN_SYSTEM_SLEEP` が EBUSY、sleep の途中（user を凍らせる前）に非常が始まったら sleep を中止させる。凍らせた後は全 CPU が suspend しているので見張りは走らず、resume の後に判定し直す。
- **手順**（v5 M4・M5）:
  1. 事象 EMERGENCY と GET_STATUS の固定の欄。以後、kernel は `KERN_SYSTEM_REBOOT`・`HALT` を POWEROFF として扱う（`system-device.c` に hook、v5 M4 (b)）。
  2. pid 1 の SIGUSR2 の disposition が handler なら、写しの spinlock を離してから `process_find_ref(1)` → `signal_send_process(SIGUSR2)`（v5 m8）。handler でなければ 4 へ。
  3. init: SIGUSR2 の要求は他の要求（SIGINT・SIGTERM・制御の要求）に上書きされない。通常の停止の途中でも猶予を 2 + 2 秒に縮め、終わりを POWEROFF にする。init は始めた時に `EMERGENCY_ACK` を呼ぶ。kernel は SIGUSR2 から 10 秒で ACK が無ければ 4 へ、SIGUSR2 から 120 秒で電源が落ちていなければ 4 へ（v5 M4 (a)(c)）。
  4. 直の切り: sync は別の kernel の thread に頼み（`mount_sync_all()`）、見張りは 10 秒だけ待って進む（v5 M5）。`kern_platform_poweroff_emergency()`（pcat は `_PTS` を評価しない `drv_acpi_poweroff_emergency()`、rpi5 は poweroff、他の 9 platform は既定で `kern_platform_poweroff` と同じ、v5 m15）。失敗したら `kern_platform_halt` と GET_STATUS の「切れなかった」（v5 M5）。
- init が書く非常の記録の file（QEMU の試験の確かめ用）: `/var/log/thermal-emergency`、中身は起動の番号（boot の世代）と注入の時刻、製品の build でも書く（v5 m13）。
- 5330 は zone が無いので働かない（firmware と CPU の PROCHOT に任せる）。Pi 5 は DT の critical 110 °C。

### 4.12 資源・並行性・lock（統一の順）

```
 SET の ioctl:  strategy の mutex（短い、受け手を呼ばない） → 写しの spinlock
 worker:        （lock なし） → 受け手の lock（cpuperf_mutex、Dell の mutex） → AML の interpreter の lock
 cpuperf:       cpuperf_mutex → CPU ごとの箱の spinlock（葉、irqsave）。mutex を離してから確かめを 1 秒まで待つ
 notify の handler（割り込み）: CPU ごとの箱の spinlock だけ。離してから sched_cpu_notify()
 legacy の governor（thread）: cpuperf_mutex → 箱の spinlock
 見張り:        写しの spinlock だけ。signal・process の lock は写しの spinlock を離してから
 事象の発行:    thread の文脈で、受け手の lock を持たずに（handler の中では印だけ）
```

- 逆の道は無い: 受け手は thermal の lock を取らない、CUSTOM は `is_custom` を lock なしで読む。
- lock の rank（`include/kern/lock.h`、`LOCK_RANK_DEVICE` 125 など）は、同じ rank の入れ子の扱いを p004・p005 の着手の時に lock.h の規則で確かめて決める。
- 資源: 表は static で kernel の寿命。HFI の page は package ごとに 1 つ、止めの完了の前に解放しない（WS301 §4.10）。AML の object は呼んだ関数の中で解放。
- thread: worker 1（`thermal-work`、fan の 5 秒の読み、受け手の呼び、boost、passive、meter の 60 秒の積み）、見張り 1（`thermal-guard`）、sync の助け 1（非常の時だけ起こす）、legacy の governor 1（legacy の道の時だけ）。止まりうる物（AML・SMI・CPU の 1 秒の待ち）は worker だけで、見張りは呼ばない。

### 4.13 割り込みと DMA

- 新しい割り込みは使わない（HWP・HFI・WMI の事象・thermal の LVT は使わない、周期の読み）。CPU 間は既存の notify。DMA は無い。HFI の表は hardware が書く記憶（WS301 §4.7）。
- SMI: Dell の mode の set と SMM の boost（worker、受け手の中で 1 秒に 1 回まで）。

### 4.14 状態遷移

```
strategy:   FIRMWARE --SET(s)--> s（世代 g、受け手ごとの適用済みの世代、全部／PARTIAL／PENDING／SUPERSEDED）--SET(s')--> s'
            s --root の直の設定--> s + CUSTOM --SET--> CUSTOM は消える
CPU（x86）: ABSENT（hypervisor・どの bit も無い） / PROBED --最初の要求--> RUNNING(HWP) / RUNNING(LEGACY)   （WS301 §4.8）
idle:       HAL の実装の中（kernel は状態を持たない）
非常:       NORMAL --critical に 2 回・古い値--> SIGNALED --ACK--> INIT --電源断--> (終)
            SIGNALED --10 秒で ACK 無し--> CUTTING ; INIT --SIGUSR2 から 120 秒--> CUTTING ; NORMAL --init に handler 無し--> CUTTING
            CUTTING --sync を 10 秒まで待つ・poweroff--> (終) / --失敗--> HALTED（「切れなかった」）
            S0 idle の入口: NORMAL の時だけ（非常の後は EBUSY）
Dell の mode: UNPROBED --記述子・GUID・延長 0 の受け--> READY --apply--> SETTING --sentinel が書き換わった--> READY（覚えた mode）/ --失敗--> READY（不明）
fan の写し: UNKNOWN --read 成功--> OK --失敗--> STALE --成功--> OK
```

### 4.15 失敗と回復

WS301 §4.11 と WS303 §4.13 の表を引き継ぐ。統合で足す・変える物:

| 失敗 | 振る舞い |
| --- | --- |
| 関門を通った MSR を CPU が拒む（#GP） | kernel が止まる（HAL を変えない答えの代償、Q301-1）。既定 n、CPUID と allow list の関門、`cpuperf=off` の起動で避ける |
| RAPL・C-state の滞在の MSR が allow list の外 | その meter・統計を出さない（log 1 行） |
| p002 の MWAIT の後に起動しない・固まる | p002 の範囲（HAL の実装を戻す）。WS333 の後の Phase は idle を書かないので影響しない |
| 受け手が古い世代のまま | 次の周で最新を渡す（§4.8） |
| 非常の時に sync が返らない | 10 秒で進む（§4.11） |
| 非常の poweroff が失敗 | halt と GET_STATUS の印 |
| firmware の critical が低すぎる | 「無し」と扱う、`thermal.emergency=off` |

### 4.16 既存の framework との関係

- cdev・devfs: `/dev/thermal` を `cdev_register` で、node の mode は devfs の名前の表（`/dev/thermal-inject` は 0600）。
- ACPI: acpi-thermal は push に。WMI の層は ACPI の driver（WS303 §4.1）。`_OSC` は呼ばない（WS310）。
- sched: notify の handler に 1 行（§4.4）。sched の hint は WS302。
- sleep（WS052）: S0 idle と非常の排他（§4.11）。S0 idle の後に CPU の要求が保たれるかは p009 で確かめ、保たれなければ resume の hook で書き直す（WS301 §4.8）。
- `/dev/system`: UAPI は変えない。`system-device.c` に 2 つの hook（REBOOT・HALT を POWEROFF に、SLEEP の EBUSY）を足す（v5 m1。file の所有は Q1 に確かめる）。
- WS300: gating の軸の受け手の候補（ASPM・APST）。Q333-2。
- WS302: §4.5 の口。WS302 の p001 は WS333 の p003（kernel の header の案）の後に始められる。
- WS332: tick の周期。§4.3.3 A。
- WS331: driver は HAL を知らない。新しい driver は最初から守る。
- WS291 p022: idle の電力の改善を GT の周波数で確かめる（p009 に WS291 の script を借りる）。
- docs: `docs/architecture/power-management.md` は最後の Phase（p018）で。

## 5. インタフェースの要約

§4.2（thermal.h）・§4.4（cpuperf.h の provider）・§4.5（WS302 の口）・§4.10（UAPI）。完全な形は p003 で header の案として書く（kernel の 2 つの header と UAPI の 1 つ）。WS301 §5.2 の CPU の struct（96・240・16・32 byte）は形を変えずに入れる。

## 6. HAL の検討

### 6.1 使う HAL の口（全部が既存、使うのは kernel と HAL の実装だけ）

| 口 | 使う所 | 用途 |
| --- | --- | --- |
| `hal_cpu_idle()` | HAL の実装（p002） | 待ち方を MWAIT に（実装の補完、承認不要） |
| `hal_cpu_notify()`・`hal_cpu_count()`・`hal_cpu_current()` | `src/kern/cpuperf.c` | 相手の CPU で provider を呼ぶ、CPU の数 |
| `hal_rtc_read_counter()` | kernel（既存の時計） | 時間 |
| `hal_poweroff()` | kernel の platform（rpi5 の非常の切り） | 電源断 |

driver（`cpu-x86/`・`dell-wmi.c`・`acpi-wmi.c`・`rpi5-fan.c`）は `kern_*` だけを使う。

### 6.2 足りるか

- この設計の Phase p003〜p016・p018 は **HAL を変えずに作れる**（hal.h の差分は無い。`proposed/` も作らない）。
- 足りない所は §4.3.3 の 2 つ（idle の CPU の tick を止める、idle の深さの上限）だけで、どちらも「要るかを p009 で測ってから」の条件つき。足りない理由は §4.3.3 の表。測りで要ると出た時に、既存の口で代えられない事（例: `hal_cpu_idle_suspend()` の流用の可否、kernel の側の timer の作りの直し）を改めて検討し、差分の案とその理由を示して承認を求める（Q333-7）。
- MSR の #GP の安全網（WS301 の案 C）は Q301-1 の答えで作らない。

## 7. Phase の受け入れの物差し（要約は §9 の表）

- build: 全 platform の既定の build と、`CONFIG_CPU_PERF=y`・`CONFIG_THERMAL_TEST_INJECT=y` の build の warning 0（sparcv9 はユーザーの物なので Q1 に確かめてから）。
- 判定は console の log に依らない（ssh・QMP・道具の出力・時間）。

## 8. 引き継ぎと食い違いの整理

### 8.1 WS301 と WS303 の設計の食い違いと、統合での答え

| 食い違い | WS301 | WS303 | 統合の答え |
| --- | --- | --- | --- |
| 上限の口の引数（v5 M2-2） | `(source, domain, permille)` | `(source, permille)` | 受け手の ops の `limit(context, domain, permille)`（§4.2） |
| `kern_cpuperf_strategy` を直に呼ぶか（v5 M2-3） | 直に呼ぶ symbol に読めた | 登録だけ | 登録だけ（§4.2） |
| apply の結果（v5 M2-4） | 適用・一部・対応外 | int だけ | 0・EOPNOTSUPP・errno と UNCONFIRMED の flag（§4.2） |
| ETIMEDOUT の扱い（v5 M1） | 要求は箱に残り後で効く | 失敗に数える | UNCONFIRMED、失敗に数えない |
| 受け手の登録の Phase（v5 M2-5） | WS301 p004 | WS301 p004 | cpuperf の枠組みの Phase p005（§9） |
| lock の順（v5 M2-1） | 改訂 3 で直した | 受け手を lock なしで呼ぶ | §4.12 |
| 登録の口の数 | 受け手・上限・ioctl の範囲・is_custom の 4 つ | 同じ | 受け手の口 1 つ（上限と CUSTOM を含む）と ioctl の範囲（§4.2） |
| WS303 の apply の引数（v5 m2） | — | §4.4 は 3 つ、§5.1 は 2 つ | §4.2 の形 |
| i386 | build だけ | — | 入れない（§4.4） |
| topology と config | `CONFIG_CPU_PERF` の中、WS302 が要れば分ける（WS301 §10.8） | — | CPUID だけの部分は常に（§4.5） |
| 熱の上限の source の引数 | `KERN_CPUPERF_LIMIT_THERMAL` の 1 つ | — | 外す（§4.4） |
| 電力の読み | 無し | 無し | meter（§4.6、Q333-6） |
| CPU の idle | 範囲の外 | gating の軸だけ（Q303-16） | §4.3 |

### 8.2 規則の変化（2026-10-30 の「driver は HAL を知らない」）

WS301 の provider の口の `hal_cpu_id_t`、driver が notify の道を知る前提を、`kern_cpu_id_t` と「`hal_cpu_notify()` は `cpuperf.c` だけ」に直した（§4.4）。

### 8.3 WS303 第 5 版の review（M1〜M9・m1〜m25）の行き先

| 指摘 | 行き先 |
| --- | --- |
| M1 世代の追い越し | §4.8 |
| M2 WS301 との契約 | §4.2・§8.1 |
| M3 S0 idle の時計 | §4.11 |
| M4 init の道の穴 | §4.11 |
| M5 sync と電源断の失敗 | §4.11 |
| M6 誤った非常の停止 | §4.11、p010 の受け入れ（zone を持つ amd64 の機械、§9） |
| M7 Phase の依存 | §4.7、§9（push は p004、注入は p004） |
| M8 Dell の mode の確かめ | §4.14（成否は sentinel、ODV0 は表示の補い、resume の後は覚えた値）、p012 の試験の 3 の前に「Debian で set → 再起動 → 読む」で mode が残る事と profile の daemon が無い事を読みで確かめる（WS303 §6.3 の 3 に足す） |
| M9 SMM の boost の手順の出典 | §11 の Q1 の判断（失敗の印の値を GPL の file から値として取ってよいか）。手順は「書いた後に DDV の rpm と get で読み直す」と自分で組み、確かめられなければ boost は無し |
| m1 | §4.16 |
| m2 | §4.2 |
| m3 | §4.8 |
| m4 | §4.9 |
| m5 | p011・p012 の着手の時に WS303 §2.1 の「ECM1 の下」を「0x20 以外は」と読む |
| m6 | §2.3（行番号を固定の事実にしない） |
| m7 | §4.7・§4.10 |
| m8 | §4.11 |
| m9 | §4.2 |
| m10 | §4.7 |
| m11 | §4.10 |
| m12 | §4.10 |
| m13 | §4.11 |
| m14 | §4.11（`mount_sync_all`） |
| m15 | §4.11 |
| m16 | WS301・WS303 は canceled。この文書が本体 |
| m17 | §10（出典の字面） |
| m18 | §8.4 の 5330 の熱の試験で、合否の手順と記録だけの手順を分ける（p012 の phase.md） |
| m19 | §4.10 |
| m20 | §4.2 |
| m21 | §4.7 |
| m22 | §4.10 |
| m23 | §4.10・§9（p003） |
| m24 | WS303 §2.3 の「ACPI の thermal zone」は「Linux の thermal_zone（DPTF の participant）」と読む（p012 の phase.md に写す） |
| m25 | §4.14（古さの道・電源断の失敗・REBOOT/HALT・UNCONFIRMED） |

### 8.4 試験の計画

- **host 試験**（`plan/ws333/tests/`）: WS301 §6.1 と WS303 §6.1 の全部、加えて: 世代の 2 周目と SUPERSEDED、受け手の UNCONFIRMED、Dell の窓の眠り、`limit` の domain、meter の 32 bit の一周の積み、C-state の滞在の decode、非常の ACK の道（10 秒・120 秒）、REBOOT との競合と停止の途中の SIGUSR2、sync が返らない時の 10 秒、水準の検査（50 °C・起動 + 5 °C）、S0 idle の跳ぶ時計と古さの置き直し、UAPI の layout（-m64・-m32）。x86 の decode の module（§9 の p007a〜e）は標準廉価実装手順の単体試験（ASan・UBSan、逆方向の独立の実装、わざと壊す確かめ）。
- **QEMU**（amd64 q35、ssh・QMP・時間で判定）: `/sbin/thermal` が sensor・fan 0・受け手の結果（cpu は「対応外」= hypervisor の関門）を出す、`boot-test.sh` の回帰、riscv64 の virt でも `/sbin/thermal` が動く、非常の init の道と直の切りの道（WS303 §6.2 の形、ACK の道を足す）、sessiond（p013）。
- **5330**（1 本の script、Q1 の割当、WS291 の `5330-mview.sh` の考え方を借りる）:
  1. idle の電力: p002 の前・p002 の後・p008 の後（strategy BALANCED）で、desktop の session の idle の package の電力（meter）、PC6 以深の滞在の率、tick の割り込みの数を 60 秒ずつ。同じ日に Linux の値（RAPL・cpuidle）。
  2. i915: WS291 p022 の qs40 × 600 の fps と GT の周波数（PL1 の制限が和らぐか）。
  3. HWP の 4 strategy と直の設定で、1 CPU の spin の MHz が要求に従う（WS301 §6.3 の 2）。E-core の 4 つ組の共有（WS301 §6.3 の 3）。
  4. legacy の道（`cpuperf.hwp=off`、WS301 §6.3 の 4）。相対の性能の基準（同 5）。S0 idle の回帰（同 7）。
  5. 熱（p012）: WS303 §6.3 の 1〜4、M8 の前の確かめ、合否と記録の分け（m18）。
- **Pi 5**: WS303 §6.3 の Pi 5。
- **zone を持つ amd64 の機械**（v5 M6、p010 の受け入れ）: SV7 か 5320 に ACPI の thermal zone があるかを読み、あれば ZONE と critical を示し非常にならない事。無ければ「未実施」と記録し Q1 に返す。

## 9. Phase の分け方の案（ws.md の表の本体はこの表、±50%、1 LW ≈ 実時間 15〜25 分）

CPU の省電力を先にする（2026-10-31 の指示）。クリティカルパスは p003 → p005 → p008 → p009。x86 の decode の module（p006・p007a〜e）は UAPI の review を待たずに並べて進められる。

| Phase | 内容 | 依存 | LW | 受け入れ | 担当の型 |
| --- | --- | --- | --- | --- | --- |
| p001 | この設計、design-reviewer、問い | — | — | review の反映、問いの一覧 | 設計（P5） |
| p002 | amd64 の idle を MWAIT に（HAL の実装） | — | （進行中） | p002 の phase.md | P1 |
| p003 | header の案: `include/uapi/thermal.h`（全文）と `include/kern/thermal.h`・`include/kern/cpuperf.h`（kernel の口）を `plan/ws333/proposed/` に、layout の host 試験の案。**UAPI はユーザーの review** | p001 | 2 | ユーザーの review の答え、-m64・-m32 の layout の試験 | Opus |
| p004 | 熱の core: 写し・sensor の push（acpi-thermal と rpi5-fan を push に）・fan・meter・strategy と受け手（世代の 2 周目）・boost・worker・ioctl の振り分けと範囲の口・事象・`/dev/thermal`・`/dev/thermal-inject` と試験の受け手・`/sbin/thermal`・`hw.thermal` を写しから（2 source） | p003 | 7 | §8.4 の core の host 試験、全 platform の build、QEMU の回帰 | Opus |
| p005 | cpuperf の枠組み（kernel）: topology の表・domain・要求の箱と notify の道（clock.c）・provider の口・相対の性能と基準・受け手「cpu」の登録・直の設定と CUSTOM・上限・legacy の governor の thread・CPU の ioctl（idle の統計を含む）・WS302 の口 | p003（p004 と並べてよい。結合の試験は p004 の後） | 5 | host 試験（模擬の provider）、build | Opus |
| p006 | x86 の decode の module の設計（標準廉価実装手順の設計の Phase）: header（仕様の comment）・.c の骨格・単体試験の道具・骨格の照合の script・p007a〜e の phase.md。仕様は Intel SDM（rev と path と SHA-256 を記録）・AMD APM・ACPI 6.5（`_PSS` の package の形）。model の allow list の表もここで SDM Vol 4 から決める（ADL の 0xCE・RAPL・C-state の滞在の MSR を確かめる） | p001 | 2 | 骨格の build、design-reviewer、phase.md 5 本 | Opus |
| p007a | module: topology（CPUID 1FH/0BH・04H・1AH から package・die・module・core・thread・種類、5330 の表の試験） | p006 | 1 | 単体試験 PASS、わざと壊す確かめ | Sonnet（phase-runner-low） |
| p007b | module: HWP（CPUID.06H の bit、CAPABILITIES の decode、REQUEST の encode、範囲の検査、strategy の表と MANUAL の式、efficient より下にしない） | p006 | 2 | 同上 | Sonnet |
| p007c | module: HFI の表の解析（頭・行・class × 能力、0 の行、更新の印） | p006 | 1 | 同上 | Sonnet |
| p007d | module: legacy の段（`_PSS` の package、0xCE の比、AMD の HwPstate）と governor の判断の関数（使用率 → 段） | p006 | 2 | 同上 | Sonnet |
| p007e | module: RAPL の単位と energy の積み（一周）、C-state の滞在の計数の decode、CPUID.05H の状態の一覧 | p006 | 1 | 同上 | Sonnet |
| p008 | x86 の CPU の driver の統合: 関門（hypervisor・CPUID の bit・allow list・既定 n・`cpuperf=off`）、MSR の小さな file、probe、HWP の enable の順と既に 1 の道、Highest の MHz の測り、HFI の page、legacy の道、RAPL の meter・滞在の統計、provider の登録 | p005、p007a〜e | 5 | host 試験（模擬の CPUID・MSR）、amd64 の build、QEMU で provider 無し（関門 1） | Opus（phase-runner-high） |
| p009 | 5330 の CPU の受け入れ（§8.4 の 5330 の 1〜4、1 本の script）。§4.3.3 の判断の数を出す | p008、p002、p004 | 4 | script の PASS、数の記録、Q333-7 の材料 | Opus＋Q1 の実機の割当 |
| p010 | 見張りと非常の停止（signal の disposition の口、init の SIGUSR2 と ACK と上書きの禁止、REBOOT・HALT の hook、SLEEP の排他、sync の助けの thread、`drv_acpi_poweroff_emergency`・platform の口・halt、水準の検査と `thermal.emergency=off`、記録の file） | p004 | 6 | 非常の host 試験、QEMU の非常の 2 本、zone を持つ amd64 の機械の確かめ（無ければ未実施と記録） | Opus |
| p011 | ACPI WMI の層（`_WDG` の解析は Microsoft の文書に基づく module、attach、method・query の口）と AML の harness の `plan/tools/aml-host/` への移し | p001 | 6 | WMI の host 試験、SMI を起こさない、amd64 の build | Opus（`_WDG` の解析だけ標準廉価実装手順の 1 module にしてよい） |
| p012 | Dell の provider（DDV・mode・受け手・SMM の boost の確かめ）と 5330 の熱の試験 | p004、p011 | 7 | WS303 §6.3 の 5330 の 1〜4（v5 M8 の前の確かめ、m18） | Opus |
| p013 | sessiond の THERMAL の要求（strategy・boost、root か wheel） | p004 | 2 | rules の host 試験、QEMU（試験の受け手は p004 の注入の build） | phase-runner |
| p014 | Pi 5 の provider（fan の level・rpm、受け手、boost、lock の範囲） | p004、p010 | 3 | Pi 5 の試験 | phase-runner-high |
| p015 | passive（見張り → worker → 受け手の `limit`） | p005、p010 | 2 | host 試験（模擬の上限の受け手） | phase-runner |
| p016 | sysctl の道具と Keiland の monitor を `/dev/thermal` に、`hw.thermal` を外す（`HW_THERMAL 9` は欠番の comment） | p004 | 2 | 2 つの読み手の build と表示、`hw.thermal` の grep が 0 | phase-runner |
| p017 | （条件つき）idle の tick の停止・idle の深さの口の設計（HAL の差分の案の要否を含む） | p009、Q333-7 の答え | — | Q333-7 で A の答えと要る数が出た時だけ立てる | — |
| p018 | 規約の全文の見直しと文書（`docs/architecture/power-management.md`） | p004〜p016 | 2 | 規約、文書 | conformance-reviewer・document-writer |

合計 約 64 LW（p001・p002・p017 を除く。WS301 の約 25 と WS303 の約 35 の和より少し多いのは meter・C-state の統計・decode の module の分割と試験の道具・v5 review の反映の分）。

- **標準廉価実装手順の適用**: p006・p007a〜e（Intel SDM・AMD APM・ACPI の `_PSS` の形に決まる純粋な decode・encode で、host で単体試験できる）。p011 の `_WDG` の解析も条件を満たす（Microsoft の文書）。kernel の枠組み・driver の統合・実機の試験・非常の停止は、hardware と kernel の振る舞いを試しながら作るので普通の Phase（手順の §1 の「満たさない時」）。
- 仕様の PDF の置き場所（`build/sources/specs/`）は p006 で決める（WS301 の Fable の写しは scratchpad にあり消えうる）。
- ws.md の Phase の表はこの表の写し（p003 以降は Q1 が問いの答えの後に立てる）。

## 10. 出典と引用

- WS301 §8・WS303 §8 を引き継ぐ: 値（MSR・CPUID の bit・表の形・ACPI の offset・GUID・EC の番地）は block ごとに「Value referenced:」「Values referenced:」（v5 m17、guardrail の字面）、手順は「Procedure referenced:」、複数行の comment、file の先頭に書かない。
- Intel SDM・AMD APM・ACPI・Microsoft の WMI の文書は版を明記する。FreeBSD（BSD-2）は考え方だけ、code・名前・構造は写さない。
- Dell の SMM の値は Q303-12 の答えで GPL の file から値として出典つきで取る。手順は GPL の file から取らない（WS303 §8、Q1 の答え 2026-10-29）。失敗の印の値の扱いは §11 の Q1 の判断。
- 5330 の観測の値は試験の期待の値（`plan/ws333/tests/`）として置き、source の定数にしない。
- この設計のために GPL の source は読んでいない。web も使っていない。

## 11. 判断が要る点

### 11.1 ユーザー（[user-questions.md](user-questions.md) に背景・用語・案・推し）

| 問い | 要点 | 推し |
| --- | --- | --- |
| Q333-1 | 起動の時の strategy（旧 Q303-15 を、CPU の省電力を先にする指示を受けて聞き直す） | B: rc が保存した strategy を渡す（既定 balanced、Dell は今の mode と同じなら書かない） |
| Q333-2 | gating の軸が何に効くか（旧 Q303-16） | C: 保存と表示だけ。受け手は後の WS・Q333-7 の口が出来た時 |
| Q333-3 | root の直の設定と strategy（旧 Q303-17） | A: 直の設定が優先し CUSTOM、次の strategy で消える |
| Q333-4 | 3 軸の向き（旧 Q303-18） | A: 100 = その物が多い |
| Q333-5 | Dell の Quiet を選んでよいか（旧 Q303-19） | A: manual で noise ≤ 33 の時だけ |
| Q333-6 | 電力の読み（RAPL）と C-state の滞在の統計を /dev/thermal に入れるか（新） | A: 入れる（読みだけ） |
| Q333-7 | idle の tick の停止・idle の深さの口（HAL の口が要る）をどう扱うか（新） | A: p009 の測りの後に、数と HAL の差分の案と理由を示して決める |

### 11.2 Q1

1. WS303 v5 M9: Dell の SMM の呼び出しの「失敗の印」（返りの register の値）を、GPL の file から値として取ってよいか（値か手順か）。
2. WS301・WS303 の design.md は WS333 の設計が節を名指して引き継ぐので、WS333 の完了まで消さない。
3. `src/drivers/generic/system-device.c`（`/dev/system`）に 2 つの hook を足す事の所有（WS052 との調整）。
4. `userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c` の所有（p016、Keiland の WS との調整）。
5. p006 の仕様の PDF の置き場所（main の checkout の `build/sources/specs/` に置いてよいか）。

## 12. 未確認

1. 5330 の idle の電力の内訳（C-state・tick・P-state のどれがどれだけか）。p002 と p009 で測る。
2. ADL（family 6 model 0x9A）が SDM Vol 4 の 0xCE・RAPL・C-state の滞在の MSR の表にあるか、滞在の計数の rate（p006）。
3. 5330 の MWAIT の C-state の出る時間（latency）と、tick の 1 ms との関係（p002・p009）。
4. WS301 §10 の 1〜9（E-core の 4 つ組の共有、HFI の値、04H の値、KVM の HWP、SV7 の HWP、PM_ENABLE、legacy の PERF_CTL、など）。
5. WS303 §10 の 1〜6（Dell の mode の bit、延長 0、SMM の boost、DDDR、device の番号は §2.3 で確かめた）。
6. zone を持つ amd64 の機械（SV7・5320）があるか（v5 M6）。
7. `kern_platform_halt` の名前と全 platform の有無（p010 で確かめる）。

## 13. 選ばなかった案（統合で新しく決めた物）

| 案 | 理由 |
| --- | --- |
| 統合の後も WS301・WS303 の 4 つの登録の口を残す | 1 つの WS の中で、受け手の口に上限と CUSTOM を入れれば足りる。食い違い（v5 M2）の元を減らす |
| `cpuperf.c` を `CONFIG_CPU_PERF=n` で外す | WS302 が topology を要る（WS301 §10.8）。CPUID は #GP を起こさないので常に入れて安全。MSR の道だけを config にする |
| idle の C-state を kernel が選ぶ（Linux の cpuidle の governor の形） | `hal_cpu_idle()` が引数を持たず、HAL の変更が要る。要るかは測ってから（§4.3.3） |
| `hal_cpu_idle_suspend()` を普段の idle に使って tick を止める | 契約は系の S0 idle 用で、局所の割り込み源も静める。CPU ごとの時間つきの sleep が起きなくなる。契約の読み替えはユーザーの承認が要る。測りの後に Q333-7 で検討する |
| 起動の parameter で HWP を balanced にする近道（UAPI の前に省電力を入れる） | WS301 の Q301-7（自分の policy・起動の parameter を持たない）に反する。代わりに decode の module を UAPI の review と並べて短くする（§9） |
| RAPL の電力の上限（PL1・PL2）を書く | 到達目標の外で、firmware・Dell の mode と衝突しうる（WS303 §2.4 の Dell の KB）。読みだけ |
| x86 の driver を i386 にも | i386 は性能を考えない対象（master-design-policy §11.1）。build と試験の手間に見合わない |

## 14. design-reviewer の指摘と反映

（design-reviewer の後に書く）
