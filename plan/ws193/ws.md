<!-- awesome-plan project=zedbsd record=ws193 -->

# WS193: make menuconfig のメニュー階層の作り直しと Build boot image（進捗表示）

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q918（P1、2026-10-09）
Target: **ベータ2**（2026-10-09 ユーザー、クリック「両方ベータ2」）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー、原文）

```
make menuconfigの実装です。メニュー階層を変更します。

* toplevel
   * CPU / Board
      * CPU (x86_64, arm64)
      * Board (x86_64->UEFI+ACPI, arm64->RPi4)
   * Boot Option
      * Graphical boot (No kernel messages on boot console)
      * Graphical login (Automatically starts Keiland)
      * Mirror kernel messages to serial port
   * Development
      * Install development files (/usr/include, .so links, .pc, .a)
   * Base
      * All
      * Select
         * userland/baseの項目を選べる
   * Desktop
      * All
      * Select
         * userland/desktopの項目を選べる
   * Packages
      * userland/packages/**/*を階層的に
   * 一行空ける
   * Build boot image
   * 一行空ける
   * Exit

Build boot imageはプログレスバーを表示して、何をビルド中なのかも表示する。このメニューに限り、nprocの数だけ-jしてOKです。
```

## 目標

- toplevel を上の階層にする（CPU / Board、Boot Option、Development、Base（All / Select）、Desktop（All / Select）、Packages（userland/packages の分類の階層）、空行、Build boot image、空行、Exit）。
- Board は CPU に従う（x86_64 → UEFI+ACPI、arm64 → RPi4）。
- Base・Desktop の All は全部を選び、Select で userland/base・userland/desktop の項目を個別に選ぶ。
- Build boot image は進捗の bar と今 build している物の名前を出す。この menu に限り `-j$(nproc)`（集約の make check の禁止とは別、ユーザーの許可）。
- 既存の config.mk の形と互換（既存の変数・既定値を壊さない）。今の menuconfig の決定（2026-10-07 の OpenGL・GLX・emacs の置き場など、master の決定の記録）は新しい階層の中で保つ。

## 完了の条件

- 新しい階層で選び、保存した config.mk で image が build でき、boot-test で login prompt。
- `make menuconfig-host-test`（plan/tools/menuconfig-target-host-test.py）を新しい階層に追従させて PASS。
- 進捗の表示の画面の撮影（端末）をユーザーに見せる。
- 変えた code の規約の全文の見直し。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 今の tools/menuconfig.py の調べ、新しい階層と Build boot image の実装、host 試験 | planned | — |
| p002 | T1 の image の build と boot-test、ユーザーの確認 | planning | p001 |
| p003 | 規約の全文の見直し | planning | p001 |
