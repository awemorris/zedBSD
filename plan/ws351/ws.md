<!-- awesome-plan project=zedbsd record=ws351 -->

# WS351: 可変の tick（普段 1000 Hz、idle の時 100 Hz）と HAL_TIMER_FREQUENCY の廃止

Status: completed（2026-11-15 Q1。可変の tick（走っている間 1000 Hz・idle の間 100 Hz）と時刻の counter 化、HAL_TIMER_FREQUENCY の廃止。arm64 は Pi 5 の main の kernel（2026-11-14 の更新、UAT と 60 fps の試験）で動く。riscv64 の起動は次に VF2 を main に更新する時に確かめる）
Primary Milestone: MG004
Parent: [Master](../master.md)
Owner: Q1

## 目標

tick の周期を走っている間に CPU ごとに変える。普段は 1000 Hz、CPU が idle の時は 100 Hz。[WS332](../ws332/ws.md) で時間の API と今の周波数を取る口は作ったが、周期を実際に変える実装は将来に回していた。今は amd64 の `HAL_TIMER_FREQUENCY` が 100（2026-11-13 の Q1 の誤解による固定、d2c40ed07f）で、clock_gettime が 10 ms 刻み・ms の待ちが 10 ms に丸まる害が出ている。

## ユーザーの決定（要点）

- 2026-11-13 ユーザーから、tick は可変（普段 1000 Hz、idle で 100 Hz）が前提で、HAL も kernel も `HAL_TIMER_FREQUENCY` を参照してはいけない、という趣旨の指示を受けた。
- 2026-11-13 ユーザーが HAL の API を決めた（`include/hal/hal.h` の RTC の節）: `hal_rtc_get_tick_hz()`（呼んだ CPU の今の tick の rate）、`hal_rtc_set_tick_hz(hz)`（呼んだ CPU の tick の rate を次の timer の割り込みから近い値に、戻り値は HAL が実際に出す rate）。`HAL_TIMER_FREQUENCY` の説明と `#error` を hal.h から消した。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| p001 | 設計: kernel の時刻と sleep・timer を tick の数でなく `hal_rtc_read_counter()`（固定の周波数の counter）から作る形、idle の出入りで rate を変える所と条件（近くの timer の予定、tick の位相を新しい周期の倍数にそろえる x86_64 の TSC-deadline）、各 arch の HAL の実装の方針（重要な x86_64・arm64・riscv64、timer が全体で 1 つの機械）、`HAL_TIMER_FREQUENCY` の削除の範囲 | cleared（2026-11-15 Q1。設計と design-reviewer 済み、実装は main に merge 済み） | — |
| p002 | 実装と試験（host、QEMU、5330 で idle の電力と clock の分解能） | cleared（2026-11-15 Q1。実装と試験は main。5330 の idle の電力の受け入れは WS352・WS353 p016 に移した（4.54 W・PC2 66〜74 %、WS352 fix4 と同等）。arm64・riscv64・i386・pc9821 の起動は p003 で QEMU） | p001 |
| p003 | 規約の全文の見直し（AGENTS §2） | cleared（2026-11-15 Q1、merge f8a5a58806。WS351 の行の規約の違反 20→0、5 機種の vmunix warning 0、clock-host 42・itimer-host 10 PASS、QEMU x86_64 の TSC-deadline・periodic で idle 1 10 10 10・busy 1 1 1 1） | p002 |

## Resume point

- 2026-11-15 Q1: completed。QEMU の arm64・riscv64 は今の build に virt の機種が無く流せなかった。arm64 は Pi 5（2026-11-14 に main の tip に更新、WS351 の後）で動いている事を証拠とした。riscv64（VF2）の起動は、次に VF2 を main に更新する担当が確かめる。Phase の directory は消した（試験は tests/ に残す）。

- 2026-11-15 Q1: p001・p002 cleared。残りは p003（規約の全文の見直し、5 機種の build warning 0、QEMU の arm64・riscv64 の起動と tick_divisor）。
