<!-- awesome-plan project=zedbsd record=ws333-p024 -->

# ws333-p024: 規約の全文の見直しと文書

Status: in-progress（2026-11-13 P1・q2038-i01: p025〜p029・BUG-422 の分の規約の見直しと文書の続きを済ませた（下の「2回目」）。2026-11-12 P1・q2016-i01: 規約の見直しと文書は済み、build・host 試験・QEMU amd64 の回帰 PASS。依存の p005・p013・p022 の cleared と、実機（p014・p018・p020）の後の見直しが残る。cleared の判定は Q1）
Parent: [WS333](../ws.md)

## 目的

code を作る WS の最後の Phase（AGENTS.md §2）: WS333 が変えた C（`src/kern/thermal*.c`・`cpuperf.c`、`src/drivers/platform/pcat/cpu-x86/`、`src/drivers/acpi/acpi-dell*.c`・`acpi-wmi*.c`・`acpi-thermal.c`、`src/drivers/platform/rpi5/rpi5-fan.c`、`src/hal/x86_64/bsp-pcat/idle-suspend.c`・`lapic.c`・`clock.c` の WS333 の部分、`/sbin/thermal`）を `plan/coding-style.md` の全文と照らして書き方だけ直す（conformance-reviewer）。`docs/architecture/power-management.md` を書く（document-writer）。

## 依存

- p005〜p022 の実装の Phase が全部 cleared。

## 受け入れ

- 規約の見直しの結果（直した所の一覧）、全 port の vmunix の warning 0、host 試験の PASS。
- 文書（`docs/` は目標の設計を書き、`plan/` を参照しない）。
- Queue: q2016-i01（P1）

## 行った事（2026-11-12 P1、q2016-i01）

### 規約の全文の見直し

conformance-reviewer を 5 人、file の組ごとに並べて走らせた（同じ worktree、file は重ならない。commit と build は P1）。直したのは書き方だけ（動作・評価の順・error の値・log の文字列・UAPI の定義は変えない）。各組が `plan/tools/style-check.py` を前後に流し、残りを目で確かめた。

| 組 | file | 直した事（概数） |
| --- | --- | --- |
| 熱の core | `src/kern/thermal.c`・`thermal-guard.c`・`thermal-device.c`、`include/uapi/thermal.h`（comment だけ）。`include/kern/thermal.h`・`thermal_private.h` は直す所なし | 段落の comment 約 30、閉じ括弧の後の空行 約 12、条件の中の呼び出し 2、成功の return の分け 4 |
| cpuperf | `src/kern/cpuperf.c`（`include/kern/cpuperf.h` は直す所なし） | comment 約 110、空行の出し入れ 約 40、入れ子の呼び出しを名前の付いた変数に 11（`cpuperf_deadline()` を `cpuperf_post()` と同じ位置で先に計算）、3 句の条件の段組み |
| x86 の driver と HAL | `cpu-x86/cpu-x86.c`・`cpu-x86-control.c`・`cpux86-cpuid.c`・`cpux86-hwp.c`、`src/hal/x86_64/bsp-pcat/idle-suspend.c`、`lapic.c` の TSC-deadline の部分 | comment と空行 約 55、条件の中の呼び出し 2（`bsp_cpu_idle` の invariant と `choose_tick_source` の mode、short-circuit を保つ）、分けた呼び出し 6、開発の経緯の言い回し 3 |
| ACPI | `acpi-dell.c`・`acpi-dell-codec.c`・`acpi-wmi.c`・`acpi-wmi-table.c`・`acpi-thermal.c`、`rpi5-fan.c` の `record_temperature` | comment 約 65、成功の return を最後に 6（`drv_acpi_wmi_find` は loop を break して ENOENT と 0 を分けた）、入れ子の呼び出し 2、`(void)x` を `UNUSED_PARAMETER` に 7 |
| 道具と hook | `userland/base/thermal/main.c`、`init/main.c`・`monitor-zedbsd.c`・`devfs.c` の熱の部分（`entry.c`・`sleep.c`・`clock.c`・`platform/pcat.c`・`system-device.c`・`acpi-event.c`・`acpi-kern.c`・`platform.h` の熱の部分は直す所なし） | 条件の中の `strcmp` 約 25（`main` の else-if の連なりを command の名前の表と switch に。引数の要らない command に引数があれば前と同じく usage と 2）、条件演算子 5、空行と comment 約 25 |

- P1 が code の行の差（comment と空行を除く）を読み、動作が変わる所が無い事を確かめた（`devfs.c` の `thermal-inject` の比較は前に出したが `kern_strcmp` は副作用が無い、`lapic.c` の `tick_deadline` の代入は順が同じ）。
- 残した物: `/sbin/thermal` の `return refused(...)`（表示して定数を返す失敗の helper、約 25 箇所）、x86 の driver の log の引数の中の短い 2 択の条件演算子 5（規約が許す短い対称の選び）。
- 他の WS の code の既存の違反（直していない、記録だけ）: `src/kern/devfs.c` 663〜675 の他の node の `kern_strcmp` の条件、`userland/base/init/main.c` 2300〜2340 の「Handles the request condition」などと入れ子の条件演算子、`src/kern/clock.c` 207 付近の `#endif` の後の空行、`src/drivers/generic/system-device.c` 612 付近、`src/kern/entry.c` 376〜395 の条件の中の呼び出し、`clock.c` の `clock_handler` の `__atomic_exchange_n` の条件、`lapic.c` の `LAPIC_TSC_DEADLINE` の define の comment。
- 見張りの所見（欠陥ではない、直していない）: `thermal_critical_trusted` が写しの lock を持ち割り込みを止めたまま `kern_logf`（sensor ごとに 1 回）。`thermal-device.c` の注入の sensor の表は core の表と同じ大きさである事に依る。`acpi-dell.c` の `dell_probe_fans` は fan の登録が拒まれても部分の成功で進む。`cpux86_cpuid_identity` の `display_model` の 2 つの同じ枝。

### 文書

`docs/architecture/power-management.md` を今の設計で書き直した（sleep の節は保ち、processor の idle、`/dev/thermal` の枠組み（sensor・fan と boost・meter・strategy と受け手・CPU の性能・passive・非常の停止）、UAPI と事象、`/sbin/thermal`、試験の注入、build の option、検証を足した。Status の行を足し、`docs/architecture/README.md` の項を直した）。`plan/` への link と開発の管理情報は無い（grep 0）。`plan/tools/docs-check/check-docs-current.py` の power-management の FAIL（Status の行が無い）は消えた。残る FAIL 5 は他の文書と古い道具の path（`tools/menuconfig.py` ほか）で、この WS の外。

## 確かめた事

- 全 platform の vmunix（`plan/ws333/tests/build-vmunix-all.sh build/tmp/all4`）: sysroot 7 つ rc=0 warning 0。vmunix は x86_64 default・thermal・inject、intelmid64 thermal、i386・pc9821 default・thermal、rpi4・rpi5・vf2 default と nothermal の 15 が rc=0 warning 0。intelmid64 default だけ rc=2（mk の案 D、[p022](../phase022/phase.md)）。
- host 試験: `thermal-host` 29 scenario 0 failed を 3 回、`cpuperf-host` PASS、`cpu-x86-host` PASS、`cpux86/run-unit.sh all` PASS、`cpux86/check-skeleton.sh` PASS、`wmi/run.sh` PASS。`dell/run.sh` は FAIL（12 項、boost の全部）。原因は試験の道具: `plan/tools/aml-host/Makefile` 37 行が `-DCONFIG_DELL_SMM_BOOST=1` を渡すが、source と mk の名前は `CONFIG_DRIVER_DELL_SMM_BOOST`（見直しの前の HEAD の source も同じ名前で、見直しの前から落ちていた）。`MAKEFLAGS=e CPPFLAGS="-I<root>/include -I<root>/src -DCONFIG_DRIVER_DELL_SMM_BOOST=1"` で流すと `dell: PASS`（30 項）。
- QEMU amd64（`plan/ws333/tests/thermal-qemu/build-image.sh build/tmp/img4`、warning 0）: get-set・readers・group・emergency・direct の 5 本 PASS（`run.sh build/tmp/img4/hdd-image.img build/tmp/q5 <mode>`、画面の読み取り）。
- `git diff --check` は clean。

## 未実施

- 実機: MWAIT の idle と TSC-deadline の tick の書き換えた道（`bsp_cpu_idle`・`choose_tick_source`）は QEMU（KVM の下は HLT の道）では通らない。5330 の起動と p014 の測りで確かめる。
- p014・p018 の後・p020 で変わる code の見直し（Pi 5 の fan の登録と受け手は p020）。
- clang-format（host に無い）。

## Q1 への案

- `plan/tools/aml-host/Makefile` 37 行の `-DCONFIG_DELL_SMM_BOOST=1` を `-DCONFIG_DRIVER_DELL_SMM_BOOST=1` に（Makefile の変更なので案だけ。当てれば `dell/run.sh` が通る）。
- 試験の `plan/tools.md` への登録の案（下の最後の報告に書いた）。

## 2回目（2026-11-13 P1、q2038-i01、branch agent/p1-ws333k、基点 main ee74dc22ad）

前回（c9a8c83d00）の後に WS333 で足した・変えた C を、`plan/coding-style.md` の全文と照らして読んだ（`git diff c9a8c83d00 HEAD` の差分を file ごとに全部。機械の検査は `plan/tools/style-check.py` を差分の行だけに絞って流した）。直したのは書き方だけ（動作・評価の順・error の値・log の文字列・UAPI・ABI は変えない）。

対象と直した事（commit 0d048ba5d0）:

| file | 直した事 |
| --- | --- |
| `src/kern/cpuperf.c` | 閉じ括弧の後の空行と comment（mutex の初期化）、古い言い回しの comment の訂正、`cpuperf_measure_cpu` の最後を失敗と成功の return に分けた |
| `src/kern/sched.c` | `set_affinity_in_place` と `sched_set_cpu` の in-place の道の最後の return を失敗と成功に分け、critical section の中の `if` に comment |
| `src/kern/platform/pcat.c` | 条件の中の `drv_intel_tco_pci_register()` を変数 `err` に取り（log に error の値を足した） |
| `src/drivers/platform/pcat/cpu-x86/cpu-x86.c` | Boolean を式で作っていた `module_clock` を `if` に |
| `src/drivers/platform/pcat/intel-tco.c` | `(void)device` を `UNUSED_PARAMETER` に、comment の無い `if` 5、閉じ括弧の後の空行と comment 2 |
| `src/drivers/gpu/i915/device.c` | `awake_get/put`・park・unpark・start_publish・stop_gt の critical section の空行と段落の comment、comment の無い `if` 約 8、`worker_awake` の Boolean を `if` に、`i915_rps_awake_get` の直接の return を分けた |
| `src/drivers/gpu/i915/worker.c` | 条件の中の `sched_ticks()` を変数 `now` に、comment の無い `if` 2、関数の comment の折り返し |
| `src/drivers/gpu/i915/gt-power.c` | `i915_rps_move_to` の入れ子の `if` と RC6 の account の 2 段落に comment |
| `src/drivers/gpu/i915/display/present.c` | include の順（text-display を最後に）、window の run の段落の comment |
| `src/drivers/gpu/i915/tests/contracts/rps_contract_test.c` | 最初の文の comment |
| `src/drivers/platform/rpi5/rpi5-fan.c` | p020 の分（前回の p024 の後）: 閉じ括弧の後の空行と comment 2、lock の中の分岐の comment 4 |

直す所なしと読んだ file: `include/kern/sched.h`・`cpuperf.h`・`include/uapi/thermal.h`・`sysctl.h`・`boot.h`、`src/kern/boot.c`（`parameter_seconds`）、`src/drivers/gpu/i915/gt.h`・`mmio.c`・`mmio.h`・`gt-power.h`・`intel/gt-power.h`・`device.h`、`userland/base/thermal/main.c`、`userland/base/sysctl/main.c`（telemetry の列）、`include/hal/arch/amd64.h`。

出典の block（guardrail）: 足した出典は `present.c`（Procedure referenced: Linux v6.8.12 i915_driver.c）・`gt-power.c`（Procedure referenced: gen11_rc6_enable）・`intel/gt-power.h`（Values referenced・Value referenced）・`intel-tco.c`（Values referenced 5 block）で、どれも該当の block の直前に複数行の comment で書かれ、file の上部の comment に参考の一覧は無い。開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照）を足した行は無い（`git diff c9a8c83d00 HEAD -- src | grep` で 0）。

直さずに残した物（記録だけ）:
- `src/kern/sched.c` の `sched_set_cpu` の先頭の `cpu_online(target)` を含む条件（前からの形）。
- `mmio.c` の `forcewake_refusals++`・`forcewake_wakes++` の telemetry の counter に protocol の comment が無い（telemetry だけの数）。
- 他の WS の code の既存の違反は今回の差分の外で、前回の記録のまま。

文書: `docs/architecture/power-management.md` を今の形に合わせた（Status の「planned」を外し、Scope に watchdog と tick の周期（amd64 100 Hz）、「Graphics idle (i915)」の節（forcewake の参照の数・worker の 20 ms の rest・RC6 の residency・text console の止め）、performance domain（E-core は module の domain）と測りの thread の affinity（bound、3 回まで）、「Watchdog」の節（TCO、`watchdog=`・`watchdog.stop=`）、Verification の足し）。`mhz` の新しい値・共有の sample は前回の書き直しで入っていた。docs/ から plan/ への link と開発の管理情報は無い。

確かめた事（worktree `/home/awe/zedBSD-worktrees/p1-ws333k`、`build/` は worktree の物、main の build/ は未使用）:
- `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached` rc 0。
- x86_64（CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y）: `sysroot` rc 0、`build/x86_64/vmunix` rc 0 warning 0（cpuperf・sched・intel-tco・i915 の device・worker・gt-power・present を compile）、`userland/base/thermal` rc 0 warning 0。
- rpi5: `sysroot` rc 0、`build/rpi5/vmunix` rc 0 warning 0（rpi5-fan・cpuperf・sched）。
- host 試験: cpuperf-host PASS、cpu-x86-host PASS、thermal-host 30 scenarios 0 failed、thermal-tool-host PASS、tco-host PASS、rc6-host PASS、rpi5-fan-host PASS、`plan/bugs/BUG-422/sched-affinity-host.sh` 46 checks 0 failures PASS。
- `git diff --check` clean。

未実施: QEMU・実機の回帰（Q1 に頼む。i915 の critical section の整理と worker の `now` の変更は host の rc6 の試験と build だけ）、clang-format（host に無い）、他の platform の build（i386・pc9821・rpi4・vf2・intelmid64 は今回の差分が触る file の条件 compile の確認だけ未実施）、5330 の測定。
