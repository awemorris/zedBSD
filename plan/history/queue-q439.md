<!-- awesome-plan project=zedbsd record=queue-history q439 -->

# Queue q439: UFS の write cached を既定に（ws061-p006）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q439
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「UFSはwrite cached をデフォルトにして、write thruはマウントオプションにしてください。configureがホストとほぼ同等の性能になるまで、全般的な最適化を行なってください。」範囲は [ws061-p006](../ws061/phase006/phase.md)。この後の全般の最適化は Phase ごとに q440 以降。
Timebox: このセッション。

選んだ理由: configure（`/root`）の間の同期の書き込み 15,145 回と flush 950 回が tmpfs との差の本体（ws061-p005 の後）。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q439-i01 | [ws061-p006](../ws061/phase006/phase.md) | cleared（write cached の既定と `writethru`。configure（`/root`）12.3 秒） |

提案していた ws062-p003（既定の layout の切り替え）は Outlook に戻した（未着手）。

Upcoming Work Outlook: configure の残り（tmpfs 13 秒・host 11.2 秒。buffer cache の読み 64.5 万回・5.1 GB の複写）、ws062-p003、WS061 の `cc t.c -o t` の計測と規約の Phase、F-016（並列の make）。
