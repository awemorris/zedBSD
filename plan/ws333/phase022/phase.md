<!-- awesome-plan project=zedbsd record=ws333-p022 -->

# ws333-p022: sysctl の道具と Keiland の monitor を `/dev/thermal` に、`hw.thermal` を外す

Status: planned（2026-11-12 P1 が design.md §9 から立てた）
Parent: [WS333](../ws.md)

## 目的

design.md §9 の p022: `userland/base/sysctl/main.c` の温度の表示と Keiland の monitor（`monitor-zedbsd.c`）を `/dev/thermal` の読みに替え（monitor は package の値だけを周期で読む）、`hw.thermal`（`HW_THERMAL 9`、`struct thermal_header`・`thermal_entry`、`src/kern/sysctl.c` の registry と thermal.c の 2 source）を外す（`HW_THERMAL 9` は欠番の comment）。

## 依存

- p005（後半の `/dev/thermal`）、p013（CPU と電力の ioctl）。
- `monitor-zedbsd.c` の所有（design.md §11.2 の 4、Keiland の WS との調整、Q1）。

## 受け入れ

- 2 つの読み手の build と表示（QEMU amd64 の画面か ssh）。
- `hw.thermal` の grep が 0（欠番の comment を除く）。全 port の vmunix の warning 0。
