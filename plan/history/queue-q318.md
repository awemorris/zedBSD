<!-- awesome-plan project=zedbsd record=queue-q318 -->

# Queue q318: kernelからlibcを切り出し、includeを整理する

<!-- awesome-plan-current:start -->
Status: finished
Active Queue: none（q318 finished）
Executor: root（このエージェント）。Phaseはサブエージェントで実行
Last Queue: q317 finished（履歴 `plan/history/queue-q317.md`）
<!-- awesome-plan-current:end -->

Approval: current user「そのほかは承認します。次のququeは、libcの切り出しと、includeの整理だけ進めましょう。」（2026-09-23）
Start UTC: 2026-09-22T21:16:02+00:00
運用方針: `plan/master.md`「実行体制とQueue運用方針」

| Order | Attempt | Phase | Status | Scope | 実行 |
| --- | --- | --- | --- | --- | --- |
| 1 | q318-i01 | [ws035-p034](../ws035/phase034/phase.md) | cleared | kcrt（`include/kern/kcrt.h`、`src/kern/kcrt.c`）、`src/kern/heap.c`、vmunixへのlibcのlinkをやめる | phase-runner（Opus 5.5 High） |
| 2 | q318-i02 | [ws035-p035](../ws035/phase035/phase.md) | cleared | kernel・driver・HALから接頭辞なしの標準Cヘッダの読込みを除き、uapi・kcrt・freestandingと、driverの `<libc/vulkan/vulkan.h>` だけにする。検査で保証 | phase-runner（Opus 5.5 High） |

## 並べ方

ユーザーの指示でこの2つだけを入れた。p035はp034の完了が前提なので、並行せず、p034の後に実行する。

## 時間と上限

p034は見積300〜420分、p035は360〜480分（設計§10）。build 1回1200秒、host試験120秒、QEMU smoke 120秒の起動待ち。
同条件の変更なしretryは3回まで。aggregate `make check` は使わない。amd64以外のbuildは壊れてもよい。

## 結果（Finish UTC: 2026-09-23T00:28:25+00:00）

| Attempt | Phase | 結果 | 主な成果 |
| --- | --- | --- | --- |
| q318-i01 | ws035-p034 | cleared | kcrt（`include/kern/kcrt.h`・`src/kern/kcrt.c`）と `src/kern/heap.c`、vmunixからlibcのlinkを除去。232ファイル2,662箇所を置換。libc symbol 0（compilerが要る `memcpy`・`memset` のみ） |
| q318-i02 | ws035-p035 | cleared | kernel・driver・HALから標準Cヘッダの読込みを除去。uapi 11新設・3拡張、Vulkanを `include/libc/vulkan/` へ、`-nostdlibinc` でsysroot非依存。検査（`--require-none`、link recipe内、値台帳、否定試験）PASS |

i02はサブエージェントがユーザー指示で停止したため、残りの確認（46 fixture回帰、disk-image、boot-test）をrootが
メインセッションで実行した。46件すべて基準と同じ（差異0）、boot-test PASS。

この Queue の途中で、ユーザー指示により体制を変更した: **以後サブエージェントは使わず、メインセッションが
Phaseを1つずつ実行する**（Master「実行体制とQueue運用方針」）。q318-i02が最後のサブエージェント実行である。
