<!-- awesome-plan project=zedbsd record=ws040 -->

# WS040: 時間の単位を platform ごとの tick 周期から導く

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-23（q326・q327）
Primary Milestone: MG008
Related Milestones: なし
Objectives: O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

kernel・driver・libc の時間の計算を、platform ごとの tick 周期の定数から導く形にする。

## 結果

`HAL_TIMER_FREQUENCY` を arch ごと（amd64・arm64 1000 Hz、i386 100 Hz）にし、`KERN_CLOCK_HZ` をそれに揃え、ms と tick の換算を `kern_ms_to_ticks()`・`kern_ticks_to_ms()`・`KERN_MS_TO_TICKS()` に統一した。USB の timeout が本来の 5 秒に戻り、i386 の時計の 10 倍の遅れが直った。`times()` の単位は `KERN_PROCESS_TIMES_HZ`（100）に固定。pc98 の PIT の入力 clock は BIOS の系列から選ぶ（承認済み HAL 差分）。

## 制限・移管

sun4u・x68k はコードを残してサポート外。確認の道具は `plan/tools/clock/` に移した。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws040-p001 | HAL — tick 周波数を arch ごとに | cleared（q326） |
| ws040-p002 | kernel — `KERN_CLOCK_HZ` を HAL から導く | cleared（q326） |
| ws040-p003 | driver の「1 tick = 10 ms」の一掃 | cleared（q326） |
| ws040-p004 | `times()` の単位の固定と libc | cleared（q326） |
| ws040-p005 | 検証 | cleared（q326） |
| ws040-p006 | process 一覧の CPU 時間の単位（WS040 の漏れ） | cleared（q327） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws040/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。
