<!-- awesome-plan project=zedbsd record=queue-history q436 -->

# Queue q436: native の layout の image の生成と QEMU の起動（ws062-p001）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-25）
Active Queue: q436
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-25 ユーザー「…vmunixをEFIシステムパーティションに置いて、rootfsはUFSのパーティションにしましょう。スワップは単独パーティションにしましょう。」範囲は [ws062-p001](../ws062/phase001/phase.md)。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q436-i01 | [ws062-p001](../ws062/phase001/phase.md) | cleared（`zedimage-host --layout native`、検査器、`ZEDBSD_VARIANT=native`。QEMU の UEFI・USB で login prompt、root は `/dev/sda2` の UFS（rw）、swap は 1 GiB の partition、SSH の harness が通る。kernel・loader は変更なし） |

Upcoming Work Outlook: ws062-p002（性能と回帰）、ws062-p003（既定の切り替え）。
