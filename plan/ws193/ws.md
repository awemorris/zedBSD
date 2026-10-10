<!-- awesome-plan project=zedbsd record=ws193 -->

# WS193: make menuconfig のメニュー階層の作り直しと Build boot image（進捗表示）

<!-- awesome-plan-current:start -->
Status: incomplete（p004のarm64選択修正を実行、既存p001/p002のcleared履歴は保持）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: [Codex arm64選択修正](codex-queue.md) finished、main統合承認待ち。旧q918（P1）の履歴は保持。
Target: **ベータ2**（2026-10-09 ユーザー、クリック「両方ベータ2」）
Resume point: p004のarm64選択修正/検証済み成果のmain統合承認。p001/p002は既存のcleared出力を保持、全WSの受入は再確認前。
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

## 設計の決定（2026-10-09 ユーザー）

- 原文に無い項目（Variant の disk の形・kernel option・driver の選択・試験の hook・Noct の GPU accel）: 「menu から外す」（config.mk の直接の記述だけ、読んだ値は保つ）。
- 「Firmwareはトップレベルに階層を作る。X11とTestsはメニューから削除し、直接記述のみにする。」（Firmware の位置は Packages の次、Q1）

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
| [p001](phase001/phase.md) | 今の tools/menuconfig.py の調べ、新しい階層と Build boot image の実装、host 試験 | cleared（phase001の既存記録との投影を照合） | — |
| p002 | T1 の image の build と boot-test、ユーザーの確認 | cleared（2026-10-10 Q1: T1-497 PASS、ユーザー「menuconfigはOK」） | p001 |
| p003 | 規約の全文の見直し | planning | p001 |
| [p004](phase004/phase.md) | CPU arm64のplatform値取り違え修正 | cleared（限定source/host、main統合待ち） | p001 source |

## 2026-10-10 RPi4 UAT前のarm64選択修正

ユーザーがCPU arm64を選んでもx86_64表示が残ると報告し修正を依頼。p004と独立Queueを追加。実関数でarchitecture名をplatformに入れるとnormalizeでamd64へ戻ると再現した。p001/p002の過去のclearedは維持し、この限定バグを修正する。WSのtableに残っていたp001 plannedはphase本文の既存clearedへ投影を整えた。shared master/Queue/cache、GitHub公開はQ1。

p004 cleared。3行のplatform field統一、実関数/PTYでCPU/Board/Headerと選択indexを確認、save/load/make validation/Python syntax/diff-check PASS。[証拠](tests/arm64-selection-20261010.md)。今回の具体的commitのmain統合を最後に確認。WS全体の未完criteriaは維持、push無し。
