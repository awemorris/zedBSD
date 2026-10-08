<!-- awesome-plan project=zedbsd record=ws134-p009 -->

# ws134-p009: K4 kernel: ACPI の温度（と電池の出どころの確認）

Parent: [WS134](../ws.md)
Status: test-wait（T1-482、2026-10-08 夜 Q1。実機の温度は 5330 の UAT）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q912（P1、Q1 の投入「WS134 p009 ACPI の thermal と電池（kernel）。BUG-195・196 の DSDT の直しで namespace が読めるようになったか確かめ、host でできる所まで。実機は 5330 の UAT」）
設計: [design.md](../design.md) の §1.4 の K4、§1.5（温度の stub の差し替え）。

## 調べた事（2026-10-08 夜 P1）

- **namespace**: `make -C plan/ws049/tests && plan/ws049/tests/check-latitude5330.sh`（host、5330 の DSDT・SSDT1〜15）: 全 table が読め、`_REG`・`_INI`・
  `\_S5_`・`_BIF`・`_BST`・`_PSR`・`_LID` が評価でき、1349 の method が走り（既知の TSDD だけ失敗）、kernel と同じ LTO の build で stack 7184 bytes
  （10 KiB の予算の内）。BUG-195・196 の直しの後、host では namespace が読める。実機の確認は UAT。
- **電池・AC**: 既に ws132-p002 の `src/drivers/acpi/acpi-power.c`（`_BST`・`_BIX`/`_BIF`・`_PSR`、60 秒ごと＋Notify）が
  `KERN_SYSTEM_GET_POWER` で出し、WS131 p005 の backend（`power-zedbsd.c`）が `kl_system_power_v1` に載せている。design.md §1.5 の決め
  （monitor は電池を自分で読まず power の state を使う）のとおりで、K4 で電池の sysctl（`hw.acpi.battery`）は作らない（同じ値の 2 つ目の口になる）。
- **温度**: 5330 の table に ThermalZone の object は無い（`\_TZ.TZ00._TMP` は参照だけ）。温度は DPTF の参加者の Device の `_TMP`
  （`\_SB_.PC00.TCPU`（CPU、0:4.0 の processor participant）、EC の `TMEM`・`TSKN`・`NGFF`・`AMBF`、それぞれ `_PSV`・`_CRT` 付き、0.1 K の単位）。
  よって K4 の温度は「ThermalZone の `_TMP`」だけでなく「`_TMP` を持つ Device」も数える。

## 範囲と設計

- UAPI `include/uapi/sysctl.h`: `hw.thermal`（`HW_THERMAL` 9）。`struct thermal_header`（version・struct_size・element_size・count）＋
  `struct thermal_entry`（名前 = ACPI の path、kind（zone・device）、flags（CPU）、valid（温度・passive・critical）、温度・passive・critical の
  milli ℃、読んだ時刻の ns）。design の名前 `hw.acpi.thermal` から、出どころを ACPI に限らない `hw.thermal` にした（後で coretemp 等が同じ口に
  登録できる）。kernel の `kern_thermal_register`（hw.gputelemetry と同じ登録の形）。
- `src/drivers/acpi/acpi-thermal.c`（新）: namespace の ThermalZone と、`_TMP` を持つ（`_STA` が present の）Device を 16 まで取り、thread が
  5 秒ごとに `_TMP`（と `_PSV`・`_CRT`）を評価して覚える。sysctl は覚えた値を返す（sysctl の中で AML を評価しない）。名前が `TCPU`・`B0D4`
  （DPTF の processor participant）の物に CPU の印。`acpi-kern.c` が power の後に attach。
- backend `libkeiland-backend-zedbsd/monitor-zedbsd.c`: `hw.thermal` の CPU の印の物（無ければ最初の zone）を `cpu_milli_celsius` に、
  `KL_MONITOR_HAVE_TEMPERATURE`。GPU の温度は出どころが無いまま。

## 決定（2026-10-08 夜 Q1）

電池・AC は既存の経路（acpi-power.c → KERN_SYSTEM_GET_POWER → WS131 p005）、温度は acpi-thermal.c ＋ sysctl `hw.thermal`（HW_THERMAL 9、
UAPI の追加は可、HAL ではない）＋ monitor-zedbsd.c。

## 実装（2026-10-08 夜 P1）

- `include/uapi/sysctl.h`: `HW_THERMAL`、`struct thermal_header`（24 B）・`struct thermal_entry`（64 B: name[32]・kind・flags・valid・温度・
  passive・critical の milli ℃・time_ns）、`THERMAL_KIND_*`・`THERMAL_HAVE_*`・`THERMAL_FLAG_CPU`。
- `include/kern/sysctl.h`・`src/kern/sysctl.c`: `kern_thermal_register(read, context)`（source は 4 まで、1 source 16 sensor まで）、leaf
  `hw.thermal`、`sysctl_thermal`（求める長さは全 source の最大の分、返す長さは埋めた分）。
- `src/drivers/acpi/acpi-thermal.c`（新）・`acpi.h`・`acpi-kern.c`（power の後に attach）: ThermalZone と `_TMP` を持つ present の Device を 16
  まで。thread が 5 秒ごとに `_TMP`・`_PSV`・`_CRT` を lock の外で評価し（0.1 K、2000〜4732 の外は「無い」: EC が未だ読まない 0 など）、
  lock の中で覚える。sysctl は覚えた値の複写だけ。末尾の segment が `TCPU`・`B0D4` の物に `THERMAL_FLAG_CPU`。
- `userland/base/sysctl/main.c`: `sysctl hw.thermal` の表示（`hw.thermal: sensors=N`、sensor ごとの行）、`sysctl -a` の一覧に。
- `userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c`: `monitor_temperature`（CPU の印の物、無ければ最初の zone）→
  `cpu_milli_celsius`・`KL_MONITOR_HAVE_TEMPERATURE`。
- `userland/tests/monitor-probe`: SAMPLE の行に `cpu_mc=`。Makefile に `userland/base/net/netconf.c` を足した（network-link-zedbsd.c が
  `netconf_load` を使うようになっていて、probe の link が `undefined symbol: netconf_load` で通らなかった。既存の壊れ）。

## 確認（2026-10-08 夜 P1）

| コマンド | 結果 |
| --- | --- |
| `make -C plan/ws049/tests && sh plan/ws049/tests/check-latitude5330.sh` | PASS（全 table、power の method、1349 method、LTO の stack 7184 B） |
| `sh plan/ws134/tests/run-host-acpi-thermal.sh`（新、ASan+UBSan） | 81 checks PASS |
| `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat ZEDBSD_USER_PROGRAMS="monitor-probe sysctl" build/p1-uat/vmunix build/p1-uat/bin/sysctl build/p1-uat/bin/monitor-probe` | rc 0、warning 0 |
| `python3 plan/tools/style-check.py`（acpi-thermal.c・host の試験・変えた範囲） | 指摘は setjmp の慣用と sysctl/main.c の既存の比較の鎖の 1 つだけ |

未実施: QEMU（T1、`plan/ws134/tests/thermal-p009.sh`: q35 で `hw.thermal: sensors=0`、probe の sample に温度が無い）、実機 5330 の UAT
（`sysctl hw.thermal` に TCPU・TMEM・TSKN・NGFF・AMBF の温度、dmesg の `acpi: temperature sensor` の行、System Monitor の CPU の温度）。

## 確認の予定（元の案）

- host: `plan/ws134/tests/run-host-acpi-thermal.sh`（新、ws132 の host-acpi-power と同じ形の stand-in の namespace: zone・DPTF の device・
  `_STA` の absent・`_TMP` の無い device、5 秒の周期、sysctl の読み）。build warning 0（kernel・backend）、style-check。
- QEMU（T1）: q35 には zone も温度の device も無い → `sysctl hw.thermal` が count 0 で読め、monitor の CPU の温度が出ない（今と同じ）こと。
- 実機（5330 の UAT）: `sysctl hw.thermal` に TCPU 等が並び、温度が動く。System Monitor の CPU の温度。
