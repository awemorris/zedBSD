<!-- awesome-plan project=zedbsd record=ws332 -->

# WS332: tick の周波数を compile の時の定数でなく API で扱い、可変の tick にする

Status: incomplete
Primary Milestone: MG004
Parent: [Master](../master.md)
目標: obsolete の `KERN_CLOCK_HZ`（中身は HAL の `HAL_TIMER_FREQUENCY`）を kernel・driver から無くし、時間は単位（ms・ns）の API で扱う。今の tick の周波数を走っている時に取得する口を用意する（周波数を変える実装は将来）。
Resume point: p001・p002 cleared（2026-10-30）。次は p003（率を仮定する残りの literal）と p004 規約。

## ユーザーの決定（要点）

- 2026-10-30 ユーザーから、HAL_TIMER_FREQUENCY を POSIX の公開の macro でないなら API で取る形にし、client の computer では clock を可変にする（ゲームモード 1000 Hz、一般のデスクトップ 100 Hz、待機 10 Hz など）のが現代的だ、という趣旨の指示を受けた。
- 2026-10-30 ユーザーから、sysconf(_SC_CLK_TCK) の導入で KERN_CLOCK_HZ は obsolete になっており、主に試験していた platform 以外での動作が怪しかったので削除を遅らせていた経緯があり、今回削除する、という趣旨の指示を受けた。WS332 は Q1 の提案どおり。
- 2026-10-30 ユーザーから、可変の tick は今の周波数を取得する口だけ用意すればよく、実際に変える実装は今回は不要、という趣旨の指示を受けた。→ 範囲は KERN_CLOCK_HZ の削除と取得の口まで。周期を変える実装は将来（Future Work）。
- Q1 の調べ: POSIX に kernel の tick の周波数の compile の時の macro は無い（CLOCKS_PER_SEC は clock() の単位で XSI は 1,000,000 固定、times() の単位は sysconf(_SC_CLK_TCK)、CLK_TCK は廃止）。KERN_CLOCK_HZ の利用は kernel と driver に 175 か所（2026-10-30 の grep）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: 時間の API（tick の数でなく ms・ns の期限）、可変の tick（周期を変える口、mode と電源管理 WS052・WS301・WS306 との関係）、各 arch の HAL の timer の実装の補完（hal.h を変えずに済むか） | cleared（2026-10-30 Q1） | — |
| [p002](phase002/phase.md) | KERN_CLOCK_HZ の削除: 175 か所を時間の単位の API に置き換え、clock.h から HAL_TIMER_FREQUENCY を外す。全 port の build と、主な port の QEMU の起動 | cleared（2026-10-30 Q1） | p001 |
| p003 | 率を仮定する残り（tick の数の literal: VM_FREE_WAIT_TICKS・rpi4-vchiq・i915 の WAIT・EVADE、「次の tick まで」の +1 の約 8 か所）を時間の単位の API に（design.md §2・§5） | planned | p002 |
| p004 | 規約の全文の見直し | — | p003 |
