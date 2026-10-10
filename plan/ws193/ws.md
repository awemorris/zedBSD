<!-- awesome-plan project=zedbsd record=ws193 -->

# WS193: make menuconfig のメニュー階層の作り直しと Build boot image（進捗表示）

<!-- awesome-plan-current:start -->
Status: incomplete（p004〜p006 sourceはmain統合済み、p007のDrivers追加/選択保持を確認済み）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: [Codex Drivers選択](codex-drivers-queue.md) finished。旧arm64選択修正とq918（P1）の履歴は保持。
Target: **ベータ2**（2026-10-09 ユーザー、クリック「両方ベータ2」）
Resume point: p007の具体的成果のmain統合承認、WS全体のp003/受入照合。p001/p002は既存のcleared出力を保持、全WSの受入は再確認前。
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
| [p004](phase004/phase.md) | CPU arm64のplatform値取り違え修正 | cleared（限定source/host、93914124c main統合済み） | p001 source |
| [p005](phase005/phase.md) | CPU共通のユーザーランド選択、FFmpeg arm64設定/build | cleared（限定source/host/package build、fe4300313 main統合済み） | p004 source |
| [p006](phase006/phase.md) | Firmwareも全CPUで選択/保存/packaging可能にする | cleared（source/host、c2b97e953 main統合済み） | p005 source |
| [p007](phase007/phase.md) | Driversを指定位置へ追加、機能別階層と全CPU共通bool選択/保存 | cleared（限定source/host、main統合待ち） | p006 source |

## 2026-10-10 RPi4 UAT前のarm64選択修正

ユーザーがCPU arm64を選んでもx86_64表示が残ると報告し修正を依頼。p004と独立Queueを追加。実関数でarchitecture名をplatformに入れるとnormalizeでamd64へ戻ると再現した。p001/p002の過去のclearedは維持し、この限定バグを修正する。WSのtableに残っていたp001 plannedはphase本文の既存clearedへ投影を整えた。shared master/Queue/cache、GitHub公開はQ1。

p004 cleared。3行のplatform field統一、実関数/PTYでCPU/Board/Headerと選択indexを確認、save/load/make validation/Python syntax/diff-check PASS。[証拠](tests/arm64-selection-20261010.md)。今回の具体的commitのmain統合を最後に確認。WS全体の未完criteriaは維持、push無し。

## 2026-10-10 CPU共通のユーザーランド選択

ユーザーがarm64でFFmpegが消えると報告。「全項目を全CPUで選択可能にしたい」と回答したためp005を追加。Base/Desktop/Packagesを共通registryで選択・保存し、FFmpegのarm64設定/buildを確認する。Firmwareと直接記述専用Tests/X11は従前の対象条件を持つ。全optional packageの全CPU実行成功は今回の受入ではない。shared Master/QueueとGitHub公開はQ1へ保留。

p004のsource 93914124cはユーザーの個別承認でmain統合済み（このQueue開始時のmain HEADから確認）。前Queueの統合待ちの記録は当時の状態として保持し、現在の投影を上で訂正した。後続docs commit 308800bc2/2e48f7341は別branchに残り、この修正へ取り込んでいない。

p005 cleared: 共通registryをrootに一箇所で適用し、全6platformの255項目の選択/保存/Make実効値PASS、RPi4実メニューのFFmpeg表示PASS、FFmpeg arm64 package buildと全5 ELF検証PASS。[証拠](tests/userland-selection-20261010.md)。外部sourceの警告、全optional package/実機の未確認は証拠に明記。WS全体の受入は維持し完了扱いにはしない。mainへ具体的成果の承認を確認する。

## 2026-10-10 Firmwareの選択方針の追加訂正

current user「Firmwareもアーキテクチャに関係なくすべて選べるようにしてください。」でp006を追加。p005のFirmware条件保持という当時の方針を置換。前のp005のsource fe4300313はユーザーのmain統合指示で統合済み（このQueueの基点HEADで確認）。p005の既存結果は保持、追加訂正のscopeと結果はp006へ。全6platformでFirmware全4件の表示・選択・保存・Make実効値/配置入力とRPi4 real PTY PASS。限定規則全文review PASS。WS全体のp003/受入は未完のまま。共有Guardrail/Master/Queue投影とGitHub公開はQ1へ保留、pushなし。

## Driversの追加 / 2026-10-10指示、2026-10-11結果

userがBoot OptionとDevelopmentの間へのDrivers追加、全CPU共通選択、ECMのbuild option確認を依頼。p007を追加し、p001の当時のdriver menu削除方針を置換する。p001/p004〜p006の過去のscope/cleared出力は保持。p006 source c2b97e953はmainにある（今回基点HEADを照合）。
既存23boolを共通表示、normalizeの互換性filterを外しsave/CPU切替で保持。初期/未指定driver既定値の旧実装との一致も確認。対象host/PTY/Make条件分岐/全文review PASS。ECMはx86で既存build切替あり、arm64にはsource未接続であり移植の成功とはしない。固定driverには選択flagを追加していない。WS全体のp003/受入は未完。具体的成果commitのmain統合を確認し、shared投影/GitHubはQ1へ保留、push無し。

2026-10-11 userの追加指定でp007内の未統合UIを機能別に階層化。Disk/Input/GPU/Audio/Ethernet/WiFi/USB/Platformに既存23boolを配置。全6platformの実カテゴリhandler・全件toggle/save/load・初期既定値の一致、RPi4 real PTYのDrivers→Ethernet→ECM操作と保存を最終sourceで再確認PASS。p007 cleared、独立Queue finished。p001等の当時の結果は保持。main統合はこの最終差分の具体的commit承認待ち、共有投影/GitHub公開はQ1へ保留。
