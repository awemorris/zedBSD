<!-- awesome-plan project=zedbsd record=queue-history q437 -->

# Queue q437: native の image での harness・swap・性能・回帰（ws062-p002）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q437
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「オーバレイファイルシステムでこれ以上の改善が見られない場合、ディスクイメージを変更して、vmunixをEFIシステムパーティションに置いて、rootfsはUFSのパーティションにしましょう。スワップは単独パーティションにしましょう。これでかなり性能が上がると思います。」範囲は [ws062-p002](../ws062/phase002/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q437-i01 | [ws062-p002](../ws062/phase002/phase.md) | cleared（NVMe の起動の harness、4 GiB の root・swap、BUG-053 の修正、kernel heap の `kern_free` を O(1) に。configure（`/root`）の tmpfs との差の原因は UFS の write-through と flush と記録（F-015、判断待ち）。process の生成と終了の固定費用を ws061-p005 に分けた） |

Upcoming Work Outlook: ws061-p005（q438）、ws062-p003（既定の切り替え）。
