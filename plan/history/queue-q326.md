<!-- awesome-plan project=zedbsd record=queue -->

# Queue q326: 時間の単位を platform ごとの tick 周期から導く（WS040）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-23）
Active Queue: なし（q326 は完了。q325 を再開する）
Executor: メインセッション（サブエージェントは使わない）
Last Queue: q325 suspended（履歴 `plan/history/queue-q325.md`。WS040 の後に再開）
<!-- awesome-plan-current:end -->

Approval: current user「実行してください。」（2026-09-23、WS040 を q325 より先にする提案に対して）
Start UTC: 2026-09-23T09:00:00+00:00

| Order | Attempt | Phase | Status | Scope |
| --- | --- | --- | --- | --- |
| 1 | q326-i01 | [ws040-p001](../ws040/phase001/phase.md) | cleared | HAL: tick 周波数のマクロを `include/hal/arch/<arch>.h` へ。BSP の分周を定数から計算 |
| 2 | q326-i02 | [ws040-p002](../ws040/phase002/phase.md) | cleared | kernel: `KERN_CLOCK_HZ` を HAL から導き build 時に照合。ms⇔tick の共通換算 |
| 3 | q326-i03 | [ws040-p003](../ws040/phase003/phase.md) | cleared | driver: 「1 tick = 10 ms」の決め打ちの一掃 |
| 4 | q326-i04 | [ws040-p004](../ws040/phase004/phase.md) | cleared | UAPI: `times()` の単位を固定。libc の `sysconf`・`clock()` |
| 5 | q326-i05 | [ws040-p005](../ws040/phase005/phase.md) | cleared | 検証 |

p001〜p005 は依存の鎖なので、この順で1つずつ行う。

## 範囲外

q325 の残り（p018 の残り2点、p042、ws034-p003）、WS036、WS031。
HALの変更は差分ごとに事前承認。aggregate `make check` は使わない。
commitは `git commit -m WIP` のみでpushしない。

## 進捗

| Attempt | Phase | 結果 |
| --- | --- | --- |
| q326-i01 | ws040-p001 | **cleared**。承認済み HAL 差分（主＋追加2件）を適用。amd64・pcat・pc98 build |
| q326-i02 | ws040-p002 | **cleared**。`KERN_CLOCK_HZ = HAL_TIMER_FREQUENCY`、`kern_ms_to_ticks`・`kern_ticks_to_ms`（切り上げ・飽和）。i386 の `entry.c` に arch 定義が無かった件を修正 |
| q326-i03 | ws040-p003 | **cleared**。USB core・storage・UAS・xHCI・EHCI・UHCI・TCP（初期 RTO が 100 ms になっていた）・WLAN の tick の数字をミリ秒で書き直した。amd64・pcat・pc98 build、USB/WLAN host fixture 11件 PASS。rpi4 の build 失敗は HEAD からの既存問題 |
| q326-i04 | ws040-p004 | **cleared**。`times()` の単位を `KERN_PROCESS_TIMES_HZ`（100）に固定して kernel が換算。libc の `sysconf(_SC_CLK_TCK)`・`clock()` がその定数を参照。amd64 vmunix・world、pcat・pc98 vmunix |
| q326-i05 | ws040-p005 | **cleared**。host の時計で測った `sleep 5`: amd64 5.02 s、pcat 4.99 s、pc98 4.82 s（`sleep 20` = 19.81 s）。CPU 時間は実時間と一致。USB の timeout は 4.55 s〜5.85 s の間（本来の 5 s）。pc98 の PIT 入力クロックの誤り（常に 1.9968 MHz としていた）を承認のうえで直した |

## 結果

WS040 completed。kernel・driver・libc の時間は `HAL_TIMER_FREQUENCY`（arch ごと）から導かれる。
承認済みの HAL 差分は p001 の3件と p005 の pc98 PIT の1件（`plan/ws040/phase001/approval.md`）。
残った事項: i386 の LAPIC timer の経路は QEMU で通らず未実測。rpi4 は HEAD から build できない（既存問題）。
シリアル入力で1文字抜けて shell が終了する現象を3回観察した（範囲外、p005 に記録）。
