<!-- awesome-plan project=zedbsd record=ws332-p002 -->

# ws332-p002: KERN_CLOCK_HZ の削除

Status: cleared（2026-10-30 Q1）
Parent: [WS332](../ws.md)
Queue: q1722（P3、p001 の後）

## 範囲

p001 の分類に従い、KERN_CLOCK_HZ を使う所を時間の単位の API に置き換え、include/kern/clock.h の KERN_CLOCK_HZ と HAL_TIMER_FREQUENCY の参照を消す。確かめ: 全 port の build（warning 0）、amd64・pcat・rpi4・riscv64・m68k の QEMU の起動、TCP・wifi・USB の時間に関わる host 試験。

2026-10-30 Q1 の指示で足した物: hal.h の RTC の comment の語（ユーザーの承認）、platform/x68k/vmunix.mk の FPU の検査の誤検出、wlan の UAPI の `deadline_ticks`。

## 結果（2026-10-30 P3、branch agent/p3-ws332、基点 fe1d64379）

| commit | 内容 |
| --- | --- |
| c46408c64 | 置き換えの本体（89 file）。src・include の `KERN_CLOCK_HZ`・`KERN_MS_TO_TICKS` は 0。kernel の `HAL_TIMER_FREQUENCY` は src/kern/clock.c の `static const uint32_t tick_rate` の 1 か所。clock.h は `<hal/arch.h>` を include しない。変換は clock.c の out-of-line（design.md §3）、今の率の口は `kern_clock_tick_rate()`・`kern_clock_resolution_ns()`（§4）。host 試験の stub の追従（tests/cadence-gem-host-test.h、plan/ws220/tests/tcp-model、plan/tools/latency/kd-host-test.sh） |
| 28bf15354 | 2026-10-30 ユーザーの承認を受けて hal.h の comment の語だけを直した（RTC の節の「the kernel's KERN_CLOCK_HZ is the same number」→ `kern_clock_tick_rate()`。宣言・型・約束は変えていない） |
| fd89791b0 | platform/x68k/vmunix.mk の FPU の検査（4 か所）を、68881/68882 の mnemonic の並び（変数 `X68K_FPU_INSN_PATTERN`）に当てる形に。syscall.o の switch の jump table の値 0x04c0 を objdump が `ff1 %d0` と読み、旧い `f[a-z]` の検査が落ちたため |
| b73d872e9 | i915 の host の代役（tests/contracts/host_kernel.c・tests/display/host-kernel.c）に変換の stub |
| b3b05b2be | wlan の UAPI（`deadline_ticks` → `deadline_ns`、CLOCK_MONOTONIC の ns、`WLAN_ABI_VERSION` 2。kernel の wlan.c と userland/base/wifi を同じ commit で）、plan/ws332/tests（boot.sh と config）、この記録 |

挙動: 変換の丸めの向き（待つ側は切り上げ、報告する側は切り捨て）は元の式と同じ。元の式との違いは、元が溢れていた値で飽和すること、TCP の keepalive の ms→tick が切り捨てから切り上げになったこと（1000・100 Hz では割り切れるので値は同じ）、
RLIMIT_CPU を秒で比べるようにしたこと（同値）、powervr の core の clock の log を us で出すこと、wlan の UAPI の期限の単位。

### 確かめ（P3 が流した物）

- 全 port の vmunix（BUILD=build/ws332/<port>）: amd64・pcat・pc98・intelmac・rpi4・rpi5・odroid-c5・visionfive2・riscv64-virt・riscv32-virt・armv7-virt・zybo・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・sun4u・x68k が rc=0、warning 0
  （要約 build/tmp/ws332-ports/summary.txt。x68k は fd89791b0 の後に build し直し、`x68k-target-audit` も rc=0）。wlan の変更の後は amd64 の vmunix と下の 5 port の image を build し直した。
- QEMU の起動（plan/ws332/tests/boot.sh、boot-test.sh の画面の判定。出力 build/ws332-boot、要約の最後の行 `ws332-boot: PASS`）:
  amd64（uefi-nvme、login）・pcat（bios-ide、100 Hz、login）・rpi4（raspi4b、login）・riscv64-virt（`RISCV64 HAL READY … TIMER TICK` と `VFS initialization failed`）・
  m68k-virt（100 Hz、`M68040 HAL READY … TIMER TICK` と `VFS initialization failed`）。画面の PNG は build/ws332-boot/<port>/login.png。
- host 試験: cadence-gem-host-test PASS（trace の cmp も一致）、tcp-model（plan/ws220/tests/tcp-model/run.sh）PASS、kd-host-test PASS、run-cubic-host-test PASS、
  dw-mmc（plan/tools/vf2-host/dw-mmc/run.sh）0 failure、meson-mmc の driver・version1 rc=0、i915 の run-xd-host-tests all passed、
  i915 の contracts（run.sh の trap の rm を外した複写を build/tmp/ws332-i915c で）PASS。
- Q1 が流した物: b73d872e9 で元の i915 contracts/run.sh を流し `i915 contract tests PASS: mmio dma pci rpm pte sync rps memory forget reset (ordinary + ASan/UBSan)`。
- 変換の同値: clock.c の scale_floor・scale_ceil を抜き出し、元の式と 100 Hz・1000 Hz で 0〜5,000,000 と境界（0・1・UINT64_MAX）を比べて一致（scratchpad の使い捨て、commit しない）。
- 既存の不具合で直した物: tests/cadence-gem-host-test.h の `struct net_device_ops` の stub に `set_mtu` が無く、基点でも build が失敗していた。

### 未実施

- wifi の実機・QEMU での接続（wlan の UAPI の変更は build と起動だけで確かめた。scan・connect の待ちの動きは未確認）。
- USB・wifi の時間に関わる driver の実機の試験。
- 実機（5330・Pi など）での起動。
- design.md の絞った版の design-reviewer の再レビュー。

### 消す物（Q1 へ）

- build/tmp/ws332-i915c（i915 contracts の run.sh の trap を外した複写と作業の出力）。
- build/ws332・build/ws332-boot・build/tmp/ws332-*（build の出力、image 約 2.5 GB × 3）。
- build/i386/sysroot・build/arm64/sysroot（main から複写した sysroot、image の build に使った）。
