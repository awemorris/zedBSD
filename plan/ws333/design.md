<!-- awesome-plan project=zedbsd record=ws333-design -->

# WS333 設計: /dev/thermal で統一する電力・熱の枠組み（CPU の idle・P-state・P/E core、熱・fan・strategy・非常の停止）

- 版: **第 2 版**（2026-10-31 P5、q1734-i01）。第 1 版（500061ba1）に design-reviewer の指摘（[design-review.md](design-review.md)、blocker 0・major 8・minor 24）と、main に merge された p002 の結果（f740839e5）を反映した。行き先は §14。
- 入力: WS301 の design.md 第 3 版の改訂 3（CPU の DVFS と P/E core）と WS303 の design.md 第 5 版（熱と fan）。WS303 第 5 版の review（[../ws303/design-v5-review.md](../ws303/design-v5-review.md)、WS303 では未反映）はこの設計に反映した（§8.3）。
- 基点: worktree `p5-ws333`（branch `agent/p5-ws333`、main aa112273a）。p002 の実装は main f740839e5 で読んだ。引用の file:行 は基点の物（WS301・WS303 の引用は main 68ee65381 の物で、その後に変わった file は §2 で読み直した）。code は書いていない。
- 読んだ物: WS301 の ws.md・design.md・user-questions.md・hal-review.md・design-v3-review.md、WS303 の ws.md・design.md・user-questions.md・design-v5-review.md、WS302 の ws.md、WS299 の ws.md の結果（2026-10-28）、WS291 p022 の phase.md、WS332 の ws.md・design.md §5、WS331 の ws.md、p002 の phase.md（main）、`plan/guardrail.md`、`plan/master-design-policy.md`、`docs/agent/spec-module-delegation.md`、`include/hal/hal.h`、source（§2）。

## 0. ユーザーの決定（要点）

記録の規則（2026-10-30）により発言の原文は載せない。決定の記録は WS301・WS303 の ws.md と WS333 の ws.md にある。

| 日付 | 要点 | この設計での扱い |
| --- | --- | --- |
| 2026-10-31 | WS301 と WS303 を廃止して統合し新しい WS にする。/dev/thermal という統一の device を検討の途中で想定するようになったので、統一して設計し直す | この文書。2 つの WS の口を 1 つの kernel の口にまとめる（§4.2） |
| 2026-10-31 | CPU の省電力（idle の C-state・P-state）を先に直す。amd64 の idle（MWAIT）は p002 で先に実装する | CPU の道を先にする Phase の順（§9）。idle の状態の選びは HAL の実装（§4.3） |
| 2026-10-29（WS301 Q301-1・Q301-8） | HAL は変えず、driver の確かめで守る。HWP の無い CPU も P-state を driver で操作する | §4.4・§6 |
| 2026-10-29（WS301 Q301-2） | 相対の性能の既定は最大の周波数の比で、後で写し方を変えられる口にする | §4.5 |
| 2026-10-29（WS301 Q301-3・4・5・7） | `_OSC` は呼ばない（WS310）、`CONFIG_CPU_PERF` は試験が通るまで既定で無効、arm64・riscv64 は枠だけ、CPU の速さの側は自分の policy と起動の parameter を持たない（直の値は root の診断だけ） | §4.4、Q333-7 |
| 2026-10-29（WS301、Q1 の中継） | CPU の速さの UAPI は /dev/system でなく /dev/thermal | §4.10 |
| 2026-10-29（WS303 Q303-1〜14） | `/dev/thermal`、変更は root と sessiond 経由（root か wheel）、`/sbin/thermal` が strategy を選ぶ、fan は上げる段だけで温度に必要な回転数を下回らせない、非常の水準で電源を落とす、制御できない機械は firmware に任せ strategy だけを伝える、passive を入れる、strategy は 4 つ（manual は 3 軸 0〜100、写し方は driver が決める）、`hw.thermal` は monitor を移した後に外す、Dell の SMM の値は GPL の file から出典つきで、非常の水準は zone の critical だけ、passive は仕組みと host 試験まで | §4.6〜§4.11 |

WS303 の未回答の問い（Q303-15〜19）は Q333-1〜5 として聞き直す。新しい問いは Q333-6・7（§11、[user-questions.md](user-questions.md)）。

## 1. 目標と範囲

- 目標（ws.md）: CPU の idle（C-state）・P-state（HWP と、HWP の無い CPU の driver の P-state）・P/E core の相対の性能・熱の監視・fan・非常の停止を `/dev/thermal` と `/sbin/thermal` の下で統一して設計し、実装する。
- 最初の対象: 5330（Latitude 5330、i5-1245U、P-core 2 と E-core 8）。熱と fan は Pi 5 も。
- 範囲の外: `_OSC`（WS310）、ASPM・APST（WS300）、display の PSR・GPU の power gating（各 GPU の WS）、tick の周期を変える仕組み（WS332 の将来の拡張）、DTB の本体（WS302）、Keiland の UI。
- 受け入れの柱（§8.4、p014 の PASS の定義）: (1) 同じ image・同じ器具（§4.6 の meter）で、HLT に戻した build より MWAIT の build の idle の package の電力が低く、tick の位相を揃えた build と揃えない build の差を記録する（Linux との差をどれだけ埋めるかの数の目標は p014 の前に Q1 がユーザーと決める、§11.2）、(2) strategy の 4 つで CPU の要求の MHz が要求に従う、(3) 熱・fan・strategy・電力を `/sbin/thermal` で読める・選べる、(4) 非常の停止が QEMU の注入で 2 つの道とも動く。

## 2. 今の振る舞い（code の事実）

### 2.1 CPU の idle と tick

- amd64 の `hal_cpu_idle()`（契約は「割り込みを許して止まり、割り込みを止めて戻る」だけ、`include/hal/hal.h:194〜197`。待ち方は HAL の実装が決める）は、p002（main f740839e5）で MWAIT になった: `task.c` が `bsp_cpu_idle()`（`src/hal/amd64/bsp-pcat/idle-suspend.c`）を呼び、起動の CPU で一度、MONITOR/MWAIT・leaf 5 の拡張と masked の割り込みでの break・ARAT・invariant TSC を確かめ、`IDLE_WAIT_DEEPEST_CSTATE`（既定 3 = Alder Lake の C6）以下の最深の C-state の最深の sub-state を hint にする。どれかが無い CPU は `sti; hlt; cli`。tick は止めない。`IDLE_WAIT_DEEPEST_CSTATE=1` の build は HLT になる（比べの器具、§8.4）。
- 系の S0 idle は別の口 `hal_cpu_idle_suspend()`（`hal.h:213〜232`、LPIT の hint、待つ間は LAPIC の tick を止め、kernel は `hal_rtc_read_counter()` から tick を進める、`src/kern/clock.c` の `kern_clock_idle_suspend_begin/end`）。
- idle の loop: `sched_idle()`（`src/kern/sched.c:1562〜1599`）→ `kern_sleep_idle(cpu)`（`src/kern/sleep.c:211〜`）→ `hal_cpu_idle()`。
- tick: amd64 は `HAL_TIMER_FREQUENCY` 1000 Hz（`include/hal/arch/amd64.h:16`）、各 CPU の LAPIC の周期の timer を、その CPU が起きた時刻に始める（`src/hal/amd64/bsp-pcat/lapic.c:252〜276`）ので、**CPU 間の tick の位相は揃っていない**。`kernel_timer_handler()`（`clock.c:96〜`）は CPU 0 だけが `kernel_ticks` と process の timer を進め（`clock.c:108〜114`）、全 CPU が `sched_clock_cpu()` を呼ぶ。CPU ごとの時間つきの sleep の起床はその CPU の tick（`sched.c:1025〜1039`、thread は CPU に固定）、CPU 時間の計数（`charge_cpu_time`、`sched.c:1045`、`hw.cputimes`）と乱数の種（`kern_random_tick`、`clock.c:120`）も tick ごと。tick を止める・一度だけ鳴らす口は HAL に無い（`hal.h:546〜556` は周期の tick だけ）。

### 2.2 CPU の性能

- P-state・HWP・CPPC・RAPL・C-state の滞在の計数の code は無い。
- CPU の口: `hal_cpu_notify()`（`hal.h:154〜171`、受け手 `kernel_cpu_notify_handler()` は `clock.c:193〜`）、`hal_cpu_count()`、`hal_cpu_current()`。kernel の側の型 `kern_cpu_id_t`（`include/kern/types.h:21`、`hal_cpu_id_t` と同じ型を `sched_private.h:23` が確かめる）、`kern_cpu_current()`（`sched.h:202`、戻りは `unsigned`）。CPU の数・最大を driver に渡す kernel の口は無い（`include/kern/` に無い）。
- guardrail「driver は HAL を知らない」（2026-10-30）: driver は `hal/hal.h` を include せず `hal_*` を呼ばない。driver の中の CPU の命令の inline asm の前例はある（`src/drivers/gpu/i915/device.c:2265` の CPUID、`pcat-ide.c:218` の port の命令）。MSR がこの規則の「I/O」に入るかは書かれていない（§11.2 で Q1 に確かめる）。
- amd64（Intel Mac の config も、`config/ci/config-intelmac.mk:3`）は `src/drivers/platform/pcat/` の driver を build する（`platform/amd64/vmunix.mk:483` ほか）。
- `kern_platform_poweroff`・`kern_platform_halt` は `include/kern/platform.h:46〜59`、`src/kern/platform/` の 12 file 全部が定義する。起動の parameter は固定の enum（`include/kern/boot.h:107〜129`）と `src/kern/boot.c` の名前の表。

### 2.3 熱

- `hw.thermal`（`include/uapi/sysctl.h:240〜313`、`HW_THERMAL 9`、`struct thermal_header` 24 byte・`struct thermal_entry` 64 byte）、registry `src/kern/sysctl.c:147〜240`（4 source × 16 sensor）。源は `acpi-thermal.c`（5 秒、38・112・252 行）と `rpi5-fan.c:330`。読み手は `userland/base/sysctl/main.c`（99・226・749〜803）と `monitor-zedbsd.c`（643〜681）。
- `/dev/thermal`・`include/uapi/thermal.h`・`include/kern/thermal.h` は無い。cdev 0x00010008 と ioctl の group 'T' は空き。`struct system_event` は 128 byte（`include/uapi/system.h:357〜368`）。
- init の signal は SIGHUP・SIGINT・SIGTERM（`userland/base/init/main.c:293〜295`）。`/dev/system` の HALT・POWEROFF・REBOOT は pid 1 だけ（`src/drivers/generic/system-device.c:773〜812`）。sync は `mount_sync_all()`（`src/kern/mount.c:946`）。`drv_acpi_poweroff` は `_PTS` を評価してから event の lock を取る（`acpi-event.c:1011〜1064`）。
- WS303 第 5 版の §1 の他の事実の行番号は、その後に変わった file（`acpi-kern.c`・`system-device.c`・`devfs.c`・`vfs.c`・`sysctl.c`）でずれている。意味は p005・p016 の着手の時に読み直す。

## 3. 調べた事実（引き継ぎと p002）

- 5330 の CPU（WS301 design §2.3、Debian で読みだけ）: hypervisor 無し、HWP・EPP・HFI・HWP_CTL・Thread Director 有り、P-core 2（4 thread）・E-core 8（4 つ組 2 つ）、HWP の能力 P（56・21・20・1）・E（33・12・15・1）、測った Highest の MHz は P 4388・E 3292、TjMax 100 °C、PL1 25 W・PL2 55 W（Dell の mode の条件は未確認）。
- 5330 の熱（WS303 design §2.1〜2.5）: ACPI の thermal zone は無い（DPTF の participant だけ）、fan は EC が回し、OS の口は Dell の WMI（DDV の読み、SMBIOS の thermal mode、SMM）だけ。
- 5330 の電力（WS291 p022 と p002、器具は GT の MMIO の MCHBAR の写しを読む診断の patch、source の外）: idle の package は HLT 13.00 W → MWAIT C6 11.70 W → C10 11.59 W（C6 と C10 の差は揺れの内、p002 は C6 を既定）。Linux は 3.2〜3.5 W。PL1 13.0 W。mview c600 は HLT 30.5〜30.7 fps → C6 32.6〜34.9 fps。
- 見立て（推定、p014 の package の C-state の滞在で確かめる）: 残りの約 8 W の差は、(a) 1 kHz の tick が 12 CPU で位相が揃わずに来て package の深い C-state（PC6〜PC10）に入る間が短い事、(b) P-state の要求が firmware のまま（HWP 無効）、(c) package の深い状態を妨げる他の device（PCIe の ASPM・LTR は WS300、display の PSR は i915 の WS、未調査）。(a) は p012、(b) は p011、(c) は範囲の外で、p014 の滞在の数で他の WS に渡す。

## 4. 設計

### 4.1 全体の形

```
 user:   /sbin/thermal（表示・root の設定・保存した strategy を rc から）   sessiond（root か wheel の利用者）   Keiland の monitor（p022 から）
                 \                                  |                                             /
 UAPI:                      /dev/thermal（0666、SET は ioctl の中で root）   /dev/thermal-inject（0600、試験の build だけ）
                                                    |
 kernel: src/kern/thermal.c（core: 写し・strategy・受け手・meter・事象・ioctl の振り分け・thread）
         src/kern/thermal-guard.c（見張りの thread・非常の停止）
         src/kern/cpuperf.c（CPU: topology の表・domain・要求の箱と notify の道・相対の性能・strategy の写し・governor・受け手の 1 つ・CPU の ioctl）
                 ^ 登録（kern_* の口だけ）
 driver: src/drivers/platform/pcat/cpu-x86/（CPUID・MSR、HWP・legacy・HFI・RAPL・C-state の滞在、amd64）
         src/drivers/acpi/acpi-thermal.c（sensor の push）  src/drivers/acpi/acpi-wmi.c ＋ src/drivers/platform/pcat/dell-wmi.c（fan・Dell の mode・boost）
         src/drivers/platform/rpi5/rpi5-fan.c（sensor・fan・受け手・boost）
 HAL:    hal.h は変えない。idle の状態の選び（p002）と tick の位相（p012）は HAL の実装。hal_cpu_notify() を呼ぶのは kernel の cpuperf.c だけ
```

- kernel の core は「写しと方針の配り」、driver は「hardware の読み書き」。strategy の意味は core が定め、写し方は各 driver が決める（Q303-11）。
- `CONFIG_CPU_PERF`（既定 n）は **MSR を読む・書く道だけ**を入れる。CPUID だけで分かる topology と core の種類は amd64 で常に入れる（§4.5）。熱の core は全 platform で常に入る（`hw.thermal` を置き換えるため）。既定を y にするかは Q333-7。

### 4.2 kernel の口の統一

WS301 と WS303 の 4 つの登録（strategy の受け手、熱の上限の受け手、ioctl の範囲、`is_custom`）は引数が食い違っていた（WS303 v5 M2）。統合で **受け手の口 1 つ**（strategy・上限・CUSTOM・firmware の名前）と **ioctl の範囲の口**にする。

```c
/* include/kern/thermal.h（案。名前は p003 で coding-style に合わせる。
   strategy の値・flag などの共有の定数は include/uapi/thermal.h で 1 回だけ定義し、ここは include する） */
struct kern_thermal_strategy {
	uint32_t value;			/* KERN_THERMAL_STRATEGY_* */
	uint8_t speed, noise, gating;	/* 0..100; 0 unless MANUAL */
	uint8_t reserved;
};

struct kern_thermal_receiver_ops {
	/* the strategy thread, may sleep, no thermal lock held;
	   0, EOPNOTSUPP (does not apply here) or an errno; *flags may get KERN_THERMAL_APPLY_UNCONFIRMED */
	int (*apply)(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);
	/* the poll thread; domain KERN_THERMAL_DOMAIN_ALL or the receiver's own; permille of its range; NULL: no passive */
	int (*limit)(void *context, uint32_t domain, uint32_t permille);
	bool (*is_custom)(void *context);				/* lock-free read, may be NULL */
	int (*firmware_name)(void *context, char *name, size_t size);	/* may be NULL */
};
int kern_thermal_receiver_register(const char *name, const struct kern_thermal_receiver_ops *ops, void *context);

typedef int (*kern_thermal_ioctl_t)(struct file *file, unsigned long request, uintptr_t argument);
int kern_thermal_ioctl_range_register(unsigned first, unsigned last, kern_thermal_ioctl_t handler);

int kern_thermal_sensor_register(const char *name, uint32_t kind, uint32_t flags, struct kern_thermal_sensor **sensor);
void kern_thermal_sensor_update(struct kern_thermal_sensor *sensor, int32_t millicelsius, bool valid);
void kern_thermal_sensor_levels(struct kern_thermal_sensor *sensor, int32_t passive, int32_t critical);
int kern_fan_register(const char *name, uint32_t rpm_max, const struct kern_fan_ops *ops, void *context);
int kern_thermal_meter_register(const char *name, uint32_t kind, const struct kern_thermal_meter_ops *ops, void *context);
void kern_thermal_event(uint32_t type, uint32_t index, int64_t value);	/* thread context only */
void kern_thermal_resume(void);						/* from sleep.c after a system sleep */

/* kernel の CPU の数の口（driver が HAL を知らずに CPU ごとの表を作るため。HAL は変えない） */
unsigned kern_cpu_count(void);
```

- `kern_fan_ops`・sensor の値の規則・fan の写しは WS303 design §4.3・§5.1 のまま。STALLED は「実際の段 = max(自動, boost) > 0 で rpm 0 が 3 周期」（v5 m9）。
- 表は static（受け手 16、ioctl の範囲 4、sensor 32、fan 8、meter 8）で外さない。登録と振り分けの公開は release・acquire の順（v5 m20）。
- 受け手は apply の中で thermal の関数を呼ばない。例外は `kern_thermal_event`（事象の ring の葉の lock だけを取る、§4.12）。thermal は受け手を lock を持たずに呼ぶ。
- ioctl の範囲: core 1〜31、CPU（`cpuperf.c`）32〜63、64〜 は予備。登録の無い番号は ENOTTY。SET の root の判定は各 handler の中。
- 名前: UAPI の struct の接頭辞は `thermal_`、ただし `hw.thermal` の `thermal_header`・`thermal_entry`（p022 まで残る）とぶつけない（`sysctl.c` が両方の header を読む）。macro は `KERN_THERMAL_*`（v5 m7・review m10）。

### 4.3 CPU の idle（C-state）

#### 4.3.1 誰が状態を選ぶか（p002 との関係）

- **状態の選びは HAL の実装**（`hal_cpu_idle()`）。hal.h の契約は待ち方を名指さないので、MWAIT の C-state は契約の中（p002、hal.h は変えていない）。kernel は入った状態を知らず、名指しもしない（`hal_cpu_idle_suspend()` と同じ考え方、`hal.h:220〜221`）。
- この設計が p002 の形に求める条件と、main f740839e5 での照合（Q1 の照合、2026-10-31）:

| 条件 | 理由 | 状態 |
| --- | --- | --- |
| 1. C1 より深い状態は invariant TSC の CPU だけ | HAL の counter の契約（`hal.h:570〜582`） | 実装済み |
| 2. ARAT の無い CPU は HLT（C1） | LAPIC の tick が止まると CPU ごとの時間つきの sleep が遅れる（§2.1） | 実装済み |
| 3. `hal_cpu_notify()` で必ず抜ける（MWAIT の ECX bit 0） | `hal.h:158〜163` | 実装済み |
| 4. hypervisor の下（QEMU・KVM）は HLT | 仮想の CPU の MWAIT は exit・遅れの元 | P1 の追い commit（Q1 が依頼） |
| 5. 深さは出の時間と目安の滞在が tick の周期より十分短い物から。hint ごとの package の電力と PC の滞在の率で確かめる | 1 ms の tick と深い状態の出入り | p002 は C6 を既定（C10 との差は揺れの内）。PC の滞在の率は p014 で測る |
| 6. HLT に戻す build の口 | 前後を同じ器具で比べる | `IDLE_WAIT_DEEPEST_CSTATE=1`（P1 が comment を足す） |
| 7. `idle-suspend.c:9` の開発の管理情報の comment を消す | guardrail | P1 の追い commit（Q1 が依頼） |

- WS333 の kernel と driver は idle の状態を**書かない**。p012（tick の位相）だけが HAL の実装に触る（§4.3.3）。

#### 4.3.2 kernel と UAPI が持つ物（読みだけ）

- x86 の driver（`CONFIG_CPU_PERF=y`）が C-state の滞在の計数を読み UAPI に出す:
  - package の滞在（PC2〜PC10）: package の MSR（SDM Vol 4 の model ごとの MSR。番号と ADL が表にあるかは p007 で確かめる、未確認）を、package の任意の CPU で読む（5330 は package 1 つなので poll の thread の CPU で。複数の package の機械では notify の道で package ごと）。
  - core の滞在（CC6・CC7、同じく p007 で確かめる）: その CPU の上でしか読めないので GET_CPU_IDLE の ioctl の時だけ notify の道で読む（周期の読みで CPU を起こさない、WS301 M5）。Keiland の monitor（p022）は package の値だけを周期で読む。
  - CPU が持つ状態の一覧（CPUID.05H:EDX の sub-state の数、architectural）。HAL が使う状態は分からない。
- これは idle の電力の受け入れ（§8.4）と §4.3.3 の判断の数の源。

#### 4.3.3 OS が idle をもっと制御する道（4 段）

ユーザーの規則（HAL を変える前に、既存の HAL の実装・driver・kernel で足りない理由を示す）に従い、要る事を手段の段ごとに並べる。

| 段 | 事 | 手段と Phase | HAL の宣言 |
| --- | --- | --- | --- |
| 1. HAL の実装だけ | **tick の位相を揃える**: 全 CPU の LAPIC の tick を共通の TSC の時刻に揃え、全 core が同じ時刻に起き同じ時刻に休むようにする。候補は (a) 各 CPU が周期の timer を始める時刻を「TSC の時刻が周期の倍数の時」に合わせる、(b) TSC-deadline の mode（CPUID.01H:ECX[24]）で「前の期限 + 周期」を毎回書く（ずれない）。効果の量は未確認（package の深い状態の間が tick の 1 周期の大半になりうる、推定）。危険: 全 CPU が同じ時刻に tick の処理をするので lock の取り合い（`sched_clock_cpu` は CPU ごとの lock、CPU 0 は process の timer）。p012 で 5330 の電力と PC の滞在で確かめる | p012（承認不要: 契約は周期の tick と `HAL_TIMER_FREQUENCY` だけで、位相を決めていない、`hal.h:546〜556`） | 変えない |
| 1. HAL の実装だけ | idle の状態の選び | p002（済み） | 変えない |
| 2. driver | package の C-state の上限（MSR_PKG_CST_CONFIG_CONTROL 0xE2 の下位の field、model ごと、BIOS の lock の bit が立つと書けない）。latency の要る場面と gating の軸の受け手の候補 | p007 で SDM Vol 4 と 5330 の lock の bit を読みで確かめ、書けるなら p011 の provider の口に足す（使うかは Q333-2 の後） | 変えない |
| 3. kernel だけ | poll の idle（`hal_cpu_idle()` を呼ばず回る）。latency の最も厳しい場面（ゲームの mode）の候補。電力が増える | 要る時に WS306 などが使う（この WS では作らない） | 変えない |
| 4. まだ足りない事 | **idle の CPU の tick を止め、CPU 0 の tick も間引く**（one-shot の口、CPU 0 の仕事（`kernel_ticks`・process の timer）の受け渡し、CPU 時間の計数と乱数の種の扱い）。周期の tick しか無い HAL の契約の外で、kernel の timer の作りも変わる | 段 1〜3 の後の p014 の数を見てから。見積もりの器具: 試験の build で、CPU 0 以外の sleep の列が空の CPU の idle を `hal_cpu_idle_suspend()` に替え（hal.h を変えずに呼べる。製品の code にしない）、tick を止めた時の上限を測る（CPU 0 は残る）。要ると分かった時に、既存の口で代えられない理由と hal.h の差分の案を示して承認を求める（p023、WS332 の将来の拡張とどちらで扱うかも） | 要るかは未定 |

- 深い C-state の出の時間は 5330 で未確認（数を置かない）。

### 4.4 CPU の P-state（WS301 の設計を引き継ぐ）

WS301 design §4.1〜§4.12 を引き継ぐ。ただし **WS301 design.md:138（「`CONFIG_CPU_PERF=y` の kernel は起動の時に HWP を enable する」）と §4.8 の PROBED → HWP_ENABLED の遷移は、同じ文書の §4.4（起動では何も書かず HWP も enable しない、最初の要求で enable）で置き換える**（review m21）。変える点:

- **driver の置き場と規則**: `src/drivers/platform/pcat/cpu-x86/`（amd64 だけ。i386 の pcat は性能を考えない対象（master-design-policy §11.1）なので入れない）。driver は `hal/hal.h` を include しない。CPU の番号は `kern_cpu_id_t`、数は `kern_cpu_count()`（§4.2、p003 で kernel に足す）。CPUID・RDMSR・WRMSR は driver の中の 1 つの小さな file の inline asm に閉じる（Q301-8 の答えの形。2026-10-30 の規則と矛盾しないかは Q1 に確かめる、§11.2）。
- **provider の口**（WS301 §5.1 の型を kernel の型に直した）:

```c
/* include/kern/cpuperf.h（案） */
struct kern_cpuperf_provider_ops {
	int (*topology)(struct kern_cpuperf_topology *topology);	/* on the target CPU (CPUID is per logical CPU) */
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

- **notify の道**: `kernel_cpu_notify_handler()`（`clock.c:193〜`）の EOI の後・`sched_cpu_notify()` の前に `kern_cpuperf_notify(cpu)`（kernel の変更）。`hal_cpu_notify()` を呼ぶのは `cpuperf.c` だけ。`CONFIG_CPU_PERF=n` でも topology の CPUID の読みにこの道を使う（review m11）。
- **受け手**: `cpuperf.c` は §4.2 の受け手の口で「cpu」として登録する（p006）。apply の戻り: provider 無し・`CONFIG_CPU_PERF=n` → EOPNOTSUPP。1 秒の確かめの待ちが切れた → 0 と UNCONFIRMED（要求は箱に残り後で効く）。
- **熱の上限**: 受け手の `limit(context, domain, permille)`（下限は guaranteed、WS301 §4.6）。
- **strategy の写し**（方針。p006 の kernel の code、Opus。値は p014 で直す）: HWP は WS301 §4.4 の表（PERFORMANCE: min = その時の guaranteed・max = highest・EPP 0、BALANCED: lowest・highest・0x80、POWERSAVE: lowest・max(guaranteed, efficient)・0xC0、MANUAL: speed と noise の式、max は efficient より下にしない）。legacy は段の範囲と governor の閾値（WS301 §4.9）。
- **起動の時**: kernel は何も書かず HWP も enable しない。起動の後の strategy は rc が保存した物を渡す（Q333-1 の推し B）。
- **起動の parameter**（診断の switch、policy ではない。Q301-7 の答えの範囲）: `cpuperf=off`、`cpuperf.hwp=off`。`boot.h` の enum と `boot.c` の名前の表に p011 で足す（review m4）。
- **`CONFIG_CPU_PERF=n`**: MSR の道は compile しない。`cpuperf.c` は topology の表と CPU の GET を持ち、SET と strategy は EOPNOTSUPP。

### 4.5 P/E core の相対の性能と topology（WS302 が使う口）

- topology と core の種類は CPUID だけで分かり（CPUID は #GP を起こさず、hypervisor の下でも読める）、`CONFIG_CPU_PERF` に依らず amd64 の driver が出す。CPUID.1AH・1FH はその CPU の上で読むので notify の道を使う。
- 相対の性能（FREQUENCY は APERF/MPERF の測り、FEEDBACK は HFI、PROVIDER）は MSR を使うので `CONFIG_CPU_PERF=y` の時だけ。FREQUENCY の測り（Highest の要求で短く spin）は HWP を enable した後にしか出来ない（HWP の他の MSR は enable の前に触ると #GP）。基準の選び・既定 FREQUENCY・5330 の見込み P 100・E 75 は WS301 §4.7。
- WS302 が得る物（review M5）:

| 構成 | topology・core の種類 | 相対の性能 | HFI |
| --- | --- | --- | --- |
| `CONFIG_CPU_PERF=n` | 有り | UNKNOWN | 無し |
| y、最初の strategy の前（HWP 無効） | 有り | UNKNOWN | 無し |
| y、最初の strategy の後（Q333-1 の推し B なら起動の rc の後すぐ） | 有り | FREQUENCY（既定） | 有り（5330） |

- WS302 の口:

```c
int kern_cpuperf_cpu(kern_cpu_id_t cpu, struct kern_cpuperf_cpu *info);	/* topology, core type, relative, HFI, levels */
uint32_t kern_cpuperf_generation(void);	/* changes when relative, HFI or capabilities change */
```

  WS302 の DTB は周期の評価で世代を見て、変わった時に読み直す。UNKNOWN の間は core の種類だけで並べる（WS302 の設計の前提にする）。WS302 の ws.md の依存（WS301 の p001・p002・p004）は WS333 の p003（口）・p006（枠組み）・p011（x86）に替える（Q1、§11.2）。
- arm64・riscv64: provider 無しの既定の答え（CPU ごとに 1 core、種類 UNKNOWN、Q301-5）。

### 4.6 電力の読み（meter）（Q333-6）

- 電力の積算（energy、µJ）を読む口。`kern_thermal_meter_register(name, kind, ops, context)`、kind は PACKAGE・CORES・GRAPHICS・PLATFORM・DRAM。読みだけで電力の上限は書かない。
- x86 の driver（`CONFIG_CPU_PERF=y`）が RAPL の energy の MSR（単位と package・PP0・PP1・PSYS の候補は SDM Vol 4 の model ごとの MSR、allow list、p007 で確かめる）を meter にする。32 bit の計数の一周に備え、poll の thread が 60 秒ごとに読み 64 bit に積む。積みと GET_METER の読みは seq（書き手 1 つ）で守る。
- 用途: idle の電力の受け入れ（§8.4）、strategy の写しの調整、利用者の表示。i915 の診断の patch の電力の読みを置き換えうる（i915 の WS の判断）。

### 4.7 熱の sensor・fan・boost（WS303 の設計を引き継ぐ）

WS303 design §4.1（ACPI WMI）・§4.2（Dell）・§4.3（registry）・§4.5（fan と boost）・§4.9（Pi 5）を引き継ぐ。変える点:

- sensor の源（acpi-thermal、rpi5-fan）の push への変更と `hw.thermal` を写しから出すのは、両方とも core の Phase（p005）で行う（v5 M7）。`hw.thermal` の 1 source は 16 sensor までなので source を 2 つにして 32 を出す（v5 m7）。
- WMI の attach の試し直し（1 秒おきに 3 回）は poll の thread で行い、起動を延ばさない（v5 m21）。
- BIOS が boost を戻した事は周期の SMI でなく DDV の rpm で推し、SMM の get は SET の時だけ（v5 m10 の前半）。
- Dell の今の mode は `ODV0` を表示の補いにだけ使い、成否は SMBIOS の呼び出しの sentinel の書き換え、resume の後は覚えた値（v5 M8）。

### 4.8 strategy（統一の意味）

- 値: FIRMWARE（0、起動の時だけ。SET(FIRMWARE) は EINVAL）、PERFORMANCE、BALANCED、POWERSAVE、MANUAL。manual の時だけ 3 軸（向きは Q333-4）。manual でない SET で 3 軸が 0 でなければ EINVAL。
- 受け手ごとの写し（各 driver が決める）:

| 受け手 | 写す物 | 出典の節 |
| --- | --- | --- |
| cpu（HWP） | domain ごとの min・max・EPP | §4.4 |
| cpu（legacy） | 段の範囲と governor の閾値 | §4.4 |
| dell-wmi | BIOS の thermal mode（UltraPerformance・Optimized・Cool・Quiet） | WS303 §4.2、Q333-5 |
| rpi5-fan | trip の組（DT より遅くしない） | WS303 §4.9 |
| （今は無し） | gating の軸 | Q333-2（推しは保存と表示だけ。§4.3.3 の段 2 の上限が使えると分かれば cpu の受け手に足せる） |

- **SET の手順**（v5 M1）: (1) 引数の検査。(2) strategy の mutex の下で世代 g を進め写しを置き、mutex を離す。(3) strategy の thread に依頼し 5 秒まで待つ。(4) thread は受け手を登録の順に lock を持たずに呼ぶ。**1 周の後に最新の世代が今の周と違えば、全部の受け手にもう 1 周する**。受け手ごとの「適用済みの世代」を写しに置き GET_RECEIVER に出す。
- **ioctl の戻り**（review m9 で一意にした）: 受け手が 0 か、全部の受け手が EOPNOTSUPP → EOPNOTSUPP（strategy の値は記録する）。EOPNOTSUPP でない受け手が全部失敗 → 最初の errno。一部が失敗 → 0 と PARTIAL。自分の世代が上書きされた → 0 と SUPERSEDED。5 秒で終わらない → 0 と PENDING。EOPNOTSUPP と UNCONFIRMED は失敗に数えない。
- **速さの制限は受け手の中で**: Dell の 1 秒に 1 回の制限は EBUSY を返さず、受け手が次の窓まで眠ってから書く。
- **CUSTOM**: root の直の設定が効いている間、GET は strategy を変えずに CUSTOM の flag。次の SET で直の設定は消える（Q333-3）。
- **後から登録された受け手**: 今が FIRMWARE でなければ今の世代を渡す。
- **保存と起動の時**（Q333-1 の推し B、p013）: `/sbin/thermal strategy <値>` は成功の後に `/etc/thermal.conf`（1 行、strategy と manual の 3 軸）に書く。rc は起動の時に `/sbin/thermal restore` を走らせ、保存が無ければ balanced。Dell の受け手は、起動の時の `ODV0` が渡された mode と同じなら書かない（`ODV0` は SMI より前に AML が書く値で確かめにはならないが、ここでは書きを省くかの判断だけに使う。p018 で起動の時の `ODV0` が埋まるかを確かめ、埋まらなければ起動ごとに 1 回書く）。

### 4.9 passive（WS303 §4.7）

- CPU の sensor が passive の水準を超えたら、見張りが poll の thread に依頼し、poll の thread が `limit` を持つ受け手（今は cpu）に 5 秒ごとに 100‰ ずつ下げた上限を渡す（下限 guaranteed）。水準 − 3 °C で戻す。事象の hysteresis も 3 °C（v5 m4）。
- 5330・Pi 5 では動く場面が無い。受け入れは host 試験まで（Q303-14）。

### 4.10 UAPI `/dev/thermal`

- cdev `/dev/thermal`（0x00010008、node 0666）。試験の注入は `/dev/thermal-inject`（0600、`CONFIG_THERMAL_TEST_INJECT`、amd64・arm64）。ioctl の group 'T'。
- 権限: GET は誰でも。SET は root（sessiond 経由の strategy・boost は、root か wheel の利用者に代わって root の sessiond が呼ぶ）。EMERGENCY_ACK は pid 1 だけで、非常の状態が SIGNALED の時だけ受ける（他は EPERM・EINVAL、review M6）。
- ioctl の一覧（番号は p004 で確定）:

| 範囲 | ioctl | 中身 | 出典 |
| --- | --- | --- | --- |
| core | GET_STATUS | sensor・fan・meter・受け手の数、strategy の要約、非常の状態（固定の欄、off の印を含む） | WS303 §5.2 |
| core | GET_SENSOR・GET_FAN・GET_METER（index） | 1 つの sensor・fan・meter | WS303 §5.2、§4.6 |
| core | GET_STRATEGY・SET_STRATEGY | 値・3 軸・flags（CUSTOM・PARTIAL・PENDING・SUPERSEDED）・世代・firmware の mode の名前 | §4.8 |
| core | GET_RECEIVER（index） | 受け手の名前・最後の結果（errno・flags・適用済みの世代・時間） | §4.8 |
| core | SET_BOOST | fan の index（全部）と段 | WS303 §4.5 |
| core | SUBSCRIBE | 事象の種類の mask | WS303 §4.8 |
| core | EMERGENCY_ACK | init が非常の停止を始めた印（pid 1 だけ） | §4.11 |
| cpu | GET_CPU・GET_CPU_DOMAIN・SET_CPU_DOMAIN・GET_CPU_RELATIVE・SET_CPU_RELATIVE | WS301 §5.2 のまま（96・240・16・32 byte） | WS301 |
| cpu | GET_CPU_IDLE（cpu）・GET_PACKAGE_IDLE（package） | CPU が持つ C-state と滞在の時間（ns）、読んだ時刻 | §4.3.2 |
| inject | INJECT（`/dev/thermal-inject`） | 試験の sensor（同じ名前は同じ sensor）、試験の受け手、非常の道の選び（init に送らない道。boot の parameter でなく ioctl の flag） | WS303 §5.2 |

- 事象: `read`・`poll` で 128 byte の record（`struct system_event` と同じ大きさと頭の形、`/dev/thermal` の自分の種類の名前空間）。STRATEGY、BOOST、PASSIVE、CRITICAL、EMERGENCY、FAN_STALLED、CPU（能力・相対の性能・HFI・domain の変化）、OVERFLOW。open ごとに 64 の ring。
- 大きさは `_Static_assert` と `offsetof` で固め、ILP32 と LP64 で同じ。
- UAPI の全文の案は p004 でユーザーの review に出す。CPU の道（p006・p011）は kernel の口（p003）だけに依り、review を待たない（review M3）。

### 4.11 非常の停止（WS303 §4.6 を引き継ぎ、v5 M3〜M6 と review M6・m7・m8 を反映）

- **判定**（見張りの thread `thermal-guard`、1 秒ごと、写しの spinlock だけ、優先度は既定より高く）: ZONE の sensor の critical に時刻の違う 2 つの有効な値が続けて達したら非常。写しが 30 秒以上更新されない ZONE で最後の有効な値が critical 以上なら非常。
- **水準の検査**（v5 M6）: critical が 50 °C 未満、または起動の時の最初の値 + 5 °C 未満なら「無し」と扱い log 1 行。止める起動の parameter `thermal.emergency=off` は、ユーザーの決定（非常の水準で電源を落とす）を止める switch なので、入れるかを Q1 に確かめる（§11.2）。入れる時は、off の時に起動のたびに log と GET_STATUS の印を出す。
- **時計と S0 idle**（v5 M3、review m7）: 時間（30・10・120 秒）は kernel の tick で数え、S0 idle の後は眠った分だけ跳ぶ。`sleep.c` の resume の終わりに kernel の中の hook `kern_thermal_resume()` を足し（`sleep_post` は `/dev/system` の事象で kernel の callback ではない）、見張りは古さの基準を置き直し、新しい読みが来るまで古さの判定をしない。非常の開始と S0 idle の入口は 1 つの atomic の flag で排他: 非常の後は `KERN_SYSTEM_SLEEP` が EBUSY、sleep の途中（user を凍らせる前）に非常が始まったら sleep を中止させる。user を凍らせた後も device の suspend の間は kernel の thread が走りうる。その間の非常は SIGUSR2 が凍った init に届かないので、10 秒の ACK の門で直の切りになる。
- **停止の道は 1 つ**（review m8）: 「停止の道に入った」の flag を 1 つ置く。init の `KERN_SYSTEM_POWEROFF` が既に進んでいる時、見張りは直の切りの前にそれを 120 秒の門まで待つ。
- **手順**（v5 M4・M5）:
  1. 事象 EMERGENCY と GET_STATUS の固定の欄。以後 kernel は `KERN_SYSTEM_REBOOT`・`HALT` を POWEROFF として扱う（`system-device.c` の hook）。
  2. pid 1 の SIGUSR2 の disposition が handler なら、写しの spinlock を離してから `process_find_ref(1)` → `signal_send_process(SIGUSR2)`。handler でなければ 4。
  3. init: SIGUSR2 の要求は他の要求に上書きされない。通常の停止の途中でも猶予を 2 + 2 秒に縮め、終わりを POWEROFF にする。始めた時に `EMERGENCY_ACK`。kernel は SIGUSR2 から 10 秒で ACK が無ければ 4、120 秒で電源が落ちていなければ 4。
  4. 直の切り: sync は別の kernel の thread に頼み（`mount_sync_all()`）、見張りは 10 秒だけ待つ。`kern_platform_poweroff_emergency()`（pcat は `_PTS` を評価しない `drv_acpi_poweroff_emergency()`、rpi5 は poweroff、他の 10 platform は既定の実装 1 つで `kern_platform_poweroff()` と同じ）。失敗したら `kern_platform_halt()` と GET_STATUS の「切れなかった」。
- init が書く非常の記録の file（QEMU の試験の確かめ用）: `/var/log/thermal-emergency`（起動の世代と注入の時刻、製品の build でも書く。init がその時点で書けるかは p016 で確かめる）。
- 5330 は zone が無いので働かない（firmware と CPU の PROCHOT に任せる）。Pi 5 は DT の critical 110 °C。

### 4.12 資源・並行性・lock

```
 SET の ioctl:      strategy の mutex（短い、受け手を呼ばない） → 写しの spinlock
 strategy の thread: （lock なし） → 受け手の lock（cpuperf_mutex、Dell の mutex） → AML の interpreter の lock
 poll の thread:    （lock なし） → fan の provider の lock（AML の lock）／ meter の seq の書き／ cpu の limit（cpuperf_mutex）
 cpuperf:           cpuperf_mutex → CPU ごとの箱の spinlock（葉、irqsave）。mutex を離してから確かめを 1 秒まで待つ
 notify の handler: CPU ごとの箱の spinlock だけ。離してから sched_cpu_notify()
 legacy の governor: cpuperf_mutex → 箱の spinlock
 sensor の push:    源の lock（acpi-thermal の AML の lock、rpi5_fan.lock）を離してから → 写しの spinlock
 見張り:            写しの spinlock だけ。signal・process の lock は写しの spinlock を離してから
 事象の発行:        thread の文脈で、ring の spinlock（葉）だけ。他の lock を持っていてよい（ring の中で他の lock を取らない）
```

- 逆の道は無い: 受け手は thermal の lock を取らない、CUSTOM は `is_custom` を lock なしで読む。
- lock の rank（`include/kern/lock.h`、`LOCK_RANK_DEVICE` 125 など）は、同じ rank の入れ子の扱いを p005・p006 の着手の時に lock.h の規則で確かめて決める。
- thread（review m6、v5 m10 の後半）:
  - `thermal-work`: strategy の受け手の呼びと boost（SMI・CPU の 1 秒の確かめで眠りうる）。
  - `thermal-poll`: fan の 5 秒の読み、meter の 60 秒の積み、HFI の 1 秒の写し（WS301 §4.7）、passive の上限、WMI の attach の試し直し。DDV の読み（AML・EC）が止まると同じ thread の meter・HFI・passive も遅れる。meter の一周（数時間）と passive（5 秒の歩み）には余裕があり、遅れは log と事象で見せる。
  - `thermal-guard`: 見張り（provider を呼ばない）。
  - sync の助け: 非常の時だけ。legacy の governor: legacy の道の時だけ。
- 資源: 表は static で kernel の寿命。HFI の page は package ごとに 1 つ（`kern_pmem_alloc`）、止めの完了の前に解放しない。AML の object は呼んだ関数の中で解放。

### 4.13 割り込みと DMA

- 新しい割り込みは使わない（HWP・HFI・WMI の事象・thermal の LVT は使わない、周期の読み）。CPU 間は既存の notify。DMA は無い。HFI の表は hardware が書く記憶（WS301 §4.7）。
- SMI: Dell の mode の set と SMM の boost（`thermal-work`、受け手の中で 1 秒に 1 回まで）。

### 4.14 状態遷移

```
strategy:   FIRMWARE --SET(s)--> s（世代 g、受け手ごとの適用済みの世代、全部／PARTIAL／PENDING／SUPERSEDED）--SET(s')--> s'
            s --root の直の設定--> s + CUSTOM --SET--> CUSTOM は消える
CPU（x86）: ABSENT（MSR の道: hypervisor・CONFIG_CPU_PERF=n・どの bit も無い。topology は出す）
            PROBED --最初の要求--> RUNNING(HWP)（ここで PM_ENABLE）/ RUNNING(LEGACY)   （WS301 §4.8 の HWP_ENABLED を起動の時に置かない）
idle:       HAL の実装の中（kernel は状態を持たない）
非常:       NORMAL --critical に 2 回・古い値--> SIGNALED --pid 1 の ACK--> INIT --電源断--> (終)
            SIGNALED --10 秒で ACK 無し（凍った init を含む）--> CUTTING ; INIT --SIGUSR2 から 120 秒--> CUTTING
            NORMAL --init に handler 無し--> CUTTING
            CUTTING --sync を 10 秒まで待つ・poweroff--> (終) / --失敗--> HALTED（切れなかった）
            S0 idle の入口: NORMAL の時だけ。sleep の途中で非常 → sleep を中止
Dell の mode: UNPROBED --記述子・GUID・延長 0 の受け--> READY --apply--> SETTING --sentinel が書き換わった--> READY（覚えた mode）/ --失敗--> READY（不明）
fan の写し: UNKNOWN --read 成功--> OK --失敗--> STALE --成功--> OK
```

### 4.15 失敗と回復

WS301 §4.11 と WS303 §4.13 を引き継ぐ。足す・変える物:

| 失敗 | 振る舞い |
| --- | --- |
| 関門を通った MSR を CPU が拒む（#GP） | kernel が止まる（Q301-1 の代償）。既定 n、CPUID と allow list、`cpuperf=off` で避ける |
| RAPL・C-state の滞在の MSR が allow list の外 | その meter・統計を出さない（log 1 行） |
| tick の位相を揃えた build で lock の取り合い・遅れ | p012 の測りで分かる。HAL の実装の build の macro で今の形に戻せるようにする |
| 受け手が古い世代のまま | 次の周で最新を渡す |
| 非常の時に sync が返らない | 10 秒で進む |
| 非常の poweroff が失敗 | halt と GET_STATUS の印 |
| firmware の critical が低すぎる | 「無し」と扱う |

### 4.16 既存の framework との関係

- cdev・devfs: `/dev/thermal` を `cdev_register` で、node の mode は devfs の名前の表（`/dev/thermal-inject` は 0600）。
- ACPI: acpi-thermal は push に。WMI の層は ACPI の driver。`_OSC` は呼ばない（WS310）。
- sched: notify の handler に 1 行（§4.4）。sched の hint は WS302。
- sleep（WS052 の file）: `kern_thermal_resume()` の hook と非常との排他（§4.11）。S0 idle の後に CPU の要求が保たれるかは p014 で確かめ、保たれなければ resume の hook で書き直す。
- `/dev/system`: UAPI は変えない。`system-device.c` に 2 つの hook（非常の間 REBOOT・HALT を POWEROFF に、SLEEP の EBUSY）。file の所有は Q1（§11.2）。
- WS300: ASPM・APST は package の深い C-state の妨げの候補（§3）。gating の軸の受け手の候補。
- WS302: §4.5。WS332: tick の周期（§4.3.3 の段 4）。WS331: 新しい driver は最初から HAL を知らない。
- i915（WS291）: idle の電力の改善を GT の周波数で確かめる（p014 で WS291 の script の考え方を借りる）。display の PSR は package の深い状態の妨げの候補。
- **source の中の開発の管理情報**（review m13）: 手を入れる file に残る物は、その Phase の中で消す（log の文字列を変えるなら照合する道具も追従）: `src/kern/clock.c`（p006）、`src/drivers/generic/system-device.c`・`src/kern/sleep.c`（p016）、`src/drivers/acpi/acpi-thermal.c`（p005）、`monitor-zedbsd.c`（p022）、`src/hal/amd64/bsp-pcat/lapic.c`（p012、あれば）。
- docs: `docs/architecture/power-management.md` は最後の Phase（p024）。
- config: 新しい config の項目を `config/ci/` に足す時、`config/ci/config-amd64-server.mk` はユーザーの物なので変えない（review m23）。

## 5. インタフェースの要約

§4.2（thermal.h、kernel の CPU の数の口）・§4.4（cpuperf.h の provider）・§4.5（WS302 の口）・§4.10（UAPI）。完全な形は p003（kernel の 2 つの header、design-reviewer）と p004（UAPI、ユーザーの review）で header の案として書く。WS301 §5.2 の CPU の struct（96・240・16・32 byte）は形を変えずに入れる。

## 6. HAL の検討

### 6.1 使う HAL の口（全部が既存。使うのは kernel と HAL の実装だけ）

| 口 | 使う所 | 用途 |
| --- | --- | --- |
| `hal_cpu_idle()` | HAL の実装（p002 済み） | 待ち方を MWAIT に（実装の補完） |
| LAPIC の tick（宣言は無く、契約は周期と `HAL_TIMER_FREQUENCY`） | HAL の実装（p012） | 位相を揃える（実装の補完） |
| `hal_cpu_notify()`・`hal_cpu_count()`・`hal_cpu_current()` | `src/kern/cpuperf.c`・kernel の `kern_cpu_count()` | 相手の CPU で provider を呼ぶ、CPU の数 |
| `hal_rtc_read_counter()` | kernel の時計（driver は `kern_rtc_read_counter`） | 時間 |
| `hal_poweroff()` | kernel の platform（rpi5 の非常の切り） | 電源断 |
| `hal_cpu_idle_suspend()` | 試験の build だけ（§4.3.3 の段 4 の見積もり） | 製品の code にしない |

driver（`cpu-x86/`・`dell-wmi.c`・`acpi-wmi.c`・`rpi5-fan.c`）は `kern_*` だけを使う。

### 6.2 足りるか

- p003〜p022・p024 は **hal.h を変えずに作れる**（design-reviewer も source と照らして同じ結論）。足す物は kernel の口（`kern_cpu_count`、`kern_thermal_resume` の hook、`boot.h` の key、`kern_platform_poweroff_emergency`）で、HAL の宣言ではない。`proposed/` は作らない。
- idle の制御は §4.3.3 の段 1〜3（HAL の実装・driver・kernel）を先に試す。段 4（idle の CPU の tick を止め CPU 0 の tick を間引く）だけが HAL の契約の外の候補で、要るかは p014 の数で決まる。要ると分かった時に、段 1〜3 で足りない数と、`hal_cpu_idle_suspend()` の流用で代えられない理由（契約が系の sleep の用途、局所の割り込み源も静める、CPU 0 の仕事が残る）を示し、差分の案を作って承認を求める（p023）。
- MSR の #GP の安全網（WS301 の案 C）は Q301-1 の答えで作らない。

## 7. Phase の受け入れの物差し

- build: 全 platform の既定の build と、`CONFIG_CPU_PERF=y`・`CONFIG_THERMAL_TEST_INJECT=y` の build の warning 0（sparcv9 はユーザーの物なので Q1 に確かめてから）。
- 判定は console の log に依らない（ssh・QMP・道具の出力・時間）。

## 8. 引き継ぎと食い違いの整理

### 8.1 WS301 と WS303 の設計の食い違いと統合の答え

| 食い違い | WS301 | WS303 | 統合の答え |
| --- | --- | --- | --- |
| 上限の口の引数（v5 M2-2） | `(source, domain, permille)` | `(source, permille)` | 受け手の `limit(context, domain, permille)` |
| `kern_cpuperf_strategy` を直に呼ぶか（v5 M2-3） | 直に呼ぶ symbol に読めた | 登録だけ | 登録だけ |
| apply の結果（v5 M2-4） | 適用・一部・対応外 | int だけ | 0・EOPNOTSUPP・errno と UNCONFIRMED |
| ETIMEDOUT の扱い（v5 M1） | 要求は箱に残り後で効く | 失敗に数える | UNCONFIRMED、失敗に数えない |
| 受け手の登録の Phase（v5 M2-5） | WS301 p004 | WS301 p004 | p006 |
| lock の順（v5 M2-1） | 改訂 3 で直した | 受け手を lock なしで呼ぶ | §4.12 |
| 登録の口の数 | 4 つ | 同じ | 受け手の口と ioctl の範囲の 2 つ |
| apply の引数（v5 m2） | — | §4.4 は 3 つ、§5.1 は 2 つ | §4.2 |
| 起動の時の HWP（review m21） | :138 は enable する、§4.4 はしない | — | しない（§4.4） |
| i386 | build だけ | — | 入れない |
| topology と config | `CONFIG_CPU_PERF` の中（WS301 §10.8） | — | CPUID の部分は常に |
| 熱の上限の source の引数 | 1 つの値 | — | 外す |
| 電力の読み・CPU の idle | 無し | gating の軸だけ | meter（§4.6）、§4.3 |
| worker の thread | — | 1 本 | 3 本（§4.12） |

### 8.2 規則の変化

- 2026-10-30「driver は HAL を知らない」: WS301 の provider の口の `hal_cpu_id_t` を `kern_cpu_id_t` に、CPU の数は `kern_cpu_count()` に、`hal_cpu_notify()` は `cpuperf.c` だけに直した。
- 2026-10-30 の記録の規則（発言の原文を載せない）: この設計はユーザーの決定を要点で書く（review m14 の「原文を求める」は採らない）。

### 8.3 WS303 第 5 版の review（M1〜M9・m1〜m25）の行き先

| 指摘 | 行き先 |
| --- | --- |
| M1 世代の追い越し | §4.8 |
| M2 WS301 との契約 | §4.2・§8.1 |
| M3 S0 idle の時計 | §4.11 |
| M4 init の道の穴 | §4.11 |
| M5 sync と電源断の失敗 | §4.11 |
| M6 誤った非常の停止 | §4.11、p016 の受け入れ（zone を持つ amd64 の機械） |
| M7 Phase の依存 | §4.7、§9（push と注入は p005） |
| M8 Dell の mode の確かめ | §4.7・§4.14、p018 の試験の前に「Debian で set → 再起動 → 読む」で mode が残る事と profile の daemon が無い事を読みで確かめる |
| M9 SMM の boost の手順の出典 | §11.2 の Q1。手順は「書いた後に DDV の rpm と get で読み直す」と自分で組み、確かめられなければ boost は無し |
| m1 | §4.16 |
| m2 | §4.2 |
| m3 | §4.8 |
| m4 | §4.9 |
| m5 | p017・p018 の着手の時に WS303 §2.1 の「ECM1 の下」を「method 0x20 以外は」と読む（phase001 の phase.md の持ち越しに列べた） |
| m6 | §2.3 |
| m7 | §4.2・§4.7 |
| m8 | §4.11 |
| m9 | §4.2 |
| m10 | §4.7（前半）・§4.12（後半） |
| m11 | §4.10 |
| m12 | §4.10 |
| m13 | §4.11 |
| m14 | §4.11（`mount_sync_all`） |
| m15 | §4.11（10 platform、既定の実装 1 つ） |
| m16 | WS301・WS303 は canceled。この文書が本体 |
| m17 | §10 |
| m18 | p018 の phase.md で、5330 の熱の試験の合否の手順と記録だけの手順を分ける（持ち越し） |
| m19 | §4.10 |
| m20 | §4.2 |
| m21 | §4.7 |
| m22 | §4.10 |
| m23 | §4.10・§9（p004） |
| m24 | WS303 §2.3 の「ACPI の thermal zone」は Linux の thermal_zone（DPTF の participant）と読む（持ち越し） |
| m25 | §4.14 |

### 8.4 試験の計画

- **host 試験**（`plan/ws333/tests/`）: WS301 §6.1 と WS303 §6.1 の全部、加えて: 世代の 2 周目と SUPERSEDED、全部 EOPNOTSUPP の戻り、UNCONFIRMED、Dell の窓の眠り、`limit` の domain、meter の 32 bit の一周と seq、C-state の滞在の decode、ACK の道（pid 1 でない ACK・SIGNALED の前の ACK・10 秒・120 秒）、REBOOT との競合と停止の途中の SIGUSR2、init の POWEROFF が進行中の時の待ち、sync が返らない時の 10 秒、水準の検査、S0 idle の跳ぶ時計と `kern_thermal_resume` の置き直し、UAPI の layout（-m64・-m32）、strategy の保存と restore。x86 の decode の module（p008〜p010）は標準廉価実装手順の単体試験。
- **QEMU**（amd64 q35、ssh・QMP・時間で判定）: `/sbin/thermal` が sensor・fan 0・受け手の結果（cpu は「対応外」、hypervisor の関門）・topology を出す、`boot-test.sh` の回帰、riscv64 の virt でも `/sbin/thermal` が動く、非常の init の道と直の切りの道（ACK の道を含む）、sessiond（p019）。
- **5330**（1 本の script、Q1 の割当。p014）:
  1. idle の電力: 同じ image で kernel を替えて、HLT（`IDLE_WAIT_DEEPEST_CSTATE=1`）・MWAIT C6（既定）・C10・C6 ＋ tick の位相を揃えた build（p012）・それに strategy BALANCED、の 5 つ。desktop の session の idle で 60 秒ずつ、package の電力（meter）と package・core の C-state の滞在の率。同じ日に Linux の値（RAPL と cpuidle）。（可能なら）§4.3.3 の段 4 の見積もりの試験の build。
  2. i915: WS291 p022 の qs40 × 600 の fps と GT の周波数。
  3. HWP の 4 strategy と直の設定で 1 CPU の spin の MHz が要求に従う（WS301 §6.3 の 2）。E-core の 4 つ組の共有（同 3）。
  4. legacy の道（`cpuperf.hwp=off`、同 4）。相対の性能の基準（同 5）。S0 idle の回帰と S0 idle の後の要求（同 7）。
  - PASS: 1〜4 の全部の数が取れ、MWAIT の build の idle の package の電力が HLT の build より低く、3 の MHz が要求に従う。数の目標（Linux との差）は §11.2 で決めた値を足す。package の深い状態の滞在の率が低いままなら、妨げの候補（ASPM・PSR）を他の WS に渡す材料として記録する。
- **熱**（p018）: WS303 §6.3 の 5330 の 1〜4（v5 M8 の前の確かめ、合否と記録の分け）。**Pi 5**: WS303 §6.3。
- **zone を持つ amd64 の機械**（v5 M6、p016）: SV7 か 5320 に ACPI の thermal zone があるかを読み、あれば ZONE と critical を示し非常にならない事。無ければ「未実施」と記録し Q1 に返す。

## 9. Phase の分け方の案（ws.md の表の本体はこの表。p003 以降は問いの答えの後に Q1 が立てる。±50%、1 LW ≈ 実時間 15〜25 分）

CPU の省電力を先にする。CPU の道は p003 → p006 → p011 → p014 で、ユーザーの UAPI の review（p004）を待たない（p013 と p014 の strategy の選びだけが p004 の後。p004 の答えが遅れる時、p014 は試験の build の注入の node の試験の口で strategy を選ぶ）。x86 の decode の module（p007〜p010）と tick の位相（p012）は並べて進められる。

| Phase | 内容 | 依存 | LW | 受け入れ | 担当の型 |
| --- | --- | --- | --- | --- | --- |
| p001 | この設計、design-reviewer、問い | — | — | review の反映、問い | 設計（P5） |
| p002 | amd64 の idle を MWAIT に（HAL の実装） | — | 済み（main f740839e5、追い commit は P1） | p002 の phase.md | P1 |
| p003 | kernel の口の header の案（`include/kern/thermal.h`・`include/kern/cpuperf.h`、`kern_cpu_count`）を `plan/ws333/proposed/` に。design-reviewer | p001 | 1 | review の反映 | Opus |
| p004 | UAPI の全文の案（`include/uapi/thermal.h`）と layout の試験の案。**ユーザーの review** | p001 | 2 | ユーザーの答え、-m64・-m32 の layout | Opus |
| p005 | 熱の core: 写し・sensor の push（acpi-thermal と rpi5-fan）・fan・meter・strategy と受け手（世代の 2 周目）・boost・3 つの thread・事象・ioctl の振り分けと範囲の口・`hw.thermal` を写しから（2 source）。後半（p004 の後）: `/dev/thermal`・`/dev/thermal-inject` と試験の受け手・`/sbin/thermal` の熱の表示 | p003（後半は p004） | 8 | core の host 試験、全 platform の build、QEMU の回帰 | Opus |
| p006 | cpuperf の枠組み（kernel）: topology の表・domain・要求の箱と notify の道（clock.c）・provider の口・相対の性能と基準・受け手「cpu」・strategy の写しと MANUAL の式・直の設定と CUSTOM・上限・legacy の governor（方針と thread）・idle の統計・WS302 の口 | p003（結合の試験は p005 の後） | 6 | host 試験（模擬の provider）、build | Opus |
| p007 | x86 の decode の module の設計（標準廉価実装手順の設計の Phase）: 最初に SDM Vol 4 で ADL（family 6 model 0x9A）の 0xCE・RAPL・C-state の滞在・0xE2 の有無と、5330 の 0xE2 の lock の bit（Debian で読みだけ）を確かめ、無い MSR の module は立てない。header（仕様の comment）・.c の骨格・単体試験の道具・骨格の照合・p008〜p010 の phase.md。仕様の PDF の置き場所と SHA-256 を記録 | p001 | 2 | 骨格の build、design-reviewer、phase.md 3 本 | Opus |
| p008 | module: CPUID の解析（1FH/0BH・04H・1AH の topology、05H の C-state、06H の機能の bit、01H の hypervisor・EIST・TSC-deadline） | p007 | 1 | 単体試験 PASS、わざと壊す確かめ | Sonnet（phase-runner-low） |
| p009 | module: HWP の CAPABILITIES・REQUEST の field の decode・encode と範囲の検査、HFI の表の形の解析 | p007 | 2 | 同上 | Sonnet |
| p010 | module: legacy の段の源（ACPI の `_PSS` の package の形、0xCE の比、AMD の HwPstate）と RAPL の単位・energy の一周・C-state の滞在の計数（p007 で Vol 4 にあると確かめた物だけ） | p007 | 2 | 同上 | Sonnet |
| p011 | x86 の CPU の driver の統合: 関門（hypervisor・CPUID の bit・allow list・既定 n・`cpuperf=off`）、MSR の小さな file、probe、HWP の enable の順と既に 1 の道、Highest の MHz の測り、HFI の page、legacy の道、RAPL の meter・滞在の統計、provider の登録、起動の parameter の key | p006、p008〜p010 | 5 | host 試験（模擬の CPUID・MSR）、amd64 の build、QEMU で provider 無し | Opus（phase-runner-high） |
| p012 | tick の位相を揃える（amd64 の HAL の実装、承認不要）: 方式（周期の始めを揃える／TSC-deadline）の選び、今の形に戻す build の macro、5330 の前後の電力と PC の滞在（器具は p002 と同じ診断の patch か、p011 の後なら meter） | p002 | 3 | QEMU の起動、5330 の前後の数（下がらなくても記録して cleared、方式の採否は数で決める） | Opus（phase-runner-high） |
| p013 | CPU の ioctl・`/sbin/thermal` の CPU と電力の表示・strategy の保存と rc の restore（Q333-1 の推し B） | p004、p005、p006 | 3 | ioctl の host 試験、QEMU | Opus |
| p014 | 5330 の CPU の受け入れ（§8.4 の 5330 の 1〜4、1 本の script）。§4.3.3 の段 4 の判断の数 | p011、p012、p013（p004 が遅れる時は p005 の注入の口で） | 4 | §8.4 の PASS | Opus＋Q1 の実機の割当 |
| p015 | 既定の config の変更（Q333-7 の答えに従い `config/ci/config-amd64.mk` などの `CONFIG_CPU_PERF`、server の config は除く） | p014、Q333-7 | 1 | 全 platform の build、QEMU の回帰 | phase-runner |
| p016 | 見張りと非常の停止（signal の disposition の口、init の SIGUSR2・ACK・上書きの禁止、REBOOT・HALT の hook、SLEEP の排他と `kern_thermal_resume`、停止の道の flag、sync の助けの thread、`drv_acpi_poweroff_emergency`・platform の既定・halt、水準の検査、記録の file） | p005 | 6 | 非常の host 試験、QEMU の非常の 2 本、zone を持つ amd64 の機械の確かめ（無ければ未実施と記録） | Opus |
| p017 | ACPI WMI の層（`_WDG` の解析は Microsoft の文書に基づく 1 module にしてよい）と AML の harness の `plan/tools/aml-host/` への移し | p001 | 6 | WMI の host 試験、SMI を起こさない、amd64 の build | Opus |
| p018 | Dell の provider（DDV・mode・受け手・SMM の boost の確かめ）と 5330 の熱の試験 | p005、p017 | 7 | WS303 §6.3 の 5330 の 1〜4（v5 M8 の前の確かめ、合否と記録の分け） | Opus |
| p019 | sessiond の THERMAL の要求（strategy・boost、root か wheel） | p005 | 2 | rules の host 試験、QEMU（試験の受け手は p005 の注入の build） | phase-runner |
| p020 | Pi 5 の provider（fan の level・rpm、受け手、boost、lock の範囲） | p005、p016 | 3 | Pi 5 の試験 | phase-runner-high |
| p021 | passive（見張り → poll の thread → 受け手の `limit`） | p006、p016 | 2 | host 試験（模擬の上限の受け手） | phase-runner |
| p022 | sysctl の道具と Keiland の monitor を `/dev/thermal` に（monitor は package の値だけを周期で読む）、`hw.thermal` を外す（`HW_THERMAL 9` は欠番の comment） | p005、p013 | 2 | 2 つの読み手の build と表示、`hw.thermal` の grep が 0 | phase-runner |
| p023 | （条件つき）§4.3.3 の段 4 の設計（HAL の差分の案の要否を含む） | p014 | — | p014 の数で要ると分かった時だけ立てる | — |
| p024 | 規約の全文の見直しと文書（`docs/architecture/power-management.md`） | p005〜p022 | 2 | 規約、文書 | conformance-reviewer・document-writer |

合計 約 70 LW（p001・p002・p023 を除く。和: 1+2+8+6+2+1+2+2+5+3+3+4+1+6+6+7+2+3+2+2+2 = 70）。

- **標準廉価実装手順の適用**（review M8）: p007〜p010 は Intel SDM・AMD APM・ACPI の `_PSS` の形が決める decode・encode だけ（CPUID の leaf の解析、HWP・HFI の field と表の形、RAPL の単位と一周、`_PSS`・0xCE・HwPstate の段の源）。zedBSD の方針（strategy の写し・MANUAL の式・governor の閾値）と観測の値（5330 の表）は header の仕様に入れず、p006・p011（Opus）の code と試験の期待の値に置く。p017 の `_WDG` の解析も手順の条件を満たす。kernel の枠組み・driver の統合・実機の試験・非常の停止・tick の位相は普通の Phase。
- 仕様の PDF の置き場所は p007 で決める（WS301 の写しは scratchpad にあり消えうる。Q1 に確かめる）。
- Phase の ID は数字だけ（wsNNN-pNNN の形）。

## 10. 出典と引用

- WS301 §8・WS303 §8 を引き継ぐ: 値（MSR・CPUID の bit・表の形・ACPI の offset・GUID・EC の番地）は block ごとに「Value referenced:」「Values referenced:」、手順は「Procedure referenced:」、複数行の comment、file の先頭に書かない。
- Intel SDM・AMD APM・ACPI・Microsoft の WMI の文書は版を明記する。FreeBSD（BSD-2）は考え方だけ。
- Dell の SMM の値は Q303-12 の答えで GPL の file から値として出典つきで取る。guardrail の GPL の値の節は WS141・WS037・WS214・WS039 の物と、全部の WS に広げた 2026-10-15 の決定がある。WS333 への持ち越しの記録（ws.md と guardrail）は Q1（§11.2）。手順は GPL の file から取らない。失敗の印の値の扱いも Q1。
- 5330 の観測の値は試験の期待の値（`plan/ws333/tests/`）に置き、source の定数にしない。
- この設計のために GPL の source は読んでいない。web も使っていない。

## 11. 判断が要る点

### 11.1 ユーザー（[user-questions.md](user-questions.md) に背景・用語・案・推し）

| 問い | 要点 | 推し |
| --- | --- | --- |
| Q333-1 | 起動の時の strategy（旧 Q303-15 の聞き直し） | B: rc が保存した strategy を渡す（既定 balanced） |
| Q333-2 | gating の軸が何に効くか（旧 Q303-16） | C: 保存と表示だけ |
| Q333-3 | root の直の設定と strategy（旧 Q303-17） | A: 直の設定が優先し CUSTOM、次の strategy で消える |
| Q333-4 | 3 軸の向き（旧 Q303-18） | A: 100 = その物が多い |
| Q333-5 | Dell の Quiet を選んでよいか（旧 Q303-19） | A: manual で noise ≤ 33 の時だけ |
| Q333-6 | 電力の読み（RAPL）と C-state の滞在の統計を /dev/thermal に入れるか | A: 入れる（読みだけ） |
| Q333-7 | `CONFIG_CPU_PERF` の既定をいつ有効にするか | A: p014 の 5330 の受け入れが通ったら amd64 の既定を有効に |

- 知らせ（問いではない）: §4.3.3 の段 4（idle の CPU の tick を止める口、HAL の契約の外の候補）は、p012（tick の位相、HAL の実装）と p014 の数の後に、要ると分かった時だけ、数と理由と差分の案を付けて改めて聞く。

### 11.2 Q1

1. WS303 v5 M9: Dell の SMM の呼び出しの失敗の印（返りの register の値）を GPL の file から値として取ってよいか。Q303-12 の答えを WS333 に持ち越す記録（ws.md・guardrail の表）。
2. WS301・WS303 の design.md は WS333 が節を名指して引き継ぐので、WS333 の完了まで消さない。
3. `src/drivers/generic/system-device.c` と `src/kern/sleep.c`（WS052 の file）に hook を足す事の所有。
4. `monitor-zedbsd.c` の所有（p022、Keiland の WS との調整）。
5. p007 の仕様の PDF の置き場所（main の checkout の `build/sources/specs/` でよいか）。
6. `thermal.emergency=off`（非常の停止を止める起動の parameter）を入れてよいか（ユーザーの決定を止める switch。入れないなら水準の検査だけで守る）。
7. driver の中の MSR の inline asm（Q301-8 の答えの形）が、2026-10-30 の「driver は HAL を知らない」の規則と矛盾しないか（WS331 は port の命令を `kern_io_*` に寄せる方向）。
8. WS302 の ws.md の依存を WS333 の p003・p006・p011 に直す事（§4.5）。
9. p014 の数の目標（Linux の 3.2〜3.5 W との差をどれだけ埋めれば受け入れとするか）をユーザーと決めるか、記録だけにするか。
10. p012（HAL の実装の tick の位相）を WS333 の Phase として立ててよいか（HAL の実装の補完で承認は不要だが、p002 と同じく P1 の HAL の担当に回すかも）。

## 12. 未確認

1. 5330 の idle の電力の残り約 8 W の内訳（tick の位相・P-state・ASPM・PSR など）。p012・p014 で PC の滞在の率とともに測る。
2. ADL が SDM Vol 4 の 0xCE・RAPL・C-state の滞在・0xE2 の表にあるか、滞在の計数の rate、5330 の 0xE2 の lock（p007）。
3. 5330 の MWAIT の C-state の出の時間、tick の位相を揃えた時の lock の取り合い（p012）。
4. WS301 §10 の 1〜9（E-core の 4 つ組の共有、HFI の値、04H の値、KVM の HWP、SV7 の HWP、PM_ENABLE、legacy の PERF_CTL、など）。
5. WS303 §10 の 1〜5（Dell の mode の bit、延長 0、SMM の boost、DDDR）。
6. zone を持つ amd64 の機械（SV7・5320）があるか（p016）。
7. p002 の MWAIT の路を通った事は電力の差からの推定（p002 の phase.md）。p014 の C-state の滞在で確かめる。
8. init が非常の時に `/var/log` に書けるか（p016）。

## 13. 選ばなかった案（統合で新しく決めた物）

| 案 | 理由 |
| --- | --- |
| WS301・WS303 の 4 つの登録の口を残す | 受け手の口に上限と CUSTOM を入れれば足りる。食い違い（v5 M2）の元を減らす |
| `cpuperf.c` を `CONFIG_CPU_PERF=n` で外す | WS302 が topology を要る。CPUID は #GP を起こさないので常に入れて安全。MSR の道だけを config にする |
| idle の C-state を kernel が選ぶ（Linux の cpuidle の governor の形） | `hal_cpu_idle()` が引数を持たない。まず HAL の実装（p002・p012）と driver（0xE2）で足りるかを数で見る |
| idle の CPU の tick を止める口を今すぐ HAL に足す案 | CPU 0 の tick が残るので止めるだけでは package の深い状態に足りず、口の形（one-shot と CPU 0 の仕事の受け渡し）が大きい。HAL の実装だけで出来る位相の揃え（p012）を先に試す |
| `hal_cpu_idle_suspend()` を製品の普段の idle に使う | 契約は系の sleep の用途で、局所の割り込み源も静める。CPU ごとの時間つきの sleep が起きなくなる。試験の build の見積もりの器具にだけ使う |
| 起動の parameter で HWP を balanced にする近道 | WS301 Q301-7 の答え（自分の policy・起動の parameter を持たない）に反する。代わりに CPU の道を UAPI の review から外して短くした |
| RAPL の電力の上限（PL1・PL2）を書く | 到達目標の外で、firmware・Dell の mode と衝突しうる。読みだけ |
| x86 の driver を i386 にも | i386 は性能を考えない対象 |
| strategy の写し・governor を標準廉価実装手順の module に | SDM が決める物ではなく zedBSD の方針で、5330 の測りで直す（review M8） |
| worker の thread 1 本 | 眠る受け手が fan・meter・passive を遅らせる（review m6）。3 本に分けた |

## 14. design-reviewer の指摘と反映（[design-review.md](design-review.md)、第 1 版の review）

| 指摘 | 扱い | 行き先 |
| --- | --- | --- |
| M1 HAL の分析が網羅していない | 反映。4 段に書き直し、tick の位相（HAL の実装）を p012 に、0xE2（driver）を p007・p011 に、段 4 だけを条件つきの候補に。Q333-7（旧）の HAL の問いは外し、§11.1 の知らせに | §4.3.3・§6.2・§9・§13 |
| M2 p002 に求める条件 | 反映。条件 5〜7 を足し、Q1 経由で P1 に送った（2026-10-31）。Q1 の照合（main f740839e5）で 1〜3 と 6 は済み、4・7 は P1 の追い commit、5 は p014 で測る | §4.3.1 |
| M3 クリティカルパスが UAPI の review で止まる | 反映。p003（kernel の口）と p004（UAPI）に分け、CPU の道は p003 だけに依る | §4.10・§9 |
| M4 既定 n を y にする道が無い | 反映。Q333-7（新）と p015 | §11.1・§9 |
| M5 WS302 が得る物 | 反映。構成ごとの表、WS302 の依存の直しは Q1 | §4.5・§11.2 |
| M6 ACK の権限 | 反映。pid 1 だけ、SIGNALED の時だけ | §4.10・§4.11・§8.4 |
| M7 受け入れが落ちえない・測れない | 反映。p014 の PASS の定義、HLT の build（`IDLE_WAIT_DEEPEST_CSTATE=1`）と同じ器具で比べる、tick の数の代わりに位相の前後と段 4 の見積もり。数の目標は Q1 | §1・§8.4・§11.2 |
| M8 標準廉価実装手順の適用 | 反映。module を仕様の decode・encode に限り、方針は p006・p011 へ、Vol 4 を p007 の最初に確かめる、小さな module をまとめた（3 本） | §9・§13 |
| m1 型の場所 | 反映 | §2.2 |
| m2 halt と platform の数 | 反映（10、既定の実装 1 つ） | §2.2・§4.11 |
| m3 driver が要る kernel の口 | 反映（`kern_cpu_count`、`kern_rtc_read_counter`、`kern_pmem_alloc`） | §4.2・§4.4・§6.1 |
| m4 起動の parameter の key | 反映（p011・p016） | §4.4・§9 |
| m5 lock の抜け | 反映（事象の ring は葉、push の道、meter の seq） | §4.6・§4.12 |
| m6 worker の負荷 | 反映（3 本） | §4.12 |
| m7 resume の hook と凍った後 | 反映 | §4.11・§4.14 |
| m8 停止の道の重なり | 反映 | §4.11 |
| m9 全部 EOPNOTSUPP の戻り | 反映 | §4.8 |
| m10 名前のぶつかり | 反映 | §4.2 |
| m11 hypervisor の下の topology | 反映 | §4.4・§4.5・§4.14 |
| m12 package の MSR の読み手と monitor | 反映 | §4.3.2・§9（p022） |
| m13 source の中の開発の管理情報 | 反映（各 Phase の範囲） | §4.16 |
| m14 原文の記録 | 採らない: 2026-10-30 の規則で発言の原文は記録に載せない（AGENTS.md §8）。ws.md の Phase の表は p003 以降を Q1 が立てるまで写しが無い（§9 の題に書いた） | §8.2・§9 |
| m15 保存と rc の Phase、起動の時の Dell の読み | 反映（p013、`ODV0` は書きを省く判断だけ） | §4.8・§9 |
| m16 user-questions | 反映（旧 Q333-7 を外した、用語を足した、「後で直せる」を Q333-4 で直した） | user-questions.md |
| m17 Phase の大きさと ID | 反映（p005 8、p006 6、和を数えた、ID は数字だけ） | §9 |
| m18 持ち越しの記録 | 反映（phase001 の phase.md に列べた） | phase001/phase.md |
| m19 GPL の値の持ち越し | Q1 に | §10・§11.2 |
| m20 `thermal.emergency=off` | Q1 に、off の時の log と印 | §4.11・§11.2 |
| m21 WS301 の自己矛盾 | 反映（置き換えを明記） | §4.4・§8.1 |
| m22 MSR の inline asm と規則 | Q1 に | §4.4・§11.2 |
| m23 server の config | 反映 | §4.16 |
| m24 根拠の無い数 | 反映（数を消した） | §4.3.3・user-questions.md |
