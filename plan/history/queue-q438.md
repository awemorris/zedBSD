<!-- awesome-plan project=zedbsd record=queue-history q438 -->

# Queue q438: process の生成と終了の固定費用（ws061-p005）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q438
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「次のゴールは configure が遅い問題の解決で、USB は使わず NVMe で、Linux host と比べて許容範囲に収めるところまで自走」（前のセッション）。2026-09-25 ユーザー「はい。再開してください。」範囲は [ws061-p005](../ws061/phase005/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q438-i01 | [ws061-p005](../ws061/phase005/phase.md) | cleared（`true` 3.4 → 1.1 ms、configure（`/root`）35 → 17〜20 秒・tmpfs 29 → 13 秒（host 11.2 秒）、make（直列）34 → 20〜25 秒（host `-j1` 15.2 秒）。受け入れの `make -j4` は base の make が `-j` を無視するため未達（F-016）。回帰 boot・make 91/91（8 GiB・512 MiB）・SMP・COW・itimer・swaphog・sh 1390/1425） |

1 回目の走行（前のセッション）は host の停止（19:43、init の signal 15）で結果が失われた。

Upcoming Work Outlook: ws062-p003（amd64 の既定を native に）、WS061 の `cc t.c -o t` の計測と規約の Phase、F-015・F-016 の判断。
