<!-- awesome-plan project=zedbsd record=ws333-p022 -->

# ws333-p022: sysctl の道具と Keiland の monitor を `/dev/thermal` に、`hw.thermal` を外す

Status: in-progress（2026-11-12 P1・q2016-i01: 実装と build・host 試験・QEMU amd64 まで済み。cleared の判定は Q1）
Parent: [WS333](../ws.md)

## 目的

design.md §9 の p022: `userland/base/sysctl/main.c` の温度の表示と Keiland の monitor（`monitor-zedbsd.c`）を `/dev/thermal` の読みに替え（monitor は package の値だけを周期で読む）、`hw.thermal`（`HW_THERMAL 9`、`struct thermal_header`・`thermal_entry`、`src/kern/sysctl.c` の registry と thermal.c の 2 source）を外す（`HW_THERMAL 9` は欠番の comment）。

## 依存

- p005（後半の `/dev/thermal`）、p013（CPU と電力の ioctl）。
- `monitor-zedbsd.c` の所有（design.md §11.2 の 4、Keiland の WS との調整、Q1）。

## 受け入れ

- 2 つの読み手の build と表示（QEMU amd64 の画面か ssh）。
- `hw.thermal` の grep が 0（欠番の comment を除く）。全 port の vmunix の warning 0。
- Queue: q2016-i01（P1）

## ユーザーの決定（Q1 経由、2026-11-12）

- sysctl の `hw.thermal` は削除し、温度は `/dev/thermal` に移す。sysctl の道具に別の名前（`dev.thermal` など）で温度を出す事はしない。温度を見せるのは `/sbin/thermal` と `/dev/thermal` の読み手（Keiland の monitor）。→ 目的の「sysctl の道具の温度の表示を `/dev/thermal` の読みに替え」は「sysctl の道具から温度の表示を外す」に読み替えた。

## 作った物（2026-11-12 P1、q2016-i01）

- kernel: `src/kern/sysctl.c` から `hw.thermal` の leaf・source の表と lock・`kern_thermal_register`・`sysctl_thermal` を外した。`include/kern/sysctl.h` から `kern_thermal_read_t`・`kern_thermal_register`。`include/uapi/sysctl.h` から `HW_THERMAL`・`THERMAL_VERSION`・`THERMAL_KIND_*`・`THERMAL_HAVE_*`・`THERMAL_FLAG_CPU`・`struct thermal_header`・`struct thermal_entry` を外し、`/* CTL_HW 9 is retired (the temperatures are read from /dev/thermal); it is not reused. */` を置いた。`src/kern/thermal.c` から sysctl の 2 つの source（`thermal_sysctl_copy` と最初の sensor の表、`kern_thermal_init` の登録）と sysctl の include を外した。
- 注の直し: `include/drivers/acpi/acpi.h`（acpi-thermal の宣言の注、開発の管理情報も外した）、`src/drivers/platform/rpi5/rpi5-fan.h`・`rpi5-fan.c`（sysctl に出す → 熱の core に push）。
- `userland/base/sysctl/main.c`: `hw.thermal` の名前・`-a` の一覧・`show_thermal` を外した（温度は出さない）。
- `userland/desktop/libkeiland-backend-zedbsd/monitor-zedbsd.c`: monitor を開く時に `/dev/thermal` を読みだけで開き（開けなければ -1、閉じる時に閉じる）、sample ごとに `KERN_THERMAL_GET_SENSOR` で sensor を順に読み、読みのある sensor のうち CPU の印の物、無ければ最初の zone の温度を `cpu_milli_celsius` に入れる（前と同じ選び方）。読むのは sensor の写しだけで、CPU を起こす読み（core の idle）はしない。file の先頭と温度の関数の注から開発の管理情報（WS・Phase の番号、plan への参照）を外した。電力（meter）は monitor の sample の形（`keiland-backend.h`、Keiland の口）に CPU の電力の欄が無いので入れていない。
- 試験: `plan/ws333/tests/thermal-host/thermal-host-test.c` の `hw.thermal` の 2 つの半分の確かめを、写し（`kern_thermal_sensor_get`）の種類・印・読み・水準・名前の切り詰めの確かめに替えた。`plan/ws333/tests/thermal-qemu/run.sh` に mode `readers`、image を作る `plan/ws333/tests/thermal-qemu/build-image.sh OUTPUT` を足した。`plan/ws333/tests/build-vmunix-all.sh` の nothermal は rpi5 で `CONFIG_DRIVER_RPI5_FAN=n` も（`mk/config.mk` の Depends のとおり）。

## 確かめた事

- grep: `git grep -n "hw\.thermal\|HW_THERMAL\|thermal_entry\|thermal_header\|kern_thermal_read_t" -- . ':!plan'` が 0 件。plan の中の script では `thermal-qemu/run.sh` の readers（消えた事を確かめる行）だけ。
- host 試験 `plan/ws333/tests/thermal-host/run.sh build/tmp/host`: 29 scenario 0 failed を 3 回続けて。
- build（main の mk の直しを merge した後の tree、`plan/ws333/tests/build-vmunix-all.sh build/tmp/all3`）: sysroot は 7 platform とも rc=0 warning 0。vmunix は x86_64 default・thermal・inject、intelmid64 thermal、i386・pc9821 default・thermal、rpi4・rpi5・vf2 default、rpi4・vf2 nothermal が rc=0 warning 0。落ちたのは 2 つ: intelmid64 default（`mk/platform-intelmid64/defaults.mk` が `CONFIG_DRIVER_DELL_SMM_BOOST := y` で `CONFIG_KERN_THERMAL` は n のまま、Dell の driver が熱の core を呼ぶので link できない。mk の案 D）と rpi5 nothermal（`CONFIG_DRIVER_RPI5_FAN=y` のままの組で試した。script を直した、やり直しは p024 の build で）。
- userland: `make ... programs`（sysctl・thermal・monitor-probe ほか）warning 0。`monitor-zedbsd.c` は monitor-probe の build で compile した。
- QEMU amd64（`build-image.sh build/tmp/img3` の image、`run.sh build/tmp/img3/hdd-image.img build/tmp/q4 readers`、画面の読み取り、console・serial の log は読まない）: PASS。注入の前の monitor-probe が `probe valid=0x1f cpu_mc=0`、CPU の印の sensor を 45 °C で注入した後に `thermal sensors` が `sensor 0 cpu0 kind=zone cpu=yes temperature=45.000 ...`、monitor-probe が `probe valid=0x11f cpu_mc=45000`（温度の bit 0x100）、`sysctl hw.thermal` が `sysctl: hw.thermal: ...` で終了 1、`sysctl -a` が終了 0。画面 `build/tmp/q4/readers-after.png`（worktree、git の外）。同じ image で get-set も PASS（`build/tmp/q2`）。

## 未実施

- Keiland の System Monitor の画面（compositor の上）での温度の表示（monitor の backend を monitor-probe で確かめた。q35 は sensor が無く、実機の温度は 5330・Pi 5 の割当が要る）。
- 全 port の warning 0 の残り（intelmid64 default は mk の案 D の後、rpi5 nothermal は直した script で）。

## mk の案

- D: `mk/platform-intelmid64/defaults.mk` に `CONFIG_KERN_THERMAL := y` を足す（`CONFIG_DRIVER_DELL_SMM_BOOST := y` の依存。あるいは Dell を n に。どちらが製品の意図かは Q1）。
