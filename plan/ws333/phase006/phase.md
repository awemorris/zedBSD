<!-- awesome-plan project=zedbsd record=ws333-p006 -->

# ws333-p006: cpuperf の枠組み（kernel）

Status: 実装済み・試験 PASS（host 試験・全 port の build・QEMU の起動の回帰。cleared は Q1 の判定の後）
Parent: [WS333](../ws.md)
Queue: q1754-i01（P7）、起動の回帰は q1759-i01（P7）

## 目的

design.md §9 の p006: [p003](../phase003/phase.md) の header（`include/kern/cpuperf.h`）の口を `src/kern/cpuperf.c` に実装する（topology の表・domain・要求の箱と notify の道・provider の口・相対の性能と基準・受け手「cpu」・strategy の写しと MANUAL の式・直の設定と CUSTOM・上限・legacy の governor・idle の統計・WS302 の口）。受け入れは host 試験（模擬の provider）と全 platform の vmunix の build（warning 0）。UAPI（p004）は作らない。

## 判断（Q1）

- 2026-10-31 ユーザーから、不在の間は実装できる WS を実装だけでも進め、試験できるなら試験もする、という趣旨の指示を受けた。Q333-1〜7 は答え待ちで、推しで進めた（Q333-3 の推し A: 直の設定が優先し CUSTOM、次の strategy で消える。Q333-4 の推し A: 3 軸は 100 = その物が多い）。

## 実装

- `src/kern/cpuperf.c`（新規）:
  - CPU ごとの箱（intent・work・posted/completed・最後の要求と sample・topology・levels）と、domain の表（mutex の下）。箱は spinlock（葉、irqsave）で、notify の handler も取る。`pending` の atomic で、何も無い notify（scheduler の物）は lock を取らずに戻る。
  - 道: thread は箱に work と intent を置き `hal_cpu_notify()`（自分の CPU なら割り込みを止めてその場で）→ handler が `kern_cpuperf_notify()` で処理し waiter を起こす。待ちは 1 秒まで、mutex を持たない（feedback の写しだけは mutex の下で待つ、上限つき）。
  - 適用（review B1 の解）: notify の中で、最初の要求の前に provider の `start`（能力を返す）→ `sample`（能力の読み直し）→ intent から request を計算 → `apply`。anchor が分からなければ書かず EAGAIN。
  - strategy の写し（RANGE、WS301 §4.4 の表）: PERFORMANCE min=その時の guaranteed・max=highest・EPP 0、BALANCED lowest・highest・0x80、POWERSAVE lowest・max(guaranteed, efficient)・0xC0、MANUAL max=min(efficient→highest を speed で, max(guaranteed, efficient)→highest を noise で)・EPP 255×(100−speed)/100。STEPS（legacy、WS301 §4.9）: PERFORMANCE 最上段固定、BALANCED 全段を governor、POWERSAVE guaranteed の段まで、MANUAL は noise ≥ 50 で turbo の段を許し speed で上限の段。
  - 直の設定（root の判定は呼び手）: domain ごとの単位、全 domain は permille（CPU の上で anchor に対して単位に直す）。CUSTOM の atomic、次の strategy で消す。
  - 熱の上限: 受け手の `limit(domain, permille)`、guaranteed より下げない、待たない。
  - 相対の性能: FREQUENCY は framework が測る（最初に確かめられた strategy の後、core の種類ごとに 1 CPU、domain を highest にし、その CPU に `sched_set_cpu` で束ねた thread で provider の `measure` の間 50 ms 回す）。FEEDBACK・PROVIDER は provider の `relative`。どれかの CPU が測れなければ全部 UNKNOWN、最速を 100 に四捨五入。
  - HFI の写し: 受け手の `poll`（熱の core の poll の thread が毎秒）→ package の CPU（呼んだ CPU が属すればその場）で `feedback` → mutex の下で `feedback_read`。
  - legacy の governor: STEPS の domain が要る時だけ thread を作り、100 ms ごとに `sched_cpu_time` の差で最も忙しい member の率 > 80% で 1 段上げ、< 30% で 1 段下げる。要る domain が無い間は期限なしで眠る。
  - idle の統計: `kern_cpuperf_idle_cpu`（その CPU の上）、`kern_cpuperf_idle_package`（呼んだ CPU が属すればその場、他の CPU を起こさない）。
  - WS302 の口: `kern_cpuperf_cpu(cpu, flags, &info)`（flags 0 は CPU を起こさない、READ_SAMPLE は 100 ms 空けて 2 回 sample）、`kern_cpuperf_generation()`（topology・domain・能力・相対の性能・feedback の変化で進む）。
  - 熱の core（p005）の `kern_thermal_receiver_register`・`kern_thermal_event` は weak で呼ぶ（core の無い kernel では受け手を登録せず事象も出さない）。
- `src/kern/clock.c`: `kernel_cpu_notify_handler()` の EOI と数えの後・`sched_cpu_notify()` の前に `kern_cpuperf_notify(cpu)`。この file の開発の管理情報の comment 3 か所を消した（design.md §4.16）。
- `src/kern/entry.c`: `kern_cpu_notify_probe()` の後に `kern_cpuperf_init()`。
- 全 platform の `vmunix.mk` に `src/kern/cpuperf.c`（pcat・pc98 は `cpuperf.o`）。sparcv9 の `vmunix.mk` は別の commit。
- hal.h は変えていない。driver は無い（p011）。

## 確かめた事

- host 試験 `plan/ws333/tests/cpuperf-host/run.sh <出力 dir>`（`src/kern/cpuperf.c` をそのまま host の cc で `-Wall -Wextra -Werror` で compile。kernel の lock・waitq・notify・tick・thread は模擬、`shim/kern/thread.h` は host の libc とぶつかる型を避けた代わり）: 4 つの場面を別 process で、PASS（no-provider 20・describe-only 8・range 134・steps 34 の check、失敗 0）。
  - range（5330 の形: P-core 2 の SMT の組と E-core 8、P の能力 1・20・21・56、E 1・15・12・33）: domain 10、最初の strategy の前は何も書かない、start で能力を得る、各 strategy と MANUAL の端の値の request、実行時に下がった guaranteed が PERFORMANCE の min になる、上限 500‰・0‰・domain 1 つ・範囲外、直の設定（1 domain・全 domain の permille・範囲外・CUSTOM と次の strategy で消える）、答えない CPU で UNCONFIRMED と PENDING、FREQUENCY の測りで P 100・E 75、FEEDBACK の選びで E 71、poll の写しが他の CPU を起こさない、idle の統計（package は呼んだ CPU で）、READ_SAMPLE と答えない CPU の ETIMEDOUT、hardware の op が全部その CPU の上で呼ばれた事。
  - steps: PERFORMANCE の最上段、BALANCED の governor の上げ下げ、POWERSAVE・MANUAL の段、上限が guaranteed の段で止まる、root の desired で governor が止まる。
  - わざと壊す確かめ: 上限の guaranteed の床を外す・PERFORMANCE の min を lowest に・governor の上げを無効に・`pending` の印を外す・sample の能力の取り込みを外す、の 5 つの改変を scratchpad の写しで compile し、5 つとも FAIL になった。
- build: `plan/ws333/tests/build-vmunix-all.sh build/p006`（全 20 port の vmunix、各 30 分の上限）で 20 port 全部 rc=0・warning 0。条件の中の関数の呼びを外す style の直しの後にもう一度流した結果も 20 port 全部 rc=0・warning 0（cpuperf.c が amd64・pcat・sun4u・m68k-virt で compile された事を log で見た）。amd64 の vmunix に `kern_cpuperf_init` が link され、`kern_thermal_receiver_register` は weak の未定義（w）で残る事を llvm-nm で見た。

## QEMU の起動の回帰（q1759-i01、2026-10-31 P7）

`kern_cpuperf_init()` が全 port の起動の道（`src/kern/entry.c`、`boot: HAL initialized successfully` の行の直前）に入ったので、`plan/tools/boot-test.sh`（画面の PNG を読む。console・serial の log では判定していない）で確かめた。tree は branch agent/p7-ws333 の c33842bc9（p006 の 44678bac6・c33842bc9）。build の出力は `/tmp/p7-ws333/`（host の / の空きが少ないため）。

| port | build | boot-test | 見た物（画面） | PNG |
| --- | --- | --- | --- | --- |
| amd64 | `make -j24 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=/tmp/p7-ws333/amd64 disk-image`（flock、rc 0、kernel・HAL・driver の warning 0。image 全体の log の warning 477 は userland の package の物） | 既定の uefi-nvme（KVM）、`BOOT_TIMEOUT=300` → PASS | login prompt（`login:`）と init・sessiond・bluetoothd の起動の行 | `build/review/ws333-p006/amd64.png` |
| armv7-virt | `make ZEDBSD_CONFIG=config/ci/config-armv7-virt.mk … vmunix.bin`（rc 0、warning 0） | `BOOT_MODE=armv7-virt`、pattern `ARMV7 HAL READY`・`ARMV7 CPU 1 ONLINE`・`HAL initialized successfully`・`VFS initialization failed` → PASS | `boot: HAL initialized successfully. [cpu 1, memory 512MB, timer 1ms]` の後に platform の discovery、root 無しの `VFS initialization failed (12); entering idle.` | `build/review/ws333-p006/armv7.png` |
| riscv64-virt | `make ZEDBSD_CONFIG=config/ci/config-riscv64-virt.mk … vmunix`（rc 0、warning 0） | `BOOT_MODE=riscv64-virt`、pattern `RISCV64 HAL READY`・`RISCV64 SMP 4 ONLINE`・`HAL initialized successfully`・`VFS initialization failed` → PASS | 4 hart が online、`[cpu 4, memory 2048MB, timer 1ms]`、`CPUs ready: 4`、root 無しで `VFS initialization failed` | `build/review/ws333-p006/riscv64.png` |
| raspi4b（rpi4 の kernel） | `make ZEDBSD_CONFIG=config/ci/config-rpi4.mk … vmunix`（rc 0、warning 0） | `BOOT_MODE=raspi4b KERNEL=/tmp/p7-ws333/rpi4/vmunix`、SD は空の 64 MiB の file（root 無し）、pattern `ARM64 SMP cpu 3 online`・`HAL initialized successfully`・`CPUs ready: 4` → PASS | cpu 1〜3 が online、`[cpu 4, memory 960MB, timer 1ms]`、`CPUs ready: 4`、platform の discovery に進む | `build/review/ws333-p006/raspi4b.png` |

- PNG の path は worktree `/home/awe/zedBSD-worktrees/p7-ws333/` の下（git に入れない）。
- amd64 の image の build の間（flock の待ちの後）に worktree へ p007 の file（`src/drivers/platform/pcat/cpu-x86/`、`platform/amd64/vmunix.mk` の `CONFIG_CPU_PERF=y` の時だけの block）を足していた。既定の config（`CONFIG_CPU_PERF` 無し）の kernel には入らず（vmunix の symbol に `cpux86` は 0、`kern_cpuperf_init` は有る、llvm-nm で見た）、p006 の kernel と同じ。
- 未実施: 実機（5330・Pi 4 など）、他の port（sun4u・m68k・ppc・mips・x68k・pc98 など）の起動。

## 未実施

- 実機の起動の確かめ、上の 4 つ以外の port の QEMU の起動（QEMU の amd64・armv7-virt・riscv64-virt・raspi4b は q1759-i01 で PASS、上の節）。
- 本物の provider（p011）との結合、熱の core（p005）との結合（受け手の登録・poll の周期・事象）。
- 規約の全文の見直し（p024）。

## 持ち越し

- p005: 受け手の `poll` を毎秒呼ぶ。core が全 port に入ったら cpuperf.c の weak の宣言を外す。
- p011: provider は `start` で HWP を enable し能力を返す、`measure` は APERF/MPERF の差、`feedback` は package の CPU の上で bit 26 の順。段は 16 まで間引く。firmware_id は describe で。
- p013: CPU の ioctl は `kern_cpuperf_cpu`・`kern_cpuperf_domain`・`kern_cpuperf_domain_set`・`kern_cpuperf_relative_*`・`kern_cpuperf_idle_*` を使い、SET の root の判定はそこで行う。
- p014: governor の閾値と strategy の写しの値を 5330 の測りで直す。FREQUENCY の測りは他の CPU を idle に保たないので、忙しい時の値は低く出うる（推定）。
