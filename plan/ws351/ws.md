<!-- awesome-plan project=zedbsd record=ws351 -->

# WS351: 可変の tick（普段 1000 Hz、idle の時 100 Hz）と HAL_TIMER_FREQUENCY の廃止

Status: planned
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
| p001 | 設計: kernel の時刻と sleep・timer を tick の数でなく `hal_rtc_read_counter()`（固定の周波数の counter）から作る形、idle の出入りで rate を変える所と条件（近くの timer の予定、tick の位相を新しい周期の倍数にそろえる x86_64 の TSC-deadline）、各 arch の HAL の実装の方針（重要な x86_64・arm64・riscv64、timer が全体で 1 つの機械）、`HAL_TIMER_FREQUENCY` の削除の範囲 | in-progress（2026-11-13 P1 q2045-i01、設計と design-reviewer 済み、[phase.md](phase001/phase.md)） | — |
| p002 | 実装と試験（host、QEMU、5330 で idle の電力と clock の分解能） | in-progress（2026-11-13 P1 q2045-i01、実装と試験済み。5330 の idle の電力は WS333 p029 に届かない（4.61 W・PC2 46 %）、原因は compositor の短い poll。[phase.md](phase002/phase.md)） | p001 |
