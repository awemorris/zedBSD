<!-- awesome-plan project=zedbsd record=queue-history q448 -->

# Queue q448: make の `-j` の設計と実装（ws064-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q448
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「makeは-jに対応させて、並列makeの実行時間もホストと同等以上にしてください。」範囲は [ws064-p001](../ws064/phase001/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q448-i01 | [ws064-p001](../ws064/phase001/phase.md) | cleared（歩きの更新、job の表、GNU 互換の jobserver、`.WAIT`・`.NOTPARALLEL`、差分試験 100/100（guest）、host の expat `-j4` 5.08 秒（GNU 5.06）、guest 7.3〜7.8 秒） |

Upcoming Work Outlook: ws064-p002（並列の性能。VM の大域の `metadata_lock`）、規約の適合（最後）。
