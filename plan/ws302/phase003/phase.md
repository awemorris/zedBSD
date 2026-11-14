<!-- awesome-plan project=zedbsd record=ws302-p003 -->

# ws302-p003: sched の口（hint・段・種類の表・行き先・手渡し・飢えの防ぎ）

Status: in-progress（実装済み・試験 PASS。mk の追加の案はユーザーの承認待ち。cleared は Q1）
Parent: [WS302](../ws.md)
Queue: q2099-i01（P6）

## 目的

DTB の本体（p004）が使う sched の口を作り、DTB の本体が無くても試験の thread で全部の道を通して壊れない事を示す。DTB は sched ではないので、sched は hint を持って自分の判断の所（置き場所・盗み・idle の CPU の起こし・頼まれた移動・走っている thread の手渡し）で使うだけにし、果たせない頼みは断る。

本体は [design.md](../design.md) 第 3 版の §4.5（口）・§4.6（sched の変更）・§4.7（段と飢えの防ぎ）・§4.11（試験の config）・§6.1・§6.2 の 3、§7 の p003 の行。

## 作る物（全部 `#if CONFIG_KERN_DESKTOP_BALANCER`。n の kernel は今と同じ）

1. **段**: `sched_hint_set()`（hint と base の段を置く。RUN・WOKEN の queue に居れば新しい段に入れ直し、SLEEP は field だけ、走っていて下がれば preempt）。queue に入れる全部の所で `queue_level_locked()` が段を base に戻し、user の thread が kernel の中に居れば `SCHED_HINT_USER_DEFAULT`（10）より下げない（kernel の中の優先）。
2. **CPU の種類の表**: `sched_cpu_classes_set()`（CPU ごとに 1 語の atomic、FAST と EFFICIENT がそろう時だけ種類の好みが効く）。
3. **行き先の選び**: 純な関数 `sched_hint_choose()`（MOVE・MOVE_IDLE_CORE・PLACE）と段の純な関数（`sched_hint_queue_level()`・`sched_hint_starve_level()`）。新しい source `src/kern/sched-hint.c`（host 試験の対象）。
4. **置き場所・盗み・tick の助け**: 新しい user の thread は作り手の hint と base の段を継ぎ（作り手の CPU の lock の下で読む）、作り手の CPU に backlog があれば PLACE の順で idle の CPU を選ぶ。盗みは同じ段の中で盗む CPU の種類を望む thread を先に、NO_OVERFLOW の thread は違う種類へは飢えの上限まで渡さない。tick の助けは待ちの先頭の thread の種類の idle の CPU を先に起こす。起床の道は変えない。
5. **`sched_hint_place()`**: 走っていない thread は hint の移動の道（元の CPU の lock の中で状態を確かめ直す。`sched_set_cpu` は変えない、D-5・D-6）、走っている thread は `move_pending` を立てて CPU に通知。
6. **手渡し**: CPU の通知か tick で `move_pending` の current を席（`handoff`、1 つ、上書きしない）に置いて切り替え、後継（切り替えの後の 4 か所と自分の再開の後）か次の tick の頭が transfer して行き先に入れる。断られれば 1 回だけ次の tick で試し直し、駄目なら元の CPU に戻す。BOUND は手渡さない。
7. **飢えの防ぎ**: tick ごとに今の段より下の queue の先頭の user の thread が 100 ms 待てば、`max(今の段, 8)` の woken に 1 回だけ上げる。
8. **計数**: `sched_hint_counters()`（CPU ごとに足し、読む時に合わせる）。
9. **試験の config `CONFIG_KERN_DESKTOP_BALANCER_TEST`**: `src/kern/sched-hint-test.c`。起動の parameter `desktop_balancer.classes=PPEE`（偽の種類の表）と `desktop_balancer.stress=1`（10 ms ごとに無作為の user の thread に無作為の hint と `sched_hint_place`）。結果は global の報告の struct に書き、QEMU の gdbstub で読む（console・serial の log は読まない）。

## 試験

- host 試験（`plan/ws302/tests/balancer-host/`）: `sched_hint_choose` を 5330 の形（P 論理 4・E 8）・PPEE・全部 P・全部 ANY・usable 0 の P で、idle と backlog の bit の組を回して、MOVE が違う種類を返さない・MOVE_IDLE_CORE が兄弟の忙しい CPU を返さない・PLACE の順を独立の参照の実装と照らす。段の関数（kernel の中で 10 より下げない、上げが 8 より上にならない、上げの条件）。突然変異の確かめ。
- build: x86_64・rpi5・vf2 の n と y（と x86_64 の y＋thermal、y＋試験）が warning 0。n の kernel は基点 991f37fe79 と同じ（`tests/dis-compare.py`）。
- QEMU（x86_64、SMP 4、KVM）: y の image の起動（boot-test.sh）と、y＋試験の image で `desktop_balancer.classes=PPEE desktop_balancer.stress=1` の 30 分の stress（kstress の iostress・credrace・sigexec、thread の作成と exit・fork の嵐、時間つきの sleep が早く起きない事、syscall の無い忙しい loop）。止まらない・panic しない（serial の対話で guest が答える）、手渡しと移動の数 > 0、手渡しの失敗の数を記録（gdbstub で報告の struct を読む）。

## 受け入れ

- 上の host 試験 PASS、build warning 0、n の kernel が基点と同じ、QEMU の y の起動 PASS、30 分の stress PASS（guest が答え、手渡し > 0・移動 > 0、時間つきの sleep の早起き 0）。
- mk の追加の案（下）はユーザーの承認まで当てない。当たるまで y は overlay（[../tests/desktop-balancer-overlay.mk](../tests/desktop-balancer-overlay.mk)）で build する。

## mk の追加の案（承認まで当てない）

p003 で足した source 2 つの build の規則。当たるまで y は [../tests/desktop-balancer-overlay.mk](../tests/desktop-balancer-overlay.mk) で build する（`make -f Makefile -f plan/ws302/tests/desktop-balancer-overlay.mk CONFIG_KERN_DESKTOP_BALANCER=y [CONFIG_KERN_DESKTOP_BALANCER_TEST=y] …`）。overlay なしの y は `sched_hint_choose` などが無く link できない。

`mk/kernel.mk` の「[Desktop Thread Balancer]」の block（p002 の案で当たった物）に 1 行:

```make
# [Desktop Thread Balancer]
ifneq ($(filter y,$(CONFIG_KERN_DESKTOP_BALANCER)),)
KERN_CPPFLAGS += -DCONFIG_KERN_DESKTOP_BALANCER=1
KERN_SOURCES += \
	src/kern/desktop-balancer.c \
	src/kern/sched-hint.c
endif
```

`mk/test-config.mk`（「[Kernel Test: Thermal Injection]」の後）:

```make
# [Kernel Test: Desktop Thread Balancer Stress] (desktop_balancer.classes=, desktop_balancer.stress=1)
#  - Depends: CONFIG_KERN_DESKTOP_BALANCER
CONFIG_KERN_DESKTOP_BALANCER_TEST ?= n
```

`mk/test-kernel.mk`（最後）:

```make
# [Kernel Test] desktop thread balancer stress (needs the balancer)
ifeq ($(CONFIG_KERN_DESKTOP_BALANCER)$(CONFIG_KERN_DESKTOP_BALANCER_TEST),yy)
KERN_CPPFLAGS += -DCONFIG_KERN_DESKTOP_BALANCER_TEST=1
KERN_SOURCES += \
	src/kern/sched-hint-test.c
endif
```

- `sched-hint.c` は sched の口の純な関数（行き先の選びと段）で、host 試験のために sched.c から分けた。balancer が y の時だけ要る。
- 試験の config は balancer が y の時だけ効く（source も `#if CONFIG_KERN_DESKTOP_BALANCER && CONFIG_KERN_DESKTOP_BALANCER_TEST`）。

## 進み

2026-11-14〜15 P6（q2099-i01、基点 main 991f37fe79。途中で main 7f7f0cc752・944a8c0cf3 を取り込んだ）。

- **改名**（2026-11-14 ユーザーの決定、design.md の D-13）: `CONFIG_DTB` → `CONFIG_KERN_DESKTOP_BALANCER`、`include/kern/dtb.h`・`src/kern/dtb.c` → `desktop-balancer.h`・`desktop-balancer.c`、`kern_dtb_*` → `kern_desktop_balancer_*` ほか（commit bc8e51f9e3、Q1 の OK の後の git mv・git rm）。`plan/ws302/proposed/include/kern/` の古い 2 つの header は消し、design.md・design-review.md・phase001 の link を本物に直した。p002 の mk の案は新しい名前で main に当たった（04fabc3c96）。
- `include/kern/sched-hint.h`: 段の範囲（`SCHED_HINT_LEVEL_HIGHEST` 8・`_LOWEST` 15）、`SCHED_HINT_USER_DEFAULT` 10、`SCHED_HINT_STARVE_MS` 100、`SCHED_HINT_CHOOSE_*`、関数 `sched_cpu_classes_set`・`sched_hint_set`・`sched_hint_place`・`sched_hint_counters`・`sched_hint_choose`・`sched_hint_queue_level`・`sched_hint_starve_level`、試験の `sched_hint_test_start`（n では宣言も無い）。
- `src/kern/sched-hint.c`（新、balancer が y の時だけ）: 行き先の選び（MOVE・MOVE_IDLE_CORE・PLACE、兄弟が忙しい CPU は `sibling_rank` で比べる）と段の 2 つの純な関数。
- `src/kern/sched.c`（全部 `#if CONFIG_KERN_DESKTOP_BALANCER`）:
  - CPU ごとに手渡しの席（`handoff`・`handoff_target`・`handoff_retried`）と計数 8 つ。種類の表は 2 枚を交互に埋めて番号を release で公開する（読み手は lock 無し）。
  - `queue_level_locked()`: `sched_add`・`sched_yield`・`sched_preempt`・`wake_enqueue_locked`・盗み（成功と失敗）・`sched_set_cpu`（2 か所）・`sched_hint_set`・hint の移動・手渡しの全部の queue に入れる所で、段を base に戻し、kernel の中なら 10 より下げない（`kernel_held` に数える）。上げ（`raised`）と手渡しの印（`move_pending`）もここで消える。
  - `sched_prepare_thread`: 新しい user の thread は作り手の user の thread の hint と base を、作り手の CPU の lock の下で継ぐ（kernel の thread は継がない、review 第 3 版 M3）。作り手の CPU に backlog があれば PLACE の順で idle の CPU を選ぶ。
  - tick: 頭で `complete_handoff`、待ちの先頭の user の thread の飢えの防ぎ（1 tick に 1 本）、backlog の助けは待ちの先頭の thread の種類の idle の CPU を先に、手渡しの印があれば `hint_handoff`。`sched_cpu_notify` も印を見る。
  - 盗み: 種類の表がある時だけ `hint_take_stealable_locked`（同じ段の中で盗む CPU の種類を望む thread を先に、NO_OVERFLOW の thread は飢えの上限まで違う種類に渡さない）。表が無ければ今の `take_stealable_locked`。
  - `hint_move`: `sched_set_cpu` と同じ順で、元の CPU の lock の中で状態を確かめ直す（RUNNABLE は RUN・WOKEN、SLEEPING は時間つき SLEEP と時間の無い NONE、FREE、`need_migrate == 0`）。`sched_set_cpu` そのものは変えない（D-5）。
  - 手渡し `hint_handoff`・`complete_handoff`: 席は 1 つで上書きしない、後継（`sched_yield`・`sched_preempt`・`switch_without_enqueue`・`sched_exit_current` と手渡し自身の再開の後）か tick の頭が transfer、断られたら 1 回だけ次の tick で、2 回目は元の CPU に戻す。BOUND は手渡さない。
- `src/kern/sched-hint-test.c`（新、試験の config の時だけ）と `src/kern/entry.c` の 1 か所（thermal の block の後で `sched_hint_test_start()`）。
- design.md の §4.6.7 の確かめ直しの条件を実装に合わせた（時間の無い眠りの thread も移せる）。
- 試験の道具: [../tests/balancer-host/](../tests/balancer-host/)（host 試験と突然変異）、[../tests/p003-build.sh](../tests/p003-build.sh)、[../tests/p003-boot.sh](../tests/p003-boot.sh)、[../tests/p003-qemu.sh](../tests/p003-qemu.sh)（30 分の stress）、[../tests/hintstress.c](../tests/hintstress.c)（guest の program）、[../tests/config-x86_64-balancer-stress.mk](../tests/config-x86_64-balancer-stress.mk)。

## 結果

2026-11-15 P6。source の先頭 623adc8f4e（実装 d1fcd86453、書き方の直し f4389e8112、main 944a8c0cf3 の取り込み a8b16243ff）。toolchain は `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`、3 機種の `sysroot` を先に作った。make は -j4。

### host 試験（`plan/ws302/tests/balancer-host/run.sh OUT`、host の cc、ASan・UBSan）

```
choose: 1820736 checks, PASS
levels: PASS
hint-test: PASS
mutant ties to the highest number: caught
mutant unusable CPUs chosen: caught
mutant busy CPUs chosen: caught
mutant whole core not required: caught
mutant sibling speed ignored: caught
mutant efficient placed elsewhere: caught
mutant kernel hold off: caught
mutant raise above the default: caught
mutant raise before the limit: caught
mutant raise that lifts nothing: caught
balancer-host: PASS
```

- 選び: 5330 の形（P 論理 4・E 8）・PPEE・PpEE・全部 P・全部 ANY・usable 0 の P を含む形 7 つで、MOVE・MOVE_IDLE_CORE・PLACE × 3 種類 × idle と backlog の組（6 CPU 以下は全部、12 CPU は無作為 20 万）を、別に書いた参照の実装と照らした。どの答えも free で usable、MOVE は同じ種類、MOVE_IDLE_CORE は兄弟も free、EFFICIENT の PLACE は E だけ。
- 段: kernel の中の 10 より下げない、上げは 8 より上にならない、待ちの上限の端（99・100・101 tick）。
- 突然変異 10 個を全部検出（最初の版では 2 つが見つからず、`choose_fastest` の重なった条件を 1 つにまとめた）。

### build（`plan/ws302/tests/p003-build.sh OUT build/p003/ref x86_64 rpi5 vf2`）

REF は基点 991f37fe79 の n の kernel（同じ worktree で src・include を基点に戻して作った物）。

```
x86_64 n rc=0 warnings=0
x86_64 n-vs-base same-text-but-line-numbers 29
x86_64 y rc=0 warnings=0
x86_64 y-thermal rc=0 warnings=0
x86_64 y-test rc=0 warnings=0
rpi5 n rc=0 warnings=0
rpi5 n-vs-base same-text-but-line-numbers 29
rpi5 y rc=0 warnings=0
vf2 n rc=0 warnings=0
vf2 n-vs-base same-text-but-line-numbers 29
vf2 y rc=0 warnings=0
p003-build: PASS
```

- n の kernel は基点と同じ: 逆 assemble の行・番地・命令が同じで、違いは足した行より後の `hal_fatal` の行の番号の即値 29 個だけ（dis-compare.py が各々を基点と今の file の同じ fatal の行と確かめた）。
- y は overlay（`sched-hint.c` を足す）で build。y-thermal は `CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y`（5330 の評価の構成）、y-test は加えて `CONFIG_KERN_DESKTOP_BALANCER_TEST=y`。

### QEMU の y の起動（`plan/ws302/tests/p003-boot.sh OUT y`、x86_64、boot-test.sh の既定 uefi-nvme・KVM・SMP 4）

```
y image rc=0 warnings=0
y boot rc=0
p003-boot: PASS
```

- text の login の image（`plan/tools/boot-images/config-amd64-boot.mk`）。画面の login prompt を読み、emulate した keyboard で root で login して `uname -s` の答え `zedBSD` を画面で確かめた（d1fcd86453 の kernel）。

### QEMU の 30 分の stress（`plan/ws302/tests/p003-qemu.sh OUT 1800`、x86_64、KVM、SMP 4、2 GiB、名 p6-ws302、gdbstub 127.0.0.1:22306）

image は [../tests/config-x86_64-balancer-stress.mk](../tests/config-x86_64-balancer-stress.mk)（base の userland、console を serial にも、balancer と試験の config、ISA の NE2000 は外す）。起動の行に `desktop_balancer.classes=PPEE desktop_balancer.stress=1`。2026-11-15 00:31〜01:05、kernel は 623adc8f4e の tree。

```
image rc=0 warnings=0
kstress rounds=17 iostress-ok=17 credrace-ok=17 sigexec-ok=17
hintstress busy threads=3 result=PASS
hintstress threads rounds=196117 failures=0 result=PASS
hintstress sleep sleeps=214103 early=0 worst_early_us=0 result=PASS
guest alive=1
report magic=0x48494e5453545253 passes=3028..191994 threads=767792 set[ok,ealready,eagain,ebusy,esrch,einval,other]=699891,0,0,0,55798,12103,0, place=5526,118358,112823,12,56720,474353,0,
counters hints=693990 moves=5369 refused=112851 handoffs=93 handoff_retries=0 handoff_failures=0 starved=28453 kernel_held=23024627
p003-qemu: PASS
```

- guest は止まらず panic せず、最後の `echo alive` に答えた（serial の対話）。kstress（iostress の direct 読み・fork・munmap、credrace、sigexec）は 17 周全部 rc=0。thread の作成と exit・fork の嵐 19 万周で失敗 0。時間つきの sleep 21 万回で早く起きた物 0（hint の段の付け替えが sleep の queue を乱さない、review M2）。
- 報告（gdbstub で始めと終わりに読んだ）: stress の thread の周 3028 → 191994（30 分回り続けた）、hint を出した thread 76 万。sched の計数: hint の変更 69 万、走っていない thread の移動 5369、走っている thread の手渡し 93（試し直し 0・失敗 0）、種類の idle の CPU が無く断った 11 万、飢えの防ぎの上げ 2.8 万、kernel の中で 10 に留めた入れ 2302 万。
- `sched_hint_set` の EINVAL 12103 は試験が 64 回に 1 回わざと出す範囲の外の段（7）。`sched_hint_place` の EINVAL は PLACE でない・ANY の hint、ESRCH は exit した・NEW の thread、EALREADY は既にその種類の CPU に居る thread。
- 判定は serial の対話の command の答えと gdbstub の報告だけ（console・serial の boot の log では判定していない）。
- 120 秒の試し走り（d1fcd86453）でも同じ項目が通った（報告の読み取りの 1 語のずれを直す前の script。直した値で移動 3653・手渡し 16）。

### 未実施

- arm64・riscv64 の QEMU の起動（2026-11-14 ユーザーの決定で範囲の外。build は確かめた）。
- 5330 の実機（p006）。偽の種類の表の上でだけ確かめた。SMT の兄弟（`p`）を含む偽の表の QEMU の試験はしていない（host 試験では PpEE・5330 の形で確かめた）。
- `CONFIG_KERN_DESKTOP_BALANCER_TEST` の試験は x86_64 だけ。
- p004 以前なので、DTB の本体が hint を出す形の試験は無い（stress の thread の無作為の hint だけ）。

### 受け入れの判定（担当の推し）

host 試験 PASS、3 機種の n・y の build warning 0、n の kernel は基点と同じ、x86_64 の QEMU の y の起動 PASS、30 分の stress PASS（guest が答え、移動 5369・手渡し 93、時間つきの sleep の早起き 0）。推し: 「実装済み・試験 PASS」。mk の追加の案（上）がユーザーに承認されて当たった後に、overlay なしの 3 機種の y の build を 1 回流して cleared（p002 と同じ扱い）。
