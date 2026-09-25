<!-- awesome-plan project=zedbsd record=queue-q378 -->

# Queue q378: rpi4 の実機起動の準備（ws044-p005）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし（次は q376 で中断した ws043-p002 の再開）
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー依頼「実機RPi4で起動する上で問題になりそうなことがあれば教えてください。なければイメージを作っていただいて、私の方で実機で起動確認してみます。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q378-i01 | [ws044-p005](../ws044/phase005/phase.md) | cleared（`kernel_address=0x80000`、EMMC2 の 32-bit access。QEMU で起動・SD の読み書き。実機はユーザー確認待ち） |
